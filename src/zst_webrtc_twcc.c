/*=============================================================================
    zst_webrtc_twcc.c — Transport-Wide Congestion Control (TWCC / GCC)

    Implements RFC 8888 RTCP CCFB parsing and Google Congestion Control (GCC):
    - Outgoing RTP: injects transport-wide-cc-02 header extension
    - Incoming RTCP: parses CCFB feedback (loss + arrival deltas)
    - Delay-based estimator: AIMD with Kalman-filter overuse detector
    - Loss-based estimator: threshold-based rate adjustment
    - Combined GCC: min(delay_estimate, loss_estimate)
    - Emits ZST_EVENT_WEBRTC_REMB when target bitrate changes
=============================================================================*/
#include "zstreamer/elements/zst_webrtc_twcc.h"
#include "zst_log.h"
#include <rtc/rtc.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

/* ── Constants ─────────────────────────────────────────────────────────── */
#define TWCC_HISTORY_SIZE    8192
#define TWCC_MIN_BITRATE     100000      /* 100 kbps */
#define TWCC_MAX_BITRATE     8000000     /* 8 Mbps */
#define TWCC_INIT_BITRATE    2000000     /* 2 Mbps */
#define TWCC_UPDATE_INTERVAL_US 100000  /* 100ms GCC update interval */

/* GCC delay-based estimator constants (from WebRTC reference implementation) */
#define GCC_GAMMA          0.9f    /* Exponential moving average factor for delay gradient */
#define GCC_OVERUSE_TH     12.5f  /* ms — overuse threshold for Kalman filter */
#define GCC_BETA           0.85f  /* Multiplicative decrease factor on overuse */
#define GCC_ALPHA_INC      0.08f  /* Additive increase fraction per interval */

/* ── Types ──────────────────────────────────────────────────────────────── */
typedef enum {
    TWCC_STATE_NORMAL,
    TWCC_STATE_UNDERUSE,
    TWCC_STATE_OVERUSE,
} twcc_bw_state_t;

typedef struct {
    uint16_t seq_num;
    uint64_t send_time_us;
    size_t   packet_size;
} twcc_sent_packet_t;

struct zst_webrtc_twcc {
    int           pc_id;
    zst_bus_t*    bus;
    int           extmap_id;
    uint16_t      seq_num;
    pthread_mutex_t lock;

    /* Sent packet history (ring buffer keyed by seq % TWCC_HISTORY_SIZE) */
    twcc_sent_packet_t history[TWCC_HISTORY_SIZE];

    /* ── Delay-based estimator state ── */
    /* Kalman filter / gradient detector */
    float         delay_gradient;      /* Filtered inter-packet delay gradient (ms) */
    float         delay_gradient_var;  /* Variance estimate */
    float         delay_threshold;     /* Adaptive overuse threshold (ms) */
    twcc_bw_state_t bw_state;

    /* Previous feedback reference (for delta computation) */
    uint64_t      last_recv_time_us;   /* Reconstructed receiver-side time of last reported pkt */
    uint64_t      last_send_time_us;   /* Sender side time of last reported pkt */
    int           have_prev_ref;

    /* ── Loss-based estimator state ── */
    float         loss_rate;           /* Smoothed loss fraction [0..1] */

