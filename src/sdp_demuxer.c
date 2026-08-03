/*=============================================================================
    sdp_demuxer.c — SDP/RTP Demuxer with DPLL-based perfect timing

    Parses SDP descriptions to discover media tracks.  Receives raw RTP
    packets on its "sink" pad, reorders them by RTP sequence number using
    a per-SSRC ring buffer, depacketizes (H.264 RFC 3984, H.265, AAC
    RFC 3640), and outputs ordered, precisely-timed media buffers on
    dynamically-created source pads.

    Timing uses a DPLL (digital phase-locked loop) — PI controller on
    jitter buffer depth — that adaptively compensates for sender/receiver
    clock drift, ensuring "perfect timing" even across asynchronous
    oscillator domains.

    References:
      - RFC 3550 (RTP/RTCP)
      - RFC 3984 (H.264 over RTP)
      - RFC 3640 (AAC over RTP)
      - RFC 7798 (H.265 over RTP)
      - avclksync DPLL algorithm
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_clock.h"
#include "zstreamer/elements/zst_sdp_demuxer.h"

/*===========================================================================
    Constants
===========================================================================*/
#define SDP_DEMUXER_MAX_TRACKS        8
#define SDP_DEMUXER_MAX_PT_PER_TRACK  16
#define SDP_DEMUXER_MAX_LINE          1024
#define SDP_DEMUXER_DEFAULT_REORDER   256
#define SDP_DEMUXER_DEFAULT_JITTER_MS 200
#define SDP_DEMUXER_DEFAULT_LATENESS_MS 500
#define SDP_DEMUXER_DEFAULT_VIDEO_CLOCK 90000
#define SDP_DEMUXER_DEFAULT_AUDIO_CLOCK 48000
#define SDP_DEMUXER_MAX_FU_ACCUM     150000  /* max FU-A accumulation */

#define SDP_DEMUXER_DPLL_KP          0.5
#define SDP_DEMUXER_DPLL_KI          0.1
#define SDP_DEMUXER_DPLL_WINDUP_LIMIT 0.05
#define SDP_DEMUXER_DPLL_DELTA_LIMIT 0.01

