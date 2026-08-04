/*=============================================================================
    st2110_21_depayloader.c — SMPTE ST 2110-21 H.264/H.265 Depayloader
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

typedef struct {
    int is_h265;
    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;
    
    uint8_t* au_data;
    size_t au_size;
    size_t au_capacity;
    uint64_t au_pts;
} st2110_21_depayloader_t;

static zst_result_t depayloader_set_property(zst_element_t* el, const char* name, const char* value) {
    st2110_21_depayloader_t* s = (st2110_21_depayloader_t*)el->priv;
    if (strcmp(name, "codec") == 0) {
        s->is_h265 = (strcmp(value, "h265") == 0);
        return ZST_OK;
    }
    return ZST_ERROR_NOT_IMPLEMENTED;
}

static zst_result_t depayloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    st2110_21_depayloader_t* s = (st2110_21_depayloader_t*)el->priv;
    if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->is_h265 ? "h265" : "h264");
        return ZST_OK;
    }
    return ZST_ERROR;
}

static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

static zst_result_t append_au(st2110_21_depayloader_t* s, const uint8_t* data, size_t len) {
    if (s->au_size + len > s->au_capacity) {
        size_t need = s->au_size + len;
        size_t new_cap = need - 1;
        new_cap |= new_cap >> 1;
        new_cap |= new_cap >> 2;
        new_cap |= new_cap >> 4;
        new_cap |= new_cap >> 8;
        new_cap |= new_cap >> 16;
#if SIZE_MAX > 0xFFFFFFFF
        new_cap |= new_cap >> 32;
#endif
        new_cap++;
        if (new_cap < 16384) new_cap = 16384;
        uint8_t* ptr = realloc(s->au_data, new_cap);
        if (!ptr) return ZST_ERROR;
        s->au_data = ptr;
        s->au_capacity = new_cap;
    }
    memcpy(s->au_data + s->au_size, data, len);
    s->au_size += len;
    return ZST_OK;
}

static zst_result_t push_au(st2110_21_depayloader_t* s) {
    if (s->au_size == 0) return ZST_OK;
    
    zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!out) return ZST_ERROR;
    
    out->memory.data = malloc(s->au_size);
    out->memory.size = s->au_size;
    out->memory.release = free;
    memcpy(out->memory.data, s->au_data, s->au_size);
    
    out->pts = s->au_pts;
    out->dts = s->au_pts;
    
    s->au_size = 0;
    
    zst_result_t ret = zst_pad_push(s->src_pad, out);
    zst_buffer_unref(out);
    return ret;
}

static zst_result_t push_nal(st2110_21_depayloader_t* s, const uint8_t* payload, int payload_len, uint64_t pts) {
    s->au_pts = pts;
    append_au(s, start_code, 4);
    append_au(s, payload, payload_len);
    return ZST_OK;
}

static zst_result_t handle_fu_chunk(st2110_21_depayloader_t* s, const uint8_t* payload, int payload_len, uint8_t* nal_header, int nal_header_len, int is_start, uint64_t pts) {
    if (is_start) {
        s->au_pts = pts;
        append_au(s, start_code, 4);
        if (nal_header_len > 0) {
            append_au(s, nal_header, nal_header_len);
        }
    }
    append_au(s, payload, payload_len);
    return ZST_OK;
}

static zst_result_t depayloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    st2110_21_depayloader_t* s = (st2110_21_depayloader_t*)el->priv;
    if (!in || !out) return ZST_ERROR_INVALID_ARGUMENT;
    *out = NULL;

    if (in->memory.size < 12) return ZST_OK;

    uint8_t* pkt = (uint8_t*)in->memory.data;
    int payload_offset = 12;
    int csrc_count = pkt[0] & 0x0F;
    payload_offset += csrc_count * 4;
    
    // Check for RTP extension
    if (pkt[0] & 0x10) {
        if (in->memory.size < payload_offset + 4) return ZST_OK;
        int ext_len = (pkt[payload_offset + 2] << 8 | pkt[payload_offset + 3]) * 4;
        payload_offset += 4 + ext_len;
    }

    if (in->memory.size <= payload_offset) return ZST_OK;
    int payload_len = in->memory.size - payload_offset;
    uint8_t* payload = pkt + payload_offset;

    if (!s->is_h265) {
        uint8_t nal_type = payload[0] & 0x1f;
        if (nal_type != 28 && nal_type != 24) {
            push_nal(s, payload, payload_len, in->pts);
        } else if (nal_type == 28) {
            if (payload_len < 2) return ZST_OK;
            uint8_t start = payload[1] & 0x80;
            uint8_t nal_header = (payload[0] & 0xe0) | (payload[1] & 0x1f);
            handle_fu_chunk(s, payload + 2, payload_len - 2, &nal_header, 1, start, in->pts);
        }
    } else {
        uint8_t nal_type = (payload[0] & 0x7E) >> 1;
        if (nal_type != 49 && nal_type != 48) {
            push_nal(s, payload, payload_len, in->pts);
        } else if (nal_type == 49) {
            if (payload_len < 3) return ZST_OK;
            uint8_t start = payload[2] & 0x80;
            uint8_t nal_header[2];
            nal_header[0] = (payload[0] & 0x81) | ((payload[2] & 0x3F) << 1);
            nal_header[1] = payload[1];
            handle_fu_chunk(s, payload + 3, payload_len - 3, nal_header, 2, start, in->pts);
        }
    }
    
    if (pkt[1] & 0x80) { // Marker bit
        push_au(s);
    }
    
    return ZST_OK;
}

static zst_result_t depayloader_start(zst_element_t* el) { return ZST_OK; }
static zst_result_t depayloader_stop(zst_element_t* el) {
    st2110_21_depayloader_t* s = (st2110_21_depayloader_t*)el->priv;
    if (s->au_data) {
        free(s->au_data);
        s->au_data = NULL;
    }
    s->au_capacity = 0;
    s->au_size = 0;
    return ZST_OK;
}

static const zst_element_ops_t g_depayloader_ops = {
    .name = "st2110_21_depayloader",
    .start = depayloader_start,
    .stop = depayloader_stop,
    .process = depayloader_process,
    .set_property = depayloader_set_property,
    .get_property = depayloader_get_property,
};

zst_element_t* zst_st2110_21_depayloader_create(void) {
    st2110_21_depayloader_t* s = calloc(1, sizeof(st2110_21_depayloader_t));
    zst_element_t* el = zst_element_create(&g_depayloader_ops, s);
    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);
    return el;
}
