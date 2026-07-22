#include "zstreamer/elements/zst_webrtc_twcc.h"
#include "zst_log.h"
#include <rtc/rtc.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>

#include <time.h>

#define TWCC_HISTORY_SIZE 8192

typedef struct {
    uint16_t seq_num;
    uint64_t send_time_us;
    size_t packet_size;
} twcc_sent_packet_t;

struct zst_webrtc_twcc {
    int pc_id;
    zst_bus_t* bus;
    int extmap_id;
    uint16_t seq_num;
    pthread_mutex_t lock;
    
    twcc_sent_packet_t history[TWCC_HISTORY_SIZE];
    uint64_t current_bitrate_bps;
    uint64_t last_update_us;
};

static uint64_t twcc_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void* zst_webrtc_twcc_process_incoming(zst_webrtc_twcc_t* twcc, const char *message, int size) {
    if (!twcc) return (void*)message;
    // Check if RTCP
    if (size >= 2) {
        uint8_t pt = (uint8_t)message[1];
        if (pt >= 192 && pt <= 223) {
            // RTCP packet
            if (pt == 205) { // RTPFB
                uint8_t fmt = message[0] & 0x1F;
                if (fmt == 15) { // Transport-wide Feedback Message
                    if (size >= 20) {
                        uint16_t base_seq = ntohs(*(uint16_t*)(message + 12));
                        uint16_t status_count = ntohs(*(uint16_t*)(message + 14));
                        
                        int lost_count = 0;
                        int recv_count = 0;
                        int offset = 20;
                        uint16_t pkts_processed = 0;
                        while (offset + 2 <= size && pkts_processed < status_count) {
                            uint16_t chunk = ntohs(*(uint16_t*)(message + offset));
                            offset += 2;
                            if ((chunk & 0x8000) == 0) { // Run length chunk
                                uint8_t status = (chunk >> 13) & 0x03;
                                uint16_t run_len = chunk & 0x1FFF;
                                if (status == 0) lost_count += run_len;
                                else recv_count += run_len;
                                pkts_processed += run_len;
                            } else { // Status vector chunk
                                int sym_size = (chunk & 0x4000) ? 2 : 1;
                                int num_sym = (sym_size == 1) ? 14 : 7;
                                for (int i = 0; i < num_sym && pkts_processed < status_count; i++) {
                                    int shift = (sym_size == 1) ? (13 - i) : (12 - i*2);
                                    uint8_t status = (chunk >> shift) & ((1 << sym_size) - 1);
                                    if (status == 0) lost_count++;
                                    else recv_count++;
                                    pkts_processed++;
                                }
                            }
                        }
                        
                        // Basic Loss-Based Congestion Control
                        uint64_t now_us = twcc_get_time_us();
                        if (now_us - twcc->last_update_us > 1000000ULL) { // 1 second
                            int total = lost_count + recv_count;
                            if (total > 0) {
                                float loss_rate = (float)lost_count / total;
                                if (loss_rate > 0.1f) {
                                    // High loss, reduce bitrate by 20%
                                    twcc->current_bitrate_bps = (uint64_t)(twcc->current_bitrate_bps * 0.8f);
                                } else if (loss_rate < 0.02f) {
                                    // Low loss, increase bitrate by 5%
                                    twcc->current_bitrate_bps = (uint64_t)(twcc->current_bitrate_bps * 1.05f);
                                }
                                
                                // Cap between 100kbps and 5Mbps
                                if (twcc->current_bitrate_bps < 100000) twcc->current_bitrate_bps = 100000;
                                if (twcc->current_bitrate_bps > 5000000) twcc->current_bitrate_bps = 5000000;
                                
                                ZST_LOG_INFO("twcc", "TWCC Feedback: base_seq=%u, lost=%d, recv=%d, loss=%.2f%% -> new target %lu bps", 
                                             base_seq, lost_count, recv_count, loss_rate * 100.0f, twcc->current_bitrate_bps);
                                             
                                zst_event_t* ev = zst_event_new_webrtc_remb(NULL, twcc->pc_id, twcc->current_bitrate_bps);
                                if (ev) zst_bus_post(twcc->bus, ev);
                            }
                            twcc->last_update_us = now_us;
                        }
                    }
                }
            }
        }
    }
    // We return the original message pointer so libdatachannel processes it normally
    return (void*)message;
}