/*===========================================================================
    RTP Header (RFC 3550)
===========================================================================*/
#pragma pack(push, 1)
typedef struct {
    uint8_t  cc:4, x:1, p:1, version:2;
    uint8_t  pt:7, m:1;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr_t;
#pragma pack(pop)

/* H.264 NAL unit types */
#define H264_NAL_SINGLE_MAX   23
#define H264_NAL_STAP_A       24
#define H264_NAL_FU_A         28

/* H.265 NAL unit types */
#define H265_NAL_IDR_W_RADL   19
#define H265_NAL_IDR_N_LP     20
#define H265_NAL_CRA_NUT      21
#define H265_NAL_VPS          32
#define H265_NAL_SPS          33
#define H265_NAL_PPS          34
#define H265_NAL_AP           48   /* Aggregation Packet */
#define H265_NAL_FU           49   /* Fragmentation Unit */

/*===========================================================================
    DPLL Context (avclksync algorithm)
===========================================================================*/
typedef struct {
    double kp;
    double ki;
    double integral;
    size_t buffer_capacity;  /* max reorder slots */
    size_t target_level;     /* target occupied slots (e.g. 50% of capacity) */
    double nominal_ratio;    /* 1.0 = nominal playback rate */
    double phase_accumulator;
    uint64_t last_update_us; /* last DPLL update timestamp */
} dpll_context_t;

static void dpll_init(dpll_context_t* ctx, size_t capacity) {
    ctx->kp = SDP_DEMUXER_DPLL_KP;
    ctx->ki = SDP_DEMUXER_DPLL_KI;
    ctx->integral = 0.0;
    ctx->buffer_capacity = capacity;
    ctx->target_level = capacity / 2;
    if (ctx->target_level < 1) ctx->target_level = 1;
    ctx->nominal_ratio = 1.0;
    ctx->phase_accumulator = 0.0;
    ctx->last_update_us = 0;
}

static void dpll_reset(dpll_context_t* ctx) {
    ctx->integral = 0.0;
    ctx->phase_accumulator = 0.0;
    ctx->last_update_us = 0;
}

/* Update DPLL with current buffer fill level, return adjusted ratio */
static double dpll_update(dpll_context_t* ctx, size_t current_level, uint64_t now_us) {
    if (ctx->last_update_us == 0) {
        ctx->last_update_us = now_us;
        return ctx->nominal_ratio;
    }

    double dt = (double)(now_us - ctx->last_update_us) / 1000000.0;
    ctx->last_update_us = now_us;
    if (dt <= 0.0 || dt > 1.0) dt = 0.01; /* clamp */

    /* Phase error: deviation from target fill level, normalized */
    double error = ((double)current_level - (double)ctx->target_level)
                   / (double)ctx->buffer_capacity;

    /* PI controller with anti-windup */
    ctx->integral += error * dt;
    if (ctx->integral >  SDP_DEMUXER_DPLL_WINDUP_LIMIT)
        ctx->integral =  SDP_DEMUXER_DPLL_WINDUP_LIMIT;
    if (ctx->integral < -SDP_DEMUXER_DPLL_WINDUP_LIMIT)
        ctx->integral = -SDP_DEMUXER_DPLL_WINDUP_LIMIT;

    double delta_r = (ctx->kp * error) + (ctx->ki * ctx->integral);

    /* Clamp instantaneous ratio shift */
    if (delta_r >  SDP_DEMUXER_DPLL_DELTA_LIMIT) delta_r =  SDP_DEMUXER_DPLL_DELTA_LIMIT;
    if (delta_r < -SDP_DEMUXER_DPLL_DELTA_LIMIT) delta_r = -SDP_DEMUXER_DPLL_DELTA_LIMIT;

    return ctx->nominal_ratio + delta_r;
}

/*===========================================================================
    RTP Reorder Buffer — ring-buffer indexed by seqno
===========================================================================*/
typedef struct {
    uint8_t* data;
    int      len;
    uint32_t timestamp;
    int      marker;       /* RTP marker bit */
    int      present;      /* slot occupied */
} reorder_slot_t;

typedef struct {
    reorder_slot_t* slots;
    uint16_t        mask;          /* capacity = mask + 1, must be power-of-2 */
    uint16_t        next_seq;      /* lowest sequence number not yet drained */
    int             count;         /* number of occupied slots */
    int             total_capacity;
    uint64_t        last_drain_us; /* timestamp of last successful drain */
    int             dropped;       /* cumulative drops */
} rtp_reorder_t;

static int is_power_of_2(uint32_t v) { return v && !(v & (v - 1)); }

static int reorder_init(rtp_reorder_t* r, int capacity) {
    memset(r, 0, sizeof(*r));
    /* Round up to next power of 2 */
    int cap = 1;
    while (cap < capacity) cap <<= 1;
    if (cap > 65536) cap = 65536;
    r->slots = calloc((size_t)cap, sizeof(reorder_slot_t));
    if (!r->slots) return -1;
    r->mask = (uint16_t)(cap - 1);
    r->total_capacity = cap;
    r->next_seq = 0; /* will be set on first insert */
    r->count = 0;
    r->last_drain_us = 0;
    r->dropped = 0;
    return 0;
}

static void reorder_destroy(rtp_reorder_t* r) {
    if (!r || !r->slots) return;
    for (int i = 0; i <= r->mask; i++) {
        if (r->slots[i].present) {
            free(r->slots[i].data);
        }
    }
    free(r->slots);
    r->slots = NULL;
}

/* Insert a packet into the reorder buffer, returns 0 on success */
static int reorder_insert(rtp_reorder_t* r, uint16_t seq, uint32_t ts,
                           int marker, const uint8_t* data, int len,
                           uint16_t max_lateness)
{
    /* If this is the first packet, initialise next_seq */
    if (r->count == 0) {
        r->next_seq = seq;
    }

    /* Check if packet is too old (below next_seq - max_lateness) */
    int16_t diff = (int16_t)(seq - r->next_seq);
    if (diff < -(int16_t)max_lateness) {
        r->dropped++;
        return -2; /* too late */
    }

    /* Too far ahead — would overflow the buffer */
    if (diff > (int16_t)(r->total_capacity - 1)) {
        r->dropped++;
        return -2;
    }

    uint16_t idx = seq & r->mask;
    reorder_slot_t* slot = &r->slots[idx];

    if (slot->present) {
        /* Slot already occupied — duplicate or wrap collision */
        /* Check if this is actually a different packet (seqno collision) */
        if (slot->len == len && memcmp(slot->data, data, len) == 0) {
            return 0; /* duplicate, skip */
        }
        /* Collision: assume the existing slot is stale.  Free and replace. */
        free(slot->data);
        slot->present = 0;
        r->count--;
    }

    slot->data = malloc((size_t)len);
    if (!slot->data) return -1;
    memcpy(slot->data, data, (size_t)len);
    slot->len = len;
    slot->timestamp = ts;
    slot->marker = marker;
    slot->present = 1;
    r->count++;
    return 0;
}

/* Peek the next contiguous packet. Returns data/len and removes the slot. */
static int reorder_drain(rtp_reorder_t* r, uint8_t** out_data, int* out_len,
                          uint32_t* out_ts, int* out_marker)
{
    if (r->count == 0) return 0;

    uint16_t idx = r->next_seq & r->mask;
    reorder_slot_t* slot = &r->slots[idx];

    if (!slot->present) return 0; /* gap */

    *out_data = slot->data;
    *out_len = slot->len;
    *out_ts = slot->timestamp;
    *out_marker = slot->marker;
    slot->present = 0;
    r->count--;
    r->next_seq++;
    r->last_drain_us = 0; /* set by caller if needed */
    return 1;
}

/* Peek without removing */
static int reorder_peek(rtp_reorder_t* r) {
    if (r->count == 0) return 0;
    uint16_t idx = r->next_seq & r->mask;
    return r->slots[idx].present ? 1 : 0;
}

/*===========================================================================
    SDP Track Descriptor
===========================================================================*/
typedef struct {
    int      type;          /* 0=none, 1=video, 2=audio */
    int      payload_type;  /* selected RTP payload type number */
    int      payload_types[SDP_DEMUXER_MAX_PT_PER_TRACK]; /* all fmt values from m= */
    int      payload_count;
    char     encoding[32];  /* "H264", "MPEG4-GENERIC", etc. */
    int      clock_rate;    /* 90000 for video, sample_rate for audio */
    int      channels;      /* for audio */
    char     fmtp[512];     /* format parameters */
    uint32_t ssrc;          /* learned from RTP stream */

    /* RTP reorder buffer */
    rtp_reorder_t reorder;

    /* FU-A reassembly state (H.264/5) */
    uint8_t  fu_accum[SDP_DEMUXER_MAX_FU_ACCUM];
    int      fu_accum_len;
    uint32_t fu_accum_ts;
    uint32_t fu_accum_ssrc;

    /* DPLL timing context */
    dpll_context_t dpll;

    /* PTS tracking */
    uint64_t base_pts;
    uint32_t base_rtp_ts;
    int      has_base_pts;

    /* RTCP SR sync state */
    int      has_sr;
    uint64_t last_ntp_time;
    uint32_t last_rtp_time;

    /* Codec extradata (SPS/PPS from fmtp or stream) */
    uint8_t* extra_data;
    int      extra_size;

    /* Source pad (dynamic) */
    zst_pad_t* src_pad;

    /* Track active flag */
    int      active;

    /* Stats */
    uint64_t packets_processed;
    uint64_t frames_pushed;
} sdp_track_t;

/*===========================================================================
    Element Private Data
===========================================================================*/
typedef struct {
    /* SDP state */
    char        sdp_text[4096];
    int         sdp_parsed;
    sdp_track_t tracks[SDP_DEMUXER_MAX_TRACKS];
    int         track_count;

    /* Configuration */
    int         jitter_buffer_ms;
    int         reorder_capacity;
    int         max_lateness_ms;
    int         default_clock_video;
    int         default_clock_audio;

    /* Pads */
    zst_pad_t*  sink_pad;

    /* Element state */
    int         started;
    int         opened;
    int         eos_sent;
    pthread_mutex_t lock;
} sdp_demuxer_t;

/*===========================================================================
    Forward declarations
===========================================================================*/
static zst_result_t sdp_demuxer_parse_sdp(sdp_demuxer_t* s);
static void         sdp_demuxer_create_src_pads(zst_element_t* el);
static void         sdp_demuxer_process_track(zst_element_t* el, sdp_track_t* track);
static void         sdp_demuxer_depacketize_h264(zst_element_t* el, sdp_track_t* track,
                                                   const uint8_t* data, int len,
                                                   uint32_t rtp_ts, int marker);
static void         sdp_demuxer_depacketize_h265(zst_element_t* el, sdp_track_t* track,
                                                   const uint8_t* data, int len,
                                                   uint32_t rtp_ts, int marker);
static void         sdp_demuxer_depacketize_aac(zst_element_t* el, sdp_track_t* track,
                                                  const uint8_t* data, int len,
                                                  uint32_t rtp_ts, int marker);

/*===========================================================================
    Utility functions
===========================================================================*/
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static uint64_t ntp_to_unix_ns(uint64_t ntp) {
    uint32_t sec  = (uint32_t)(ntp >> 32);
    uint32_t frac = (uint32_t)(ntp & 0xffffffffu);
    uint64_t unix_sec = sec >= 2208988800U ? (uint64_t)(sec - 2208988800U) : 0;
    uint64_t ns = ((uint64_t)frac * 1000000000ULL) >> 32;
    return unix_sec * 1000000000ULL + ns;
}

static void sdp_demuxer_free_track(sdp_track_t* tr) {
    reorder_destroy(&tr->reorder);
    free(tr->extra_data);
    tr->extra_data = NULL;
    tr->extra_size = 0;
    tr->active = 0;
}

static char* sdp_trim(char* str) {
    if (!str) return str;
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char* end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static int sdp_track_has_payload_type(const sdp_track_t* tr, int pt) {
    if (!tr || pt < 0) return 0;
    for (int i = 0; i < tr->payload_count; i++) {
        if (tr->payload_types[i] == pt) return 1;
    }
    return tr->payload_count == 0 && tr->payload_type == pt;
}

static void sdp_track_add_payload_type(sdp_track_t* tr, int pt) {
    if (!tr || pt < 0 || pt > 127 || sdp_track_has_payload_type(tr, pt)) return;
    if (tr->payload_count < SDP_DEMUXER_MAX_PT_PER_TRACK) {
        tr->payload_types[tr->payload_count++] = pt;
        if (tr->payload_type < 0) tr->payload_type = pt;
    }
}

static int sdp_encoding_is_h264(const char* enc) {
    return enc && strcasecmp(enc, "H264") == 0;
}

static int sdp_encoding_is_h265(const char* enc) {
    return enc && (strcasecmp(enc, "H265") == 0 ||
                   strcasecmp(enc, "HEVC") == 0 ||
                   strcasecmp(enc, "HVC1") == 0);
}

static int sdp_encoding_is_aac(const char* enc) {
    return enc && (strcasecmp(enc, "MPEG4-GENERIC") == 0 ||
                   strcasecmp(enc, "AAC") == 0);
}

static int sdp_track_codec_supported(const sdp_track_t* tr) {
    if (!tr || tr->payload_type < 0) return 0;
    if (tr->type == 1) return sdp_encoding_is_h264(tr->encoding) || sdp_encoding_is_h265(tr->encoding);
    if (tr->type == 2) return sdp_encoding_is_aac(tr->encoding);
    return 0;
}

static int sdp_base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    if (ch == '=') return -2;
    return -1;
}

static int sdp_base64_decode(const char* in, uint8_t* out, size_t out_cap) {
    int val = 0;
    int valb = -8;
    size_t out_len = 0;
    for (const unsigned char* p = (const unsigned char*)in; p && *p; p++) {
        if (isspace(*p)) continue;
        int d = sdp_base64_value((char)*p);
        if (d == -2) break;
        if (d < 0) return -1;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            if (out_len >= out_cap) return -1;
            out[out_len++] = (uint8_t)((val >> valb) & 0xff);
            valb -= 8;
        }
    }
    return (int)out_len;
}

