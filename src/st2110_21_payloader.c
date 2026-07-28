/*=============================================================================
    st2110_21_payloader.c — SMPTE ST 2110-21 H.264/H.265 Payloader
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_log.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zst_timestamp_pacer.h"
#include "zst_media_utils.h"

typedef struct {
    int mtu;
    int rtp_pt;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t ext_seq_num;
    int fps_num;
    int fps_den;
    int is_h265;
    int pacer_enabled;

    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;
    zst_timestamp_pacer_t pacer;
} st2110_21_payloader_t;

static zst_result_t payloader_set_property(zst_element_t* el, const char* name, const char* value) {
    st2110_21_payloader_t* s = (st2110_21_payloader_t*)el->priv;
    if (strcmp(name, "mtu") == 0) { s->mtu = atoi(value); return ZST_OK; }
    if (strcmp(name, "rtp-pt") == 0) { s->rtp_pt = atoi(value); return ZST_OK; }
    if (strcmp(name, "fps-num") == 0) { s->fps_num = atoi(value); return ZST_OK; }
    if (strcmp(name, "fps-den") == 0) { s->fps_den = atoi(value); return ZST_OK; }
    if (strcmp(name, "pacing") == 0) { s->pacer_enabled = (strcmp(value, "on") == 0); return ZST_OK; }
    if (strcmp(name, "codec") == 0) { s->is_h265 = (strcmp(value, "h265") == 0); return ZST_OK; }
    return ZST_ERROR_NOT_IMPLEMENTED;
}

static zst_result_t payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    st2110_21_payloader_t* s = (st2110_21_payloader_t*)el->priv;
    if (strcmp(name, "mtu") == 0) { snprintf(value_out, max_len, "%d", s->mtu); return ZST_OK; }
    if (strcmp(name, "rtp-pt") == 0) { snprintf(value_out, max_len, "%d", s->rtp_pt); return ZST_OK; }
    if (strcmp(name, "codec") == 0) { snprintf(value_out, max_len, "%s", s->is_h265 ? "h265" : "h264"); return ZST_OK; }
    return ZST_ERROR;
}

static zst_buffer_t* make_packet(st2110_21_payloader_t* s, const uint8_t* hdr, int hdr_len, const uint8_t* payload, int payload_len, uint32_t rtp_ts, int marker, uint64_t pts) {
    size_t pkt_len = 12 + hdr_len + payload_len;
    zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!out) return NULL;
    
    out->memory.data = malloc(pkt_len);
    out->memory.size = pkt_len;
    out->memory.release = free;
    
    uint8_t* pkt = (uint8_t*)out->memory.data;
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((marker ? 0x80 : 0x00) | (s->rtp_pt & 0x7f));
    pkt[2] = (uint8_t)(s->seq >> 8);
    pkt[3] = (uint8_t)(s->seq & 0xff);
    pkt[4] = (uint8_t)(rtp_ts >> 24);
    pkt[5] = (uint8_t)(rtp_ts >> 16);
    pkt[6] = (uint8_t)(rtp_ts >> 8);
    pkt[7] = (uint8_t)(rtp_ts);
    pkt[8] = (uint8_t)(s->ssrc >> 24);
    pkt[9] = (uint8_t)(s->ssrc >> 16);
    pkt[10] = (uint8_t)(s->ssrc >> 8);
    pkt[11] = (uint8_t)(s->ssrc);
    
    if (hdr_len > 0) memcpy(pkt + 12, hdr, hdr_len);
    if (payload_len > 0) memcpy(pkt + 12 + hdr_len, payload, payload_len);
    
    out->pts = pts;
    out->dts = pts;
    
    s->seq++;
    if (s->seq == 0) s->ext_seq_num += 0x10000;
    return out;
}

static zst_result_t send_nal(st2110_21_payloader_t* s, zst_element_t* el, const uint8_t* nal, int nal_len, uint32_t ts, int marker, uint64_t pts, zst_time_t* pacing_ts, zst_time_t pacing_interval) {
    int max_payload = s->mtu - 12;
    if (nal_len <= max_payload) {
        zst_buffer_t* pkt = make_packet(s, NULL, 0, nal, nal_len, ts, marker, pts);
        if (s->pacer_enabled) zst_timestamp_pacer_wait(&s->pacer, el->pipeline ? el->pipeline->clock : NULL, *pacing_ts, NULL);
        *pacing_ts += pacing_interval;
        zst_result_t ret = zst_pad_push(s->src_pad, pkt);
        zst_buffer_unref(pkt);
        return ret;
    }

    if (!s->is_h265) {
        // H.264 FU-A
        uint8_t fu_ind = (nal[0] & 0xe0) | 28;
        uint8_t nal_type = nal[0] & 0x1f;
        int off = 1;
        int first = 1;
        
        while (off < nal_len) {
            int chunk = nal_len - off;
            if (chunk > max_payload - 2) chunk = max_payload - 2;
            
            uint8_t fu[2];
            fu[0] = fu_ind;
            fu[1] = nal_type;
            if (first) fu[1] |= 0x80;
            if (off + chunk >= nal_len) fu[1] |= 0x40;
            
            zst_buffer_t* pkt = make_packet(s, fu, 2, nal + off, chunk, ts, marker && (off + chunk >= nal_len), pts);
            if (s->pacer_enabled) zst_timestamp_pacer_wait(&s->pacer, el->pipeline ? el->pipeline->clock : NULL, *pacing_ts, NULL);
            *pacing_ts += pacing_interval;
            zst_pad_push(s->src_pad, pkt);
            zst_buffer_unref(pkt);
            
            off += chunk;
            first = 0;
        }
    } else {
        // H.265 FU
        uint8_t fu_ind[2];
        fu_ind[0] = (nal[0] & 0x81) | (49 << 1); // payload type 49
        fu_ind[1] = nal[1];
        uint8_t nal_type = (nal[0] & 0x7E) >> 1;
        int off = 2;
        int first = 1;
        
        while (off < nal_len) {
            int chunk = nal_len - off;
            if (chunk > max_payload - 3) chunk = max_payload - 3;
            
            uint8_t fu[3];
            fu[0] = fu_ind[0];
            fu[1] = fu_ind[1];
            fu[2] = nal_type;
            if (first) fu[2] |= 0x80;
            if (off + chunk >= nal_len) fu[2] |= 0x40;
            
            zst_buffer_t* pkt = make_packet(s, fu, 3, nal + off, chunk, ts, marker && (off + chunk >= nal_len), pts);
            if (s->pacer_enabled) zst_timestamp_pacer_wait(&s->pacer, el->pipeline ? el->pipeline->clock : NULL, *pacing_ts, NULL);
            *pacing_ts += pacing_interval;
            zst_pad_push(s->src_pad, pkt);
            zst_buffer_unref(pkt);
            
            off += chunk;
            first = 0;
        }
    }
    return ZST_OK;
}

static zst_result_t payloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    st2110_21_payloader_t* s = (st2110_21_payloader_t*)el->priv;
    if (!in || !out) return ZST_ERROR_INVALID_ARGUMENT;

    *out = NULL;
    uint8_t* data = (uint8_t*)in->memory.data;
    int size = in->memory.size;
    uint32_t ts = (uint32_t)(in->pts * 90000 / 1000000000ULL);
    
    // Quick estimation for pacing
    int num_packets = size / (s->mtu - 12) + 1;
    zst_time_t frame_duration_ns = s->fps_num > 0 ? (1000000000ULL * s->fps_den / s->fps_num) : 16666666ULL;
    zst_time_t pacing_interval = frame_duration_ns / num_packets;
    zst_time_t current_pacing_ts = in->pts;

    int code = 0;
    int pos = zst_find_start_code(data, size, 0, &code);
    if (pos < 0) {
        send_nal(s, el, data, size, ts, 1, in->pts, &current_pacing_ts, pacing_interval);
        return ZST_OK;
    }

    while (pos >= 0 && pos < size) {
        int nal_start = pos + code;
        int next_code = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        int is_last = next < 0;

        if (nal_len > 0) {
            send_nal(s, el, data + nal_start, nal_len, ts, is_last, in->pts, &current_pacing_ts, pacing_interval);
        }
        pos = next;
        code = next_code;
    }

    return ZST_OK;
}

static zst_result_t payloader_start(zst_element_t* el) {
    st2110_21_payloader_t* s = (st2110_21_payloader_t*)el->priv;
    zst_timestamp_pacer_init(&s->pacer);
    zst_timestamp_pacer_set_enabled(&s->pacer, s->pacer_enabled);
    return ZST_OK;
}

static zst_result_t payloader_stop(zst_element_t* el) {
    st2110_21_payloader_t* s = (st2110_21_payloader_t*)el->priv;
    zst_timestamp_pacer_deinit(&s->pacer);
    return ZST_OK;
}

static const zst_element_ops_t g_payloader_ops = {
    .name = "st2110_21_payloader",
    .start = payloader_start,
    .stop = payloader_stop,
    .process = payloader_process,
    .set_property = payloader_set_property,
    .get_property = payloader_get_property,
};

zst_element_t* zst_st2110_21_payloader_create(void) {
    st2110_21_payloader_t* s = calloc(1, sizeof(st2110_21_payloader_t));
    s->mtu = 1200;
    s->rtp_pt = 96;
    s->ssrc = 0x21102100;
    s->fps_num = 60;
    s->fps_den = 1;
    s->pacer_enabled = 1;
    
    zst_element_t* el = zst_element_create(&g_payloader_ops, s);
    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);
    return el;
}