void* zst_webrtc_twcc_process_outgoing(zst_webrtc_twcc_t* twcc, const char *message, int size) {
    if (!twcc || twcc->extmap_id < 0) {
        return (void*)message;
    }

    if (size < 12) return (void*)message;

    uint8_t v_p_x_cc = message[0];
    uint8_t version = (v_p_x_cc >> 6) & 0x03;
    if (version != 2) return (void*)message;

    uint8_t pt = message[1] & 0x7F;
    if (pt >= 64 && pt <= 95) {
        // This is RTCP, not RTP
        return (void*)message;
    }

    uint8_t x = (v_p_x_cc >> 4) & 0x01;
    uint8_t cc = v_p_x_cc & 0x0F;
    int csrc_len = cc * 4;
    if (size < 12 + csrc_len) return (void*)message;

    uint8_t new_pkt[2048];
    if (size + 8 > (int)sizeof(new_pkt)) {
        return (void*)message; // Too large to modify in-place
    }

    pthread_mutex_lock(&twcc->lock);
    uint16_t my_seq = twcc->seq_num++;
    
    // Track sent packet
    int idx = my_seq % TWCC_HISTORY_SIZE;
    twcc->history[idx].seq_num = my_seq;
    twcc->history[idx].send_time_us = twcc_get_time_us();
    twcc->history[idx].packet_size = size;
    
    pthread_mutex_unlock(&twcc->lock);

    int hdr_len = 12 + csrc_len;
    memcpy(new_pkt, message, hdr_len);

    int new_size = size;
    int ext_offset = hdr_len;

    if (x == 0) {
        // No existing extensions. We add a 0xBEDE extension block.
        new_pkt[0] |= 0x10; // Set X=1
        
        // Extension header: 0xBE DE <length in 32-bit words>
        new_pkt[ext_offset] = 0xBE;
        new_pkt[ext_offset+1] = 0xDE;
        new_pkt[ext_offset+2] = 0x00;
        new_pkt[ext_offset+3] = 0x01; // 1 word
        
        // Our TWCC extension (1-byte header)
        // Format: ID (4 bits) | length-1 (4 bits)
        // Length of TWCC data is 2 bytes (seq_num), so length-1 = 1
        new_pkt[ext_offset+4] = (twcc->extmap_id << 4) | 0x01;
        new_pkt[ext_offset+5] = (my_seq >> 8) & 0xFF;
        new_pkt[ext_offset+6] = my_seq & 0xFF;
        new_pkt[ext_offset+7] = 0x00; // padding
        
        // Copy the rest of the payload
        memcpy(new_pkt + ext_offset + 8, message + ext_offset, size - ext_offset);
        new_size += 8;
    } else {
        // There is an existing extension block. 
        // For simplicity in Phase 9, if there is already an extension (e.g. from libdatachannel?),
        // we could parse it and append. However, libdatachannel's packetizers (like H264)
        // do not currently add ANY header extensions by default unless configured.
        // If we encounter one, we'll just log and pass through for now, to be implemented.
        ZST_LOG_WARN("twcc", "Outgoing packet already has extensions. Appending not yet implemented.");
        return (void*)message;
    }

    return rtcCreateOpaqueMessage(new_pkt, new_size);
}

zst_webrtc_twcc_t* zst_webrtc_twcc_create(int pc_id, zst_bus_t* bus) {
    zst_webrtc_twcc_t* twcc = calloc(1, sizeof(zst_webrtc_twcc_t));
    twcc->pc_id = pc_id;
    twcc->bus = bus;
    twcc->extmap_id = -1;
    twcc->seq_num = 1;
    twcc->current_bitrate_bps = 2000000; // Start at 2Mbps
    twcc->last_update_us = twcc_get_time_us();
    pthread_mutex_init(&twcc->lock, NULL);

    return twcc;
}

void zst_webrtc_twcc_destroy(zst_webrtc_twcc_t* twcc) {
    if (!twcc) return;
    pthread_mutex_destroy(&twcc->lock);
    free(twcc);
}

int zst_webrtc_twcc_parse_offer(zst_webrtc_twcc_t* twcc, const char* offer_sdp) {
    // Look for: a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
    const char* ext_uri = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
    const char* p = strstr(offer_sdp, ext_uri);
    if (!p) {
        twcc->extmap_id = -1;
        return -1;
    }
    // Now look backwards for a=extmap:
    const char* extmap_str = "a=extmap:";
    const char* start = p;
    while (start > offer_sdp && *start != '\n') {
        start--;
    }
    if (*start == '\n') start++;
    
    if (strncmp(start, extmap_str, strlen(extmap_str)) == 0) {
        int id = atoi(start + strlen(extmap_str));
        twcc->extmap_id = id;
        ZST_LOG_INFO("twcc", "Parsed TWCC extmap ID from offer: %d", id);
        return id;
    }
    return -1;
}

int zst_webrtc_twcc_inject_answer(zst_webrtc_twcc_t* twcc, char* answer_sdp, size_t max_len) {
    if (twcc->extmap_id < 0) return 0; // Not negotiated
    
    // We need to inject a=extmap:<id> http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
    // into each m= section. For simplicity, we can insert it right after the m= line,
    // or we can append it at the end of each media section.
    
    char inj_buf[256];
    snprintf(inj_buf, sizeof(inj_buf), "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n", twcc->extmap_id);
    
    char temp[4096];
    temp[0] = '\0';
    
    char* line = strtok(answer_sdp, "\r\n");
    while (line) {
        strcat(temp, line);
        strcat(temp, "\r\n");
        if (strncmp(line, "c=IN ", 5) == 0) {
            // Good place to inject, right after connection info
            strcat(temp, inj_buf);
        }
        line = strtok(NULL, "\r\n");
    }
    
    if (strlen(temp) >= max_len) {
        return -1; // Too big
    }
    strcpy(answer_sdp, temp);
    return 0;
}