static int sdp_append_extradata(sdp_track_t* tr, const uint8_t* data, int len,
                                int annexb_start_code) {
    if (!tr || !data || len <= 0) return 0;
    int prefix = annexb_start_code ? 4 : 0;
    uint8_t* next = realloc(tr->extra_data, (size_t)tr->extra_size + (size_t)prefix + (size_t)len);
    if (!next) return -1;
    tr->extra_data = next;
    if (prefix) {
        tr->extra_data[tr->extra_size + 0] = 0;
        tr->extra_data[tr->extra_size + 1] = 0;
        tr->extra_data[tr->extra_size + 2] = 0;
        tr->extra_data[tr->extra_size + 3] = 1;
    }
    memcpy(tr->extra_data + tr->extra_size + prefix, data, (size_t)len);
    tr->extra_size += prefix + len;
    return 0;
}

static int sdp_fmtp_get_param(const char* fmtp, const char* key,
                              char* out, size_t out_cap) {
    if (!fmtp || !key || !out || out_cap == 0) return 0;
    size_t key_len = strlen(key);
    const char* p = fmtp;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char* tok = p;
        p += strcspn(p, ";");
        size_t tok_len = (size_t)(p - tok);
        while (tok_len > 0 && isspace((unsigned char)tok[tok_len - 1])) tok_len--;
        if (tok_len > key_len && strncasecmp(tok, key, key_len) == 0) {
            const char* eq = tok + key_len;
            const char* tok_end = tok + tok_len;
            while (eq < tok_end && isspace((unsigned char)*eq)) eq++;
            if (eq < tok_end && *eq == '=') {
                const char* val = eq + 1;
                size_t val_len = (size_t)(tok_end - val);
                while (val_len > 0 && isspace((unsigned char)*val)) { val++; val_len--; }
                while (val_len > 0 && isspace((unsigned char)val[val_len - 1])) val_len--;
                if (val_len >= out_cap) val_len = out_cap - 1;
                memcpy(out, val, val_len);
                out[val_len] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static void sdp_parse_fmtp_extradata(sdp_track_t* tr) {
    if (!tr || tr->fmtp[0] == '\0') return;

    free(tr->extra_data);
    tr->extra_data = NULL;
    tr->extra_size = 0;

    if (sdp_encoding_is_h264(tr->encoding)) {
        char value[512];
        if (!sdp_fmtp_get_param(tr->fmtp, "sprop-parameter-sets", value, sizeof(value))) return;
        char* token = value;
        while (token && *token) {
            char* comma = strchr(token, ',');
            if (comma) *comma = '\0';
            token = sdp_trim(token);
            uint8_t decoded[512];
            int len = sdp_base64_decode(token, decoded, sizeof(decoded));
            if (len > 0) sdp_append_extradata(tr, decoded, len, 1);
            token = comma ? comma + 1 : NULL;
        }
    } else if (sdp_encoding_is_h265(tr->encoding)) {
        const char* keys[] = { "sprop-vps", "sprop-sps", "sprop-pps", NULL };
        for (int i = 0; keys[i]; i++) {
            char value[512];
            if (!sdp_fmtp_get_param(tr->fmtp, keys[i], value, sizeof(value))) continue;
            uint8_t decoded[512];
            int len = sdp_base64_decode(value, decoded, sizeof(decoded));
            if (len > 0) sdp_append_extradata(tr, decoded, len, 1);
        }
    } else if (sdp_encoding_is_aac(tr->encoding)) {
        char value[256];
        if (!sdp_fmtp_get_param(tr->fmtp, "config", value, sizeof(value))) return;
        size_t n = strlen(value);
        if ((n & 1u) != 0) return;
        uint8_t decoded[128];
        size_t out = 0;
        for (size_t i = 0; i + 1 < n && out < sizeof(decoded); i += 2) {
            if (!isxdigit((unsigned char)value[i]) || !isxdigit((unsigned char)value[i + 1])) return;
            char byte_str[3] = { value[i], value[i + 1], '\0' };
            decoded[out++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }
        if (out > 0) sdp_append_extradata(tr, decoded, (int)out, 0);
    }
}

/*===========================================================================
    SDP Parser
===========================================================================*/
static zst_result_t sdp_demuxer_parse_sdp(sdp_demuxer_t* s) {
    if (!s || s->sdp_text[0] == '\0') return -1;

    /* The parser is intentionally conservative: it builds one zstreamer track
     * per media section, remembers every advertised fmt long enough to parse
     * matching rtpmap/fmtp lines, and selects the first supported payload for
     * actual RTP routing/depacketization. */
    for (int i = 0; i < s->track_count; i++) {
        if (s->tracks[i].src_pad) {
            ZST_LOG_WARN("sdpdemux", "refusing to reparse SDP while dynamic pads exist");
            return -1;
        }
        if (!s->tracks[i].src_pad) {
            sdp_demuxer_free_track(&s->tracks[i]);
            memset(&s->tracks[i], 0, sizeof(s->tracks[i]));
        }
    }

    const char* p = s->sdp_text;
    const char* end = s->sdp_text + strlen(s->sdp_text);
    sdp_track_t* track = NULL;
    int raw_count = 0;

    while (p < end) {
        const char* eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;

        size_t line_len = (size_t)(eol - p);
        while (line_len > 0 && (p[line_len - 1] == '\r' || p[line_len - 1] == '\n'))
            line_len--;

        char line[SDP_DEMUXER_MAX_LINE];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        char* trimmed = sdp_trim(line);

        if (trimmed[0] == 'm' && trimmed[1] == '=') {
            track = NULL;
            if (raw_count >= SDP_DEMUXER_MAX_TRACKS) {
                p = (eol < end) ? eol + 1 : end;
                continue;
            }

            char* rest = sdp_trim(trimmed + 2);
            char* save = NULL;
            char* media = strtok_r(rest, " \t", &save);
            char* port  = strtok_r(NULL, " \t", &save);
            char* proto = strtok_r(NULL, " \t", &save);
            (void)port;
            (void)proto;

            int media_type = 0;
            if (media && strcasecmp(media, "video") == 0) media_type = 1;
            else if (media && strcasecmp(media, "audio") == 0) media_type = 2;
            if (media_type == 0) {
                p = (eol < end) ? eol + 1 : end;
                continue;
            }

            sdp_track_t* cur = &s->tracks[raw_count];
            memset(cur, 0, sizeof(*cur));
            cur->payload_type = -1;
            cur->type = media_type;
            cur->clock_rate = (cur->type == 1) ? s->default_clock_video : s->default_clock_audio;
            cur->channels = (cur->type == 2) ? 2 : 0;

            char* fmt = NULL;
            while ((fmt = strtok_r(NULL, " \t", &save)) != NULL) {
                char* endptr = NULL;
                long pt = strtol(fmt, &endptr, 10);
                if (endptr != fmt && *endptr == '\0' && pt >= 0 && pt <= 127) {
                    sdp_track_add_payload_type(cur, (int)pt);
                }
            }

            if (cur->payload_count == 0) {
                ZST_LOG_WARN("sdpdemux", "ignoring media section without RTP payloads");
                memset(cur, 0, sizeof(*cur));
                p = (eol < end) ? eol + 1 : end;
                continue;
            }

            if (reorder_init(&cur->reorder, s->reorder_capacity) != 0) {
                ZST_LOG_ERROR("sdpdemux", "reorder_init failed for track %d", raw_count);
                memset(cur, 0, sizeof(*cur));
                p = (eol < end) ? eol + 1 : end;
                continue;
            }
            dpll_init(&cur->dpll, (size_t)s->reorder_capacity);
            cur->dpll.target_level = (size_t)(s->reorder_capacity *
                                         s->jitter_buffer_ms /
                                         (s->jitter_buffer_ms + 100));
            if (cur->dpll.target_level < 1) cur->dpll.target_level = 1;
            cur->active = 1;
            track = cur;
            raw_count++;
        } else if (trimmed[0] == 'a' && trimmed[1] == '=' && track) {
            char* val = sdp_trim(trimmed + 2);

            if (strncasecmp(val, "rtpmap:", 7) == 0) {
                char* attr = sdp_trim(val + 7);
                char* endptr = NULL;
                long pt = strtol(attr, &endptr, 10);
                if (endptr != attr && pt >= 0 && pt <= 127 && sdp_track_has_payload_type(track, (int)pt)) {
                    char* enc = sdp_trim(endptr);
                    char* slash = strchr(enc, '/');
                    if (slash) {
                        *slash = '\0';
                        char* clock = slash + 1;
                        char* slash2 = strchr(clock, '/');
                        if (slash2) *slash2 = '\0';

                        /* SDP may advertise several payload alternatives in one
                         * m= section.  Keep the first supported codec we find;
                         * keep scanning while the current selection is unsupported. */
                        if (!sdp_track_codec_supported(track) || track->payload_type == (int)pt) {
                            size_t enc_len = strlen(enc);
                            if (enc_len >= sizeof(track->encoding)) enc_len = sizeof(track->encoding) - 1;
                            memcpy(track->encoding, enc, enc_len);
                            track->encoding[enc_len] = '\0';

                            int clock_rate = atoi(clock);
                            if (clock_rate > 0) track->clock_rate = clock_rate;
                            if (slash2) {
                                int ch = atoi(slash2 + 1);
                                if (ch > 0) track->channels = ch;
                            } else if (track->type == 2 && track->channels <= 0) {
                                track->channels = 1;
                            }
                            track->payload_type = (int)pt;
                            if (track->fmtp[0]) sdp_parse_fmtp_extradata(track);
                        }
                    }
                }
            } else if (strncasecmp(val, "fmtp:", 5) == 0) {
                char* attr = sdp_trim(val + 5);
                char* endptr = NULL;
                long pt = strtol(attr, &endptr, 10);
                if (endptr != attr && pt >= 0 && pt <= 127 && sdp_track_has_payload_type(track, (int)pt)) {
                    if (track->payload_type == (int)pt || !sdp_track_codec_supported(track)) {
                        char* fmtp = sdp_trim(endptr);
                        strncpy(track->fmtp, fmtp, sizeof(track->fmtp) - 1);
                        track->fmtp[sizeof(track->fmtp) - 1] = '\0';
                        track->payload_type = (int)pt;
                        sdp_parse_fmtp_extradata(track);
                    }
                }
            } else if (strcasecmp(val, "inactive") == 0) {
                track->active = 0;
            }
        }

        p = (eol < end) ? eol + 1 : end;
    }

    /* Compact away unsupported/inactive media sections so the rest of the
     * element never creates misleading H.264/AAC caps for unknown codecs. */
    int kept = 0;
    for (int i = 0; i < raw_count; i++) {
        sdp_track_t* cur = &s->tracks[i];
        if (!cur->active || !sdp_track_codec_supported(cur)) {
            ZST_LOG_WARN("sdpdemux", "ignoring unsupported SDP track type=%d pt=%d encoding=%s",
                            cur->type, cur->payload_type, cur->encoding[0] ? cur->encoding : "<unset>");
            sdp_demuxer_free_track(cur);
            memset(cur, 0, sizeof(*cur));
            continue;
        }
        if (kept != i) {
            s->tracks[kept] = *cur;
            memset(cur, 0, sizeof(*cur));
        }
        kept++;
    }

    s->track_count = kept;
    s->sdp_parsed = 1;
    ZST_LOG_INFO("sdpdemux", "parsed %d supported track(s) from SDP", s->track_count);
    return s->track_count;
}

/*===========================================================================
    DPLL-adjusted PTS computation
===========================================================================*/
static uint64_t sdp_rtp_ts_to_pts(sdp_demuxer_t* s, sdp_track_t* track,
                                   uint32_t rtp_ts)
{
    (void)s;
    int clock_rate = track->clock_rate > 0 ? track->clock_rate : 90000;

    if (track->has_sr) {
        uint32_t delta = rtp_ts - track->last_rtp_time;
        return ntp_to_unix_ns(track->last_ntp_time) +
               (uint64_t)delta * 1000000000ULL / (uint64_t)clock_rate;
    }

    if (!track->has_base_pts) {
        track->base_pts = now_us() * 1000;
        track->base_rtp_ts = rtp_ts;
        track->has_base_pts = 1;
        return track->base_pts;
    }

    /* Handle RTP timestamp wraparound (32-bit) */
    int32_t delta = (int32_t)(rtp_ts - track->base_rtp_ts);
    if (delta < 0) delta += (int32_t)0x100000000LL;

    uint64_t pts = track->base_pts +
        (uint64_t)delta * 1000000000ULL / (uint64_t)clock_rate;

    /* Apply DPLL ratio adjustment */
    uint64_t now = now_us();
    double ratio = dpll_update(&track->dpll, (size_t)track->reorder.count, now);

    if (ratio > 0.0 && ratio != 1.0) {
        /* Scale PTS inversely to speed up or slow down output timing */
        pts = (uint64_t)((double)pts / ratio);
    }

    return pts;
}

/*===========================================================================
    RTCP processing (for Sender Report NTP sync)
===========================================================================*/
static void sdp_process_rtcp(sdp_demuxer_t* s, const uint8_t* data, int len) {
    if (!data || len < 4) return;
    int off = 0;
    while (off + 4 <= len) {
        uint8_t pt = data[off + 1];
        uint16_t words = ((uint16_t)data[off + 2] << 8) | data[off + 3];
        int plen = ((int)words + 1) * 4;
        if (plen <= 0 || off + plen > len) break;

        /* Sender Report (SR) */
        if (pt == 200 && plen >= 28) {
            uint32_t ssrc = ((uint32_t)data[off + 4] << 24) |
                            ((uint32_t)data[off + 5] << 16) |
                            ((uint32_t)data[off + 6] << 8)  |
                            (uint32_t)data[off + 7];
            uint64_t ntp = ((uint64_t)data[off + 8] << 56) |
                           ((uint64_t)data[off + 9] << 48) |
                           ((uint64_t)data[off + 10] << 40) |
                           ((uint64_t)data[off + 11] << 32) |
                           ((uint64_t)data[off + 12] << 24) |
                           ((uint64_t)data[off + 13] << 16) |
                           ((uint64_t)data[off + 14] << 8)  |
                           (uint64_t)data[off + 15];
            uint32_t rtp = ((uint32_t)data[off + 16] << 24) |
                           ((uint32_t)data[off + 17] << 16) |
                           ((uint32_t)data[off + 18] << 8)  |
                           (uint32_t)data[off + 19];
            /* Match SSRC to a track */
            for (int i = 0; i < s->track_count; i++) {
                if (s->tracks[i].ssrc == ssrc || s->tracks[i].ssrc == 0) {
                    if (s->tracks[i].ssrc == 0) s->tracks[i].ssrc = ssrc;
                    s->tracks[i].last_ntp_time = ntp;
                    s->tracks[i].last_rtp_time = rtp;
                    s->tracks[i].has_sr = 1;
                    dpll_reset(&s->tracks[i].dpll);
                    break;
                }
            }
        }
        off += plen;
    }
}

/*===========================================================================
    Push a depacketized media buffer on a source pad
===========================================================================*/
static void sdp_push_buffer(zst_element_t* el, sdp_track_t* track,
                             const uint8_t* data, int len,
                             uint64_t pts, int is_video)
{
    (void)el;
    if (!track->src_pad || !track->src_pad->peer) return;

    uint32_t buf_type = is_video ? ZST_BUFFER_VIDEO_PACKET : ZST_BUFFER_AUDIO_PACKET;
    zst_buffer_t* buf = zst_buffer_create(buf_type);
    if (!buf) return;

    /* With Annex B start code for video */
    int total = len;
    uint8_t* out = malloc((size_t)total);
    if (!out) { zst_buffer_unref(buf); return; }
    memcpy(out, data, (size_t)total);

    buf->memory.data = out;
    buf->memory.size = (size_t)total;
    buf->memory.priv = out;
    buf->memory.release = free;
    buf->pts = pts;
    buf->dts = pts;

    /* Attach codec extradata if available */
    if (track->extra_data && track->extra_size > 0) {
        /* Set as buffer metadata — simple approach: store pointer */
        buf->payload = track->extra_data;
    }

    zst_pad_push(track->src_pad, buf);
    zst_buffer_unref(buf);
    track->frames_pushed++;
}

/*===========================================================================
    H.264 Depacketization (RFC 3984)
===========================================================================*/
static void sdp_push_h264_nal(zst_element_t* el, sdp_track_t* track,
                               const uint8_t* data, int len, uint64_t pts, int marker)
{
    (void)marker;
    /* Write Annex B start code (4 bytes) + NAL unit */
    int total = len + 4;
    uint8_t* out = malloc((size_t)total);
    if (!out) return;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    memcpy(out + 4, data, (size_t)len);

    sdp_push_buffer(el, track, out, total, pts, 1);
    free(out);
}

static void sdp_demuxer_depacketize_h264(zst_element_t* el, sdp_track_t* track,
                                          const uint8_t* data, int len,
                                          uint32_t rtp_ts, int marker)
{
    if (len < 1) return;
    uint8_t nal_type = data[0] & 0x1f;
    uint64_t pts = sdp_rtp_ts_to_pts(el->priv, track, rtp_ts);

    if (nal_type <= H264_NAL_SINGLE_MAX) {
        /* Single NAL unit */
        sdp_push_h264_nal(el, track, data, len, pts, marker);
    } else if (nal_type == H264_NAL_FU_A && len >= 2) {
        /* FU-A fragmentation */
        uint8_t fu_indicator = data[0];
        uint8_t fu_header    = data[1];
        uint8_t fu_nal_type  = fu_header & 0x1f;
        uint8_t start_bit    = (fu_header >> 7) & 1;
        uint8_t end_bit      = (fu_header >> 6) & 1;
        uint8_t nri          = (fu_indicator >> 5) & 0x03;

        if (start_bit) {
            track->fu_accum_len = 0;
            track->fu_accum_ts = rtp_ts;
            track->fu_accum_ssrc = track->ssrc;
            if (track->fu_accum_len + 1 <= (int)sizeof(track->fu_accum)) {
                track->fu_accum[track->fu_accum_len++] = (nri << 5) | fu_nal_type;
            }
        }

        if (track->fu_accum_ssrc == track->ssrc && track->fu_accum_ts == rtp_ts) {
            int copy_len = len - 2;
            if (copy_len > 0 && track->fu_accum_len + copy_len <= (int)sizeof(track->fu_accum)) {
                memcpy(track->fu_accum + track->fu_accum_len, data + 2, (size_t)copy_len);
                track->fu_accum_len += copy_len;
            }
        }

        if (end_bit && track->fu_accum_len > 1) {
            sdp_push_h264_nal(el, track, track->fu_accum, track->fu_accum_len,
                              sdp_rtp_ts_to_pts(el->priv, track, track->fu_accum_ts), 1);
            track->fu_accum_len = 0;
        }
    } else if (nal_type == H264_NAL_STAP_A) {
        /* STAP-A: multiple NALs in one packet */
        int offset = 1;
        while (offset + 2 <= len) {
            uint16_t nalu_size = (uint16_t)(data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (offset + nalu_size > len) break;
            sdp_push_h264_nal(el, track, data + offset, nalu_size, pts,
                              (offset + nalu_size >= len) ? marker : 0);
            offset += nalu_size;
        }
    }
}

/*===========================================================================
    H.265 Depacketization (RFC 7798)
===========================================================================*/
static void sdp_push_h265_nal(zst_element_t* el, sdp_track_t* track,
                               const uint8_t* data, int len, uint64_t pts)
{
    int total = len + 4;
    uint8_t* out = malloc((size_t)total);
    if (!out) return;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    memcpy(out + 4, data, (size_t)len);

    sdp_push_buffer(el, track, out, total, pts, 1);
    free(out);
}

static void sdp_demuxer_depacketize_h265(zst_element_t* el, sdp_track_t* track,
                                          const uint8_t* data, int len,
                                          uint32_t rtp_ts, int marker)
{
    (void)marker;
    if (len < 2) return;

    /* H.265 NAL unit header: 2 bytes */
    uint16_t nal_header = (uint16_t)((data[0] << 8) | data[1]);
    uint8_t nal_type = (uint8_t)((nal_header >> 9) & 0x3f);
    uint64_t pts = sdp_rtp_ts_to_pts(el->priv, track, rtp_ts);

    if (nal_type == H265_NAL_FU) {
        /* Fragmentation Unit */
        if (len < 3) return;
        uint8_t fu_header = data[2];
        uint8_t fu_nal_type = fu_header & 0x3f;
        uint8_t start_bit   = (fu_header >> 7) & 1;
        uint8_t end_bit     = (fu_header >> 6) & 1;

        if (start_bit) {
            track->fu_accum_len = 0;
            track->fu_accum_ts = rtp_ts;
            track->fu_accum_ssrc = track->ssrc;
            /* Write the NAL unit header with reconstructed type */
            if (track->fu_accum_len + 2 <= (int)sizeof(track->fu_accum)) {
                track->fu_accum[0] = (uint8_t)((nal_header >> 8) & 0xff);
                track->fu_accum[1] = (uint8_t)(nal_header & 0xff);
                /* Replace the NAL unit type field */
                track->fu_accum[1] = (track->fu_accum[1] & 0x81) | (fu_nal_type << 1);
                track->fu_accum_len = 2;
            }
        }

        if (track->fu_accum_ssrc == track->ssrc && track->fu_accum_ts == rtp_ts) {
            int copy_len = len - 3;
            if (copy_len > 0 && track->fu_accum_len + copy_len <= (int)sizeof(track->fu_accum)) {
                memcpy(track->fu_accum + track->fu_accum_len, data + 3, (size_t)copy_len);
                track->fu_accum_len += copy_len;
            }
        }

        if (end_bit && track->fu_accum_len > 2) {
            sdp_push_h265_nal(el, track, track->fu_accum, track->fu_accum_len,
                              sdp_rtp_ts_to_pts(el->priv, track, track->fu_accum_ts));
            track->fu_accum_len = 0;
        }
    } else if (nal_type == H265_NAL_AP) {
        /* Aggregation Packet */
        int offset = 2; /* skip 2-byte DONL? Actually AP has no DONL by default */
        /* Each aggregation unit has 2-byte size + NAL unit */
        while (offset + 2 <= len) {
            uint16_t nalu_size = (uint16_t)(data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (offset + nalu_size > len) break;
            sdp_push_h265_nal(el, track, data + offset, nalu_size, pts);
            offset += nalu_size;
        }
    } else {
        /* Single NAL unit */
        sdp_push_h265_nal(el, track, data, len, pts);
    }
}

/*===========================================================================
    AAC Depacketization (RFC 3640 MPEG4-Generic)
===========================================================================*/
static void sdp_push_aac_frame(zst_element_t* el, sdp_track_t* track,
                                const uint8_t* data, int len, uint64_t pts)
{
    /* AAC ADTS frames or raw — push as-is */
    uint8_t* out = malloc((size_t)len);
    if (!out) return;
    memcpy(out, data, (size_t)len);

    uint32_t buf_type = ZST_BUFFER_AUDIO_PACKET;
    zst_buffer_t* buf = zst_buffer_create(buf_type);
    if (!buf) { free(out); return; }

    buf->memory.data = out;
    buf->memory.size = (size_t)len;
    buf->memory.priv = out;
    buf->memory.release = free;
    buf->pts = pts;
    buf->dts = pts;

    if (track->src_pad && track->src_pad->peer) {
        zst_pad_push(track->src_pad, buf);
    }
    zst_buffer_unref(buf);
    track->frames_pushed++;
}

static void sdp_demuxer_depacketize_aac(zst_element_t* el, sdp_track_t* track,
                                         const uint8_t* data, int len,
                                         uint32_t rtp_ts, int marker)
{
    (void)marker;
    if (len < 4) return;

    /* MPEG4-Generic: AU-headers-length (2 bytes, in bits) */
    int au_headers_len_bits = (data[0] << 8) | data[1];
    int au_headers_len_bytes = (au_headers_len_bits + 7) / 8;

    if (au_headers_len_bytes + 2 > len) return;

    int offset = 2; /* skip AU-headers-length */
    uint64_t pts = sdp_rtp_ts_to_pts(el->priv, track, rtp_ts);

    /* Parse each AU-header (13-bit size + 3-bit index) */
    while (offset + 2 <= len) {
        uint16_t au_header = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        offset += 2;
        int au_size = (au_header >> 3) & 0x1FFF;

        if (au_size > 0 && offset + au_size <= len) {
            sdp_push_aac_frame(el, track, data + offset, au_size, pts);
            offset += au_size;
        } else {
            break;
        }
    }
}

/*===========================================================================
    Process a single depacketized media buffer from the reorder drain
===========================================================================*/
static void sdp_demuxer_depacketize(sdp_demuxer_t* s, sdp_track_t* track,
                                     const uint8_t* payload, int payload_len,
                                     uint32_t rtp_ts, int marker,
                                     zst_element_t* el)
{
    track->packets_processed++;

    if (track->type == 1) {
        if (sdp_encoding_is_h264(track->encoding)) {
            sdp_demuxer_depacketize_h264(el, track, payload, payload_len, rtp_ts, marker);
        } else if (sdp_encoding_is_h265(track->encoding)) {
            sdp_demuxer_depacketize_h265(el, track, payload, payload_len, rtp_ts, marker);
        }
    } else if (track->type == 2) {
        if (sdp_encoding_is_aac(track->encoding)) {
            sdp_demuxer_depacketize_aac(el, track, payload, payload_len, rtp_ts, marker);
        }
    }
}

/*===========================================================================
    Process RTP header and route packet to the correct track's reorder buffer
===========================================================================*/
static void sdp_process_rtp_packet(sdp_demuxer_t* s, const uint8_t* rtp_data,
                                    int pkt_len, zst_element_t* el)
{
    if (pkt_len < 12) return;

    const rtp_hdr_t* rh = (const rtp_hdr_t*)rtp_data;
    int pt = rh->pt;
    int marker = rh->m;
    uint16_t seq = ntohs(rh->seq);
    uint32_t rtp_ts = ntohl(rh->timestamp);
    uint32_t ssrc = ntohl(rh->ssrc);

    /* Skip header extensions and CSRC */
    int payload_offset = 12;
    if (rh->cc > 0) payload_offset += rh->cc * 4;
    if (rh->x && payload_offset + 4 <= pkt_len) {
        uint16_t ext_len = ntohs(*(uint16_t*)(rtp_data + payload_offset + 2));
        payload_offset += 4 + ext_len * 4;
    }

    if (payload_offset >= pkt_len) return;
    int payload_len = pkt_len - payload_offset;
    const uint8_t* payload = rtp_data + payload_offset;

    /* Find matching track by SSRC or payload type */
    sdp_track_t* track = NULL;
    for (int i = 0; i < s->track_count; i++) {
        if (!s->tracks[i].active) continue;
        if (s->tracks[i].ssrc == ssrc) {
            track = &s->tracks[i];
            break;
        }
    }
    /* Fallback: match by payload type */
    if (!track) {
        for (int i = 0; i < s->track_count; i++) {
            if (!s->tracks[i].active) continue;
            if (s->tracks[i].payload_type == pt) {
                track = &s->tracks[i];
                if (track->ssrc == 0) track->ssrc = ssrc;
                break;
            }
        }
    }
    if (!track) {
        /* Unknown stream — try learning by creating new mapping */
        ZST_LOG_DEBUG("sdpdemux", "unknown RTP stream pt=%d ssrc=%u", pt, ssrc);
        return;
    }

    /* Insert into reorder buffer */
    int ret = reorder_insert(&track->reorder, seq, rtp_ts, marker,
                              payload, payload_len,
                              (uint16_t)s->max_lateness_ms);
    if (ret == -2) {
        ZST_LOG_DEBUG("sdpdemux", "dropped late packet seq=%u track=%d", seq,
                      (int)(track - s->tracks));
    }
}

/*===========================================================================
    Drain a track's reorder buffer — depacketize contiguous packets
===========================================================================*/
static void sdp_demuxer_process_track(zst_element_t* el, sdp_track_t* track) {
    sdp_demuxer_t* s = el->priv;
    if (!track->active || !track->src_pad || !track->src_pad->peer) return;

    /* Drain as many contiguous packets as possible */
    while (reorder_peek(&track->reorder)) {
        uint8_t* data;
        int len;
        uint32_t ts;
        int marker;
        if (reorder_drain(&track->reorder, &data, &len, &ts, &marker)) {
            if (data && len > 0) {
                sdp_demuxer_depacketize(s, track, data, len, ts, marker, el);
            }
            free(data);
        } else {
            break;
        }
    }
}

/*===========================================================================
    Sink pad push callback — receives chunks of raw RTP data
===========================================================================*/
static zst_result_t sdp_sink_push(zst_pad_t* pad, zst_buffer_t* buf) {
    zst_element_t* el = pad->parent;
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_sent = 1;
        /* Push EOS on all source pads */
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->track_count; i++) {
            if (s->tracks[i].active && s->tracks[i].src_pad && s->tracks[i].src_pad->peer) {
                zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
                if (eos) {
                    eos->flags |= ZST_BUFFER_FLAG_EOS;
                    zst_pad_push(s->tracks[i].src_pad, eos);
                    zst_buffer_unref(eos);
                }
            }
        }
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (!buf->memory.data || buf->memory.size == 0) return ZST_OK;

    pthread_mutex_lock(&s->lock);

    const uint8_t* data = (const uint8_t*)buf->memory.data;
    size_t size = buf->memory.size;
    size_t offset = 0;

    /* Process the data. It could be:
     *   - A single raw RTP packet
     *   - Multiple RTP packets (batched by upstream)
     *   - An interleaved RTP stream ($ + channel + len + data)
     *   - An RTCP compound packet
     *
     * We first check if this is interleaved data (starts with '$').
     */
    while (offset < size) {
        if (data[offset] == '$' && offset + 4 <= size) {
            /* Interleaved format: $channel(1) len(2) data(len) */
            int channel_len = (data[offset + 2] << 8) | data[offset + 3];
            int total = 4 + channel_len;
            if (offset + total > size) break;

            uint8_t channel = data[offset + 1];
            (void)channel;

            if (channel_len >= 12) {
                sdp_process_rtp_packet(s, data + offset + 4, channel_len, el);
            }
            offset += (size_t)total;
        } else if (offset + 4 <= size) {
            /* Raw RTP/RTCP packet.
             * Check if it's an RTCP packet (PT 200-209) */
            uint8_t pt = data[offset + 1];
            if (pt >= 200 && pt <= 209) {
                /* RTCP packet — process for SR/BYE */
                int pkt_len = (int)((data[offset + 2] << 8) | data[offset + 3]);
                int total = (pkt_len + 1) * 4;
                if ((size_t)total > size - offset) total = (int)(size - offset);
                sdp_process_rtcp(s, data + offset, total);
                offset += (size_t)total;
            } else {
                /* Raw RTP: process it.  RTP header version should be 2. */
                int version = (data[offset] >> 6) & 0x03;
                if (version == 2) {
                    int in_pkt_len = (int)((data[offset + 2] << 8) | data[offset + 3]);
                    in_pkt_len += 12; /* minimum RTP header */
                    if (in_pkt_len > (int)(size - offset))
                        in_pkt_len = (int)(size - offset);

                    sdp_process_rtp_packet(s, data + offset, in_pkt_len, el);
                    offset += (size_t)in_pkt_len;
                } else {
                    /* Unknown format, skip one byte */
                    offset++;
                }
            }
        } else {
            break;
        }
    }

    /* Now drain all active tracks */
    for (int i = 0; i < s->track_count; i++) {
        sdp_demuxer_process_track(el, &s->tracks[i]);
    }

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

/*===========================================================================
    Element ops
===========================================================================*/
static zst_result_t sdp_open(zst_element_t* el) {
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    if (s->opened) {
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    /* Parse SDP if not already parsed */
    if (!s->sdp_parsed && s->sdp_text[0]) {
        if (sdp_demuxer_parse_sdp(s) <= 0) {
            ZST_LOG_ERROR("sdpdemux", "no valid SDP tracks found");
            pthread_mutex_unlock(&s->lock);
            return ZST_ERROR;
        }
    }

    /* Create dynamic source pads from SDP tracks */
    if (s->sdp_parsed && s->track_count > 0 && !s->tracks[0].src_pad) {
        sdp_demuxer_create_src_pads(el);
    }

    s->opened = 1;
    s->eos_sent = 0;
    ZST_LOG_INFO("sdpdemux", "opened with %d tracks", s->track_count);
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t sdp_close(zst_element_t* el) {
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->track_count; i++) {
        sdp_demuxer_free_track(&s->tracks[i]);
        /* Remove dynamic source pads */
        if (s->tracks[i].src_pad) {
            zst_element_remove_pad(el, s->tracks[i].src_pad);
            s->tracks[i].src_pad = NULL;
        }
    }
    s->track_count = 0;
    s->sdp_parsed = 0;
    s->opened = 0;
    s->started = 0;
    s->eos_sent = 0;
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t sdp_start(zst_element_t* el) {
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    if (!s->opened) {
        /* Auto-open if needed */
        pthread_mutex_unlock(&s->lock);
        sdp_open(el);
        pthread_mutex_lock(&s->lock);
    }

    if (s->track_count == 0 || !s->sdp_parsed) {
        ZST_LOG_ERROR("sdpdemux", "no SDP tracks configured");
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    s->started = 1;
    ZST_LOG_INFO("sdpdemux", "started streaming");
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t sdp_stop(zst_element_t* el) {
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    s->started = 0;
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_caps_t* sdp_get_caps(zst_element_t* el, zst_pad_t* pad,
                                 const zst_caps_t* filter)
{
    (void)filter;
    sdp_demuxer_t* s = el->priv;
    if (!s) return NULL;

    /* For the sink pad: we accept raw RTP data */
    if (pad == s->sink_pad) {
        zst_caps_t* caps = zst_caps_create();
        if (!caps) return NULL;
        zst_caps_append(caps, zst_caps_struct_create_video("raw/rtp", 0, 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("application/x-rtp", 0, 0, 0, ""));
        return caps;
    }

    /* For source pads: determine caps from track info */
    for (int i = 0; i < s->track_count; i++) {
        if (s->tracks[i].src_pad == pad) {
            zst_caps_t* caps = zst_caps_create();
            if (!caps) return NULL;

            if (s->tracks[i].type == 1) {
                const char* media_type = "video/x-h264";
                if (sdp_encoding_is_h265(s->tracks[i].encoding))
                    media_type = "video/x-h265";
                zst_caps_append(caps, zst_caps_struct_create_video(media_type, 0, 0, 0, ""));
            } else if (s->tracks[i].type == 2) {
                zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
            }
            return caps;
        }
    }

    return NULL;
}

/*===========================================================================
    Create dynamic source pads from parsed SDP tracks
===========================================================================*/
static void sdp_demuxer_create_src_pads(zst_element_t* el) {
    sdp_demuxer_t* s = el->priv;
    if (!s) return;

    for (int i = 0; i < s->track_count; i++) {
        sdp_track_t* track = &s->tracks[i];
        if (!track->active) continue;

        char pad_name[32];
        const char* prefix = (track->type == 1) ? "video" : "audio";
        snprintf(pad_name, sizeof(pad_name), "%s_%d", prefix, i);

        track->src_pad = zst_pad_create(pad_name, ZST_PAD_SRC);
        if (!track->src_pad) {
            ZST_LOG_ERROR("sdpdemux", "failed to create source pad %s", pad_name);
            track->active = 0;
            continue;
        }

        if (zst_element_add_pad(el, track->src_pad) != ZST_OK) {
            ZST_LOG_ERROR("sdpdemux", "failed to add source pad %s", pad_name);
            zst_pad_destroy(track->src_pad);
            track->src_pad = NULL;
            track->active = 0;
            continue;
        }

        /* Set template caps */
        zst_caps_t* template_caps = sdp_get_caps(el, track->src_pad, NULL);
        if (template_caps) {
            zst_pad_set_template_caps(track->src_pad, template_caps);
            zst_caps_destroy(template_caps);
        }

        ZST_LOG_INFO("sdpdemux", "created src pad %s (type=%s, encoding=%s, pt=%d, clock=%d)",
                     pad_name,
                     track->type == 1 ? "video" : "audio",
                     track->encoding,
                     track->payload_type,
                     track->clock_rate);
    }
}

/*===========================================================================
    Process callback (pull mode — not used, this is push-based)
===========================================================================*/
static zst_result_t sdp_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el; (void)in; (void)out;
    return ZST_OK; /* handled by sink pad push */
}

/*===========================================================================
    Load SDP from file
===========================================================================*/
static int sdp_load_file(sdp_demuxer_t* s, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        ZST_LOG_ERROR("sdpdemux", "failed to open SDP file '%s': %s",
                       path, strerror(errno));
        return -1;
    }

    size_t total = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF && total < sizeof(s->sdp_text) - 1) {
        s->sdp_text[total++] = (char)ch;
    }
    s->sdp_text[total] = '\0';
    fclose(f);

    ZST_LOG_INFO("sdpdemux", "loaded SDP from '%s' (%zu bytes)", path, total);
    return sdp_demuxer_parse_sdp(s);
}

/*===========================================================================
    Properties
===========================================================================*/
static zst_result_t sdp_set_property(zst_element_t* el, const char* name,
                                      const char* value)
{
    if (!el || !name || !value) return ZST_ERROR;
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    if (strcmp(name, "sdp") == 0) {
        strncpy(s->sdp_text, value, sizeof(s->sdp_text) - 1);
        s->sdp_text[sizeof(s->sdp_text) - 1] = '\0';
        s->sdp_parsed = 0;
        zst_result_t ret = sdp_demuxer_parse_sdp(s);
        pthread_mutex_unlock(&s->lock);
        return ret >= 0 ? ZST_OK : ZST_ERROR;
    }

    if (strcmp(name, "sdp-file") == 0 || strcmp(name, "sdp_file") == 0) {
        int ret = sdp_load_file(s, value);
        pthread_mutex_unlock(&s->lock);
        return ret >= 0 ? ZST_OK : ZST_ERROR;
    }

    if (strcmp(name, "jitter-buffer-ms") == 0 || strcmp(name, "jitter_buffer_ms") == 0) {
        s->jitter_buffer_ms = atoi(value);
        if (s->jitter_buffer_ms < 1) s->jitter_buffer_ms = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "reorder-capacity") == 0 || strcmp(name, "reorder_capacity") == 0) {
        s->reorder_capacity = atoi(value);
        if (s->reorder_capacity < 16) s->reorder_capacity = 16;
        if (s->reorder_capacity > 2048) s->reorder_capacity = 2048;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "max-lateness-ms") == 0 || strcmp(name, "max_lateness_ms") == 0) {
        s->max_lateness_ms = atoi(value);
        if (s->max_lateness_ms < 1) s->max_lateness_ms = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "clock-rate-video") == 0 || strcmp(name, "clock_rate_video") == 0) {
        s->default_clock_video = atoi(value);
        if (s->default_clock_video < 1) s->default_clock_video = 90000;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "clock-rate-audio") == 0 || strcmp(name, "clock_rate_audio") == 0) {
        s->default_clock_audio = atoi(value);
        if (s->default_clock_audio < 1) s->default_clock_audio = 48000;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    pthread_mutex_unlock(&s->lock);
    return ZST_ERROR;
}

static zst_result_t sdp_get_property(zst_element_t* el, const char* name,
                                      char* value_out, size_t max_len)
{
    if (!el || !name || !value_out) return ZST_ERROR;
    sdp_demuxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    if (strcmp(name, "sdp") == 0) {
        strncpy(value_out, s->sdp_text, max_len - 1);
        value_out[max_len - 1] = '\0';
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "jitter-buffer-ms") == 0 || strcmp(name, "jitter_buffer_ms") == 0) {
        snprintf(value_out, max_len, "%d", s->jitter_buffer_ms);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "reorder-capacity") == 0 || strcmp(name, "reorder_capacity") == 0) {
        snprintf(value_out, max_len, "%d", s->reorder_capacity);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "max-lateness-ms") == 0 || strcmp(name, "max_lateness_ms") == 0) {
        snprintf(value_out, max_len, "%d", s->max_lateness_ms);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "clock-rate-video") == 0 || strcmp(name, "clock_rate_video") == 0) {
        snprintf(value_out, max_len, "%d", s->default_clock_video);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (strcmp(name, "clock-rate-audio") == 0 || strcmp(name, "clock_rate_audio") == 0) {
        snprintf(value_out, max_len, "%d", s->default_clock_audio);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    pthread_mutex_unlock(&s->lock);
    return ZST_ERROR;
}

/*===========================================================================
    Element ops table
===========================================================================*/
static zst_element_ops_t g_ops = {
    .name          = "sdpdemux",
    .open          = sdp_open,
    .close         = sdp_close,
    .start         = sdp_start,
    .stop          = sdp_stop,
    .process       = sdp_process,
    .get_caps      = sdp_get_caps,
    .set_property  = sdp_set_property,
    .get_property  = sdp_get_property,
};

zst_element_t* zst_sdp_demuxer_create(void) {
    sdp_demuxer_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    pthread_mutex_init(&s->lock, NULL);

    /* Defaults */
    s->jitter_buffer_ms   = SDP_DEMUXER_DEFAULT_JITTER_MS;
    s->reorder_capacity   = SDP_DEMUXER_DEFAULT_REORDER;
    s->max_lateness_ms    = SDP_DEMUXER_DEFAULT_LATENESS_MS;
    s->default_clock_video = SDP_DEMUXER_DEFAULT_VIDEO_CLOCK;
    s->default_clock_audio = SDP_DEMUXER_DEFAULT_AUDIO_CLOCK;

    zst_element_t* el = zst_element_create(&g_ops, s);
    if (!el) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }

    /* Create the sink pad */
    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    if (!s->sink_pad) {
        pthread_mutex_destroy(&s->lock);
        zst_element_destroy(el);
        return NULL;
    }
    s->sink_pad->push = sdp_sink_push;

    if (zst_element_add_pad(el, s->sink_pad) != ZST_OK) {
        zst_pad_destroy(s->sink_pad);
        pthread_mutex_destroy(&s->lock);
        zst_element_destroy(el);
        return NULL;
    }

    /* Set template caps on sink pad */
    zst_caps_t* sink_caps = zst_caps_create();
    if (sink_caps) {
        zst_caps_append(sink_caps, zst_caps_struct_create_video("raw/rtp", 0, 0, 0, ""));
        zst_caps_append(sink_caps, zst_caps_struct_create_video("application/x-rtp", 0, 0, 0, ""));
        zst_pad_set_template_caps(s->sink_pad, sink_caps);
        zst_caps_destroy(sink_caps);
    }

    ZST_LOG_INFO("sdpdemux", "created SDP demuxer element");

    return el;
}

/*===========================================================================
    Plugin registration
===========================================================================*/
#ifdef BUILDING_PLUGIN

#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name) {
    if (strcmp(name, "sdpdemux") == 0) {
        return zst_sdp_demuxer_create();
    }
    return NULL;
}

static const zst_pad_template_t g_sdpdemux_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "raw/rtp;application/x-rtp" }
};