    /* ── Combined GCC output ── */
    uint64_t      delay_estimate_bps;  /* Delay-based target */
    uint64_t      loss_estimate_bps;   /* Loss-based target */
    uint64_t      current_bitrate_bps; /* Combined GCC output = min(delay, loss) */
    uint64_t      last_update_us;      /* Timestamp of last bitrate event posted */
    uint64_t      last_emitted_bps;    /* Last bitrate posted to bus (avoid spam) */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static uint64_t twcc_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t twcc_clamp_bps(uint64_t bps) {
    if (bps < TWCC_MIN_BITRATE) return TWCC_MIN_BITRATE;
    if (bps > TWCC_MAX_BITRATE) return TWCC_MAX_BITRATE;
    return bps;
}

/* ── Delay-based AIMD estimator ─────────────────────────────────────────── */
/*
 * Process one inter-packet delay delta (ms). This implements a simplified
 * version of the WebRTC GCC overuse detector using exponential smoothing.
 *
 * delta_delay_ms: arrival_delta_ms - send_delta_ms
 *   > 0 → queuing build-up (congestion)
 *   < 0 → queue draining
 *   ≈ 0 → normal
 */
static void gcc_delay_update(zst_webrtc_twcc_t* twcc, float delta_delay_ms) {
    /* Exponential smoothing of the delay gradient */
    twcc->delay_gradient = GCC_GAMMA * twcc->delay_gradient
                         + (1.0f - GCC_GAMMA) * delta_delay_ms;

    /* Adaptive threshold (slowly tracks |gradient|) */
    float abs_grad = fabsf(twcc->delay_gradient);
    float alpha = 0.01f; /* threshold tracking speed */
    if (abs_grad > twcc->delay_threshold) {
        /* Track up fast */
        twcc->delay_threshold += alpha * (abs_grad - twcc->delay_threshold);
    } else {
        /* Decay slowly when not overusing */
        twcc->delay_threshold -= alpha * 0.1f * twcc->delay_threshold;
    }
    if (twcc->delay_threshold < 6.0f)  twcc->delay_threshold = 6.0f;
    if (twcc->delay_threshold > 600.0f) twcc->delay_threshold = 600.0f;

    /* Classify signal */
    twcc_bw_state_t prev_state = twcc->bw_state;
    if (twcc->delay_gradient > twcc->delay_threshold) {
        twcc->bw_state = TWCC_STATE_OVERUSE;
    } else if (twcc->delay_gradient < -twcc->delay_threshold) {
        twcc->bw_state = TWCC_STATE_UNDERUSE;
    } else {
        twcc->bw_state = TWCC_STATE_NORMAL;
    }
    if (twcc->bw_state != prev_state) {
        ZST_LOG_DEBUG("twcc", "BW state: %s → %s (grad=%.2f threshold=%.2f)",
            prev_state == TWCC_STATE_OVERUSE ? "OVERUSE" :
            prev_state == TWCC_STATE_UNDERUSE ? "UNDERUSE" : "NORMAL",
            twcc->bw_state == TWCC_STATE_OVERUSE ? "OVERUSE" :
            twcc->bw_state == TWCC_STATE_UNDERUSE ? "UNDERUSE" : "NORMAL",
            twcc->delay_gradient, twcc->delay_threshold);
    }
}

static void gcc_delay_adapt(zst_webrtc_twcc_t* twcc) {
    switch (twcc->bw_state) {
    case TWCC_STATE_OVERUSE:
        /* Multiplicative decrease */
        twcc->delay_estimate_bps = (uint64_t)(twcc->delay_estimate_bps * GCC_BETA);
        break;
    case TWCC_STATE_NORMAL:
        /* Additive increase */
        twcc->delay_estimate_bps += (uint64_t)(twcc->delay_estimate_bps * GCC_ALPHA_INC);
        break;
    case TWCC_STATE_UNDERUSE:
        /* Hold — don't increase further, let queue drain */
        break;
    }
    twcc->delay_estimate_bps = twcc_clamp_bps(twcc->delay_estimate_bps);
}

/* ── Loss-based estimator ───────────────────────────────────────────────── */
static void gcc_loss_adapt(zst_webrtc_twcc_t* twcc, float loss_rate) {
    /* Smooth the loss rate */
    twcc->loss_rate = 0.5f * twcc->loss_rate + 0.5f * loss_rate;

    if (twcc->loss_rate > 0.10f) {
        /* High loss (>10%) — decrease 20% */
        twcc->loss_estimate_bps = (uint64_t)(twcc->loss_estimate_bps * 0.80f);
    } else if (twcc->loss_rate > 0.02f) {
        /* Medium loss (2–10%) — hold */
    } else {
        /* Low loss (<2%) — gentle increase 5% */
        twcc->loss_estimate_bps = (uint64_t)(twcc->loss_estimate_bps * 1.05f);
    }
    twcc->loss_estimate_bps = twcc_clamp_bps(twcc->loss_estimate_bps);
}

/* ── RTCP CCFB parser ────────────────────────────────────────────────────── */
/*
 * RTCP Transport Feedback (RFC 8888) layout starting at byte 8 (after SSRC):
 *   Bytes  8–9:  base sequence number
 *   Bytes 10–11: packet status count
 *   Bytes 12–14: reference time (24 bits, unit = 250ms/64 = ~3.906ms)
 *   Byte  15:    feedback packet count
 *   Bytes 16+:   status chunks (2 bytes each)
 *   After status chunks: recv delta vector (variable length)
 */
static void twcc_parse_ccfb(zst_webrtc_twcc_t* twcc, const uint8_t* msg, int size) {
    if (size < 20) return;

    uint32_t sender_ssrc  = ntohl(*(uint32_t*)(msg + 4));
    uint32_t media_ssrc   = ntohl(*(uint32_t*)(msg + 8));
    uint16_t base_seq     = ntohs(*(uint16_t*)(msg + 12));
    uint16_t status_count = ntohs(*(uint16_t*)(msg + 14));
    /* ref_time is 24 bits, unit = 2^6 ms = 64ms */
    uint32_t ref_time_64ms = ((uint32_t)msg[16] << 16) | ((uint32_t)msg[17] << 8) | msg[18];
    /* Convert to microseconds: ref_time_64ms * 64 * 1000 */
    uint64_t recv_ref_us = (uint64_t)ref_time_64ms * 64000ULL;

    (void)sender_ssrc; (void)media_ssrc;

    if (status_count == 0) return;

    /* --- Parse status chunks to collect recv/lost per packet --- */
    /* We'll also collect recv deltas (250µs units) to build arrival times */
    uint8_t statuses[1024]; /* 0=not recv, 1=recv (small delta), 2=recv (large delta) */
    int n_statuses = 0;

    int offset = 20;
    uint16_t pkts_done = 0;

    while (offset + 2 <= size && pkts_done < status_count && n_statuses < 1024) {
        uint16_t chunk = ntohs(*(uint16_t*)(msg + offset));
        offset += 2;

        if ((chunk & 0x8000) == 0) {
            /* Run-Length Encoding chunk */
            uint8_t  st  = (chunk >> 13) & 0x03;
            uint16_t run = chunk & 0x1FFF;
            for (uint16_t r = 0; r < run && pkts_done < status_count && n_statuses < 1024; r++) {
                statuses[n_statuses++] = st;
                pkts_done++;
            }
        } else {
            /* Status Vector chunk */
            int sym_size = (chunk & 0x4000) ? 2 : 1;
            int num_sym  = (sym_size == 1) ? 14 : 7;
            for (int i = 0; i < num_sym && pkts_done < status_count && n_statuses < 1024; i++) {
                int shift = (sym_size == 1) ? (13 - i) : (12 - i * 2);
                uint8_t st = (chunk >> shift) & ((1 << sym_size) - 1);
                statuses[n_statuses++] = st;
                pkts_done++;
            }
        }
    }

    /* --- Parse recv delta vector --- */
    /* Each received packet (status != 0) contributes one delta entry */
    int lost_count = 0, recv_count = 0;
    uint64_t recv_time_us = recv_ref_us; /* running receiver-side arrival time */

    /* Collect per-packet delay deltas for AIMD */
    float delay_deltas_ms[64];
    int   n_delay_deltas = 0;

    for (int i = 0; i < n_statuses; i++) {
        uint16_t seq = base_seq + (uint16_t)i;
        if (statuses[i] == 0) {
            lost_count++;
            continue;
        }
        recv_count++;

        /* Read delta */
        int32_t delta_250us = 0;
        if (statuses[i] == 1) {
            /* Small delta: 1 byte, unit = 250µs, range 0..255 */
            if (offset + 1 > size) break;
            delta_250us = (int32_t)(uint8_t)msg[offset++];
        } else {
            /* Large delta: 2 bytes signed, unit = 250µs */
            if (offset + 2 > size) break;
            delta_250us = (int32_t)(int16_t)ntohs(*(uint16_t*)(msg + offset));
            offset += 2;
        }
        recv_time_us += (uint64_t)delta_250us * 250ULL;

        /* Look up sender-side send time from history */
        int hist_idx = seq % TWCC_HISTORY_SIZE;
        pthread_mutex_lock(&twcc->lock);
        uint64_t send_time_us = twcc->history[hist_idx].send_time_us;
        int have_hist = (twcc->history[hist_idx].seq_num == seq && send_time_us != 0);
        pthread_mutex_unlock(&twcc->lock);

        if (have_hist && twcc->have_prev_ref) {
            /* Compute inter-packet delay delta in ms:
             *   Δdelay = (arrival_i - arrival_{i-1}) - (send_i - send_{i-1})
             */
            int64_t send_delta_us = (int64_t)send_time_us - (int64_t)twcc->last_send_time_us;
            int64_t recv_delta_us = (int64_t)recv_time_us - (int64_t)twcc->last_recv_time_us;
            float delta_delay_ms = (float)(recv_delta_us - send_delta_us) / 1000.0f;

            if (n_delay_deltas < 64) {
                delay_deltas_ms[n_delay_deltas++] = delta_delay_ms;
            }
        }
        if (have_hist) {
            twcc->last_send_time_us = send_time_us;
            twcc->last_recv_time_us = recv_time_us;
            twcc->have_prev_ref = 1;
        }
    }

    /* --- Update delay-based estimator --- */
    for (int i = 0; i < n_delay_deltas; i++) {
        gcc_delay_update(twcc, delay_deltas_ms[i]);
    }

    /* --- Throttle combined GCC update to TWCC_UPDATE_INTERVAL_US --- */
    uint64_t now_us = twcc_get_time_us();
    if (now_us - twcc->last_update_us < TWCC_UPDATE_INTERVAL_US) return;
    twcc->last_update_us = now_us;

    /* --- Apply loss-based estimator --- */
    int total = lost_count + recv_count;
    float lr = (total > 0) ? (float)lost_count / (float)total : 0.0f;
    gcc_loss_adapt(twcc, lr);

    /* --- Apply delay-based estimator --- */
    gcc_delay_adapt(twcc);

    /* --- Combined GCC: min(delay_estimate, loss_estimate) --- */
    uint64_t combined = twcc->delay_estimate_bps < twcc->loss_estimate_bps
                      ? twcc->delay_estimate_bps : twcc->loss_estimate_bps;
    twcc->current_bitrate_bps = combined;

    /* --- Post event if changed significantly (> 3%) --- */
    uint64_t prev = twcc->last_emitted_bps;
    uint64_t diff = combined > prev ? combined - prev : prev - combined;
    if (prev == 0 || diff * 100 / prev > 3) {
        ZST_LOG_INFO("twcc",
            "GCC: delay_est=%lu loss_est=%lu combined=%lu bps "
            "[loss=%.1f%% delay_grad=%.2f state=%s]",
            twcc->delay_estimate_bps, twcc->loss_estimate_bps, combined,
            twcc->loss_rate * 100.0f, twcc->delay_gradient,
            twcc->bw_state == TWCC_STATE_OVERUSE ? "OVERUSE" :
            twcc->bw_state == TWCC_STATE_UNDERUSE ? "UNDERUSE" : "NORMAL");

        zst_event_t* ev = zst_event_new_webrtc_remb(NULL, twcc->pc_id, (unsigned int)combined);
        if (ev) zst_bus_post(twcc->bus, ev);
        twcc->last_emitted_bps = combined;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void* zst_webrtc_twcc_process_incoming(zst_webrtc_twcc_t* twcc, const char* message, int size) {
    if (!twcc || size < 4) return (void*)message;

    const uint8_t* buf = (const uint8_t*)message;
    uint8_t pt = buf[1];

    /* RTCP payload types: 192–223 (RFC 5761 mux: discriminate by PT range and version) */
    if (pt >= 192 && pt <= 223) {
        if (pt == 205) { /* RTPFB */
            uint8_t fmt = buf[0] & 0x1F;
            if (fmt == 15) { /* Transport-Wide Feedback (TWCC CCFB) */
                twcc_parse_ccfb(twcc, buf, size);
            }
        }
    }
    return (void*)message;
}

void* zst_webrtc_twcc_process_outgoing(zst_webrtc_twcc_t* twcc, const char* message, int size) {
    if (!twcc || twcc->extmap_id < 0) return (void*)message;
    if (size < 12) return (void*)message;

    const uint8_t* in = (const uint8_t*)message;
    uint8_t version = (in[0] >> 6) & 0x03;
    if (version != 2) return (void*)message;

    uint8_t pt = in[1] & 0x7F;
    /* RTCP range: filter out */
    if (pt >= 64 && pt <= 95) return (void*)message;

    uint8_t x  = (in[0] >> 4) & 0x01;
    uint8_t cc = in[0] & 0x0F;
    int csrc_len = cc * 4;
    if (size < 12 + csrc_len) return (void*)message;

    /* We only handle the no-extension case; if extensions already present, pass through */
    if (x != 0) return (void*)message;

    if (size + 8 > 2048) return (void*)message;

    /* Allocate and stamp sequence number */
    pthread_mutex_lock(&twcc->lock);
    uint16_t my_seq = twcc->seq_num++;
    int idx = my_seq % TWCC_HISTORY_SIZE;
    twcc->history[idx].seq_num      = my_seq;
    twcc->history[idx].send_time_us = twcc_get_time_us();
    twcc->history[idx].packet_size  = (size_t)size;
    pthread_mutex_unlock(&twcc->lock);

    /* Build new packet with injected header extension */
    uint8_t new_pkt[2048];
    int hdr_len = 12 + csrc_len;
    memcpy(new_pkt, in, hdr_len);
    new_pkt[0] |= 0x10; /* Set X=1 */

    int ext_off = hdr_len;
    /* 0xBEDE magic + length=1 word */
    new_pkt[ext_off+0] = 0xBE;
    new_pkt[ext_off+1] = 0xDE;
    new_pkt[ext_off+2] = 0x00;
    new_pkt[ext_off+3] = 0x01;
    /* One-byte header: ID (4 bits) | len-1 (4 bits). TWCC data=2 bytes → len-1=1 */
    new_pkt[ext_off+4] = (uint8_t)((twcc->extmap_id << 4) | 0x01);
    new_pkt[ext_off+5] = (my_seq >> 8) & 0xFF;
    new_pkt[ext_off+6] = my_seq & 0xFF;
    new_pkt[ext_off+7] = 0x00; /* padding */

    memcpy(new_pkt + ext_off + 8, in + hdr_len, size - hdr_len);
    return rtcCreateOpaqueMessage(new_pkt, size + 8);
}

zst_webrtc_twcc_t* zst_webrtc_twcc_create(int pc_id, zst_bus_t* bus) {
    zst_webrtc_twcc_t* twcc = calloc(1, sizeof(zst_webrtc_twcc_t));
    if (!twcc) return NULL;
    twcc->pc_id               = pc_id;
    twcc->bus                 = bus;
    twcc->extmap_id           = -1;
    twcc->seq_num             = 1;
    twcc->current_bitrate_bps = TWCC_INIT_BITRATE;
    twcc->delay_estimate_bps  = TWCC_INIT_BITRATE;
    twcc->loss_estimate_bps   = TWCC_INIT_BITRATE;
    twcc->delay_threshold     = GCC_OVERUSE_TH;
    twcc->bw_state            = TWCC_STATE_NORMAL;
    twcc->last_update_us      = twcc_get_time_us();
    pthread_mutex_init(&twcc->lock, NULL);
    return twcc;
}

void zst_webrtc_twcc_destroy(zst_webrtc_twcc_t* twcc) {
    if (!twcc) return;
    pthread_mutex_destroy(&twcc->lock);
    free(twcc);
}

int zst_webrtc_twcc_parse_offer(zst_webrtc_twcc_t* twcc, const char* offer_sdp) {
    const char* ext_uri = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
    const char* p = strstr(offer_sdp, ext_uri);
    if (!p) { twcc->extmap_id = -1; return -1; }

    const char* start = p;
    while (start > offer_sdp && *start != '\n') start--;
    if (*start == '\n') start++;

    const char* extmap_str = "a=extmap:";
    if (strncmp(start, extmap_str, strlen(extmap_str)) == 0) {
        int id = atoi(start + strlen(extmap_str));
        twcc->extmap_id = id;
        ZST_LOG_INFO("twcc", "TWCC extmap ID from offer: %d", id);
        return id;
    }
    return -1;
}

int zst_webrtc_twcc_inject_answer(zst_webrtc_twcc_t* twcc, char* answer_sdp, size_t max_len) {
    if (twcc->extmap_id < 0) return 0;

    char inj_buf[256];
    snprintf(inj_buf, sizeof(inj_buf),
        "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n",
        twcc->extmap_id);

    /* Inject after every c=IN line (one per m= section) */
    size_t inj_len = strlen(inj_buf);
    size_t sdp_len = strlen(answer_sdp);
    if (sdp_len + inj_len + 1 > max_len) return -1;

    char* temp = malloc(max_len);
    if (!temp) return -1;

    size_t temp_len = 0;
    const char* p = answer_sdp;

    /* Bolt Optimization:
     * Avoid O(N^2) strncat(..., max_len - strlen(temp) - 1) and multiple memory allocations (strdup, strtok_r).
     * We scan the original SDP string line by line using strchr and memcpy to build the output buffer.
     * We also skip leading \r and \n to perfectly emulate strtok_r("\r\n") dropping empty lines,
     * which prevents libdatachannel from crashing on trailing empty lines during ICE restart.
     */
    while (*p) {
        // Skip leading \r and \n to emulate strtok_r("\r\n") dropping empty lines
        while (*p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        const char* eol = strchr(p, '\n');
        size_t raw_len = eol ? (size_t)(eol - p) : strlen(p);

        size_t line_len = raw_len;
        if (line_len > 0 && p[line_len - 1] == '\r') {
            line_len--;
        }

        if (temp_len + line_len + 2 >= max_len) {
            free(temp);
            return -1;
        }

        memcpy(temp + temp_len, p, line_len);
        temp_len += line_len;
        temp[temp_len++] = '\r';
        temp[temp_len++] = '\n';

        if (line_len >= 5 && strncmp(p, "c=IN ", 5) == 0) {
            if (temp_len + inj_len >= max_len) {
                free(temp);
                return -1;
            }
            memcpy(temp + temp_len, inj_buf, inj_len);
            temp_len += inj_len;
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (temp_len >= max_len) { free(temp); return -1; }
    temp[temp_len] = '\0';
    strcpy(answer_sdp, temp);
    free(temp);
    return 0;
}