static const zst_property_spec_t g_sdpdemux_properties[] = {
    { "sdp",            ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SDP description text" },
    { "sdp-file",       ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Path to SDP file" },
    { "jitter-buffer-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "200", "Target jitter buffer depth in ms" },
    { "reorder-capacity", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "256", "Per-track RTP reorder buffer slots" },
    { "max-lateness-ms",  ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Max packet lateness before drop" },
    { "clock-rate-video", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "Default video RTP clock rate" },
    { "clock-rate-audio", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "48000", "Default audio RTP clock rate" }
};

static const zst_element_desc_t g_sdpdemux_elements[] = {
    {
        .name = "sdpdemux",
        .long_name = "SDP/RTP Demuxer",
        .category = "Demuxer/RTP",
        .description = "Parses SDP, reorders RTP packets by sequence number, depacketizes H.264/H.265/AAC, and outputs perfectly-timed media via DPLL clock drift compensation",
        .author = "zstreamer",
        .properties = g_sdpdemux_properties,
        .nb_properties = sizeof(g_sdpdemux_properties) / sizeof(g_sdpdemux_properties[0]),
        .pads = g_sdpdemux_pads,
        .nb_pads = sizeof(g_sdpdemux_pads) / sizeof(g_sdpdemux_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name    = "sdpdemux_plugin",
        .author  = "zstreamer",
        .version = "1.0.0",
        .init    = NULL,
        .deinit  = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_sdpdemux_elements) / sizeof(g_sdpdemux_elements[0]);
    }
    return g_sdpdemux_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}

#endif /* BUILDING_PLUGIN */
