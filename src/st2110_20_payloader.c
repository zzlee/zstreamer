/*=============================================================================
    st2110_20_payloader.c — SMPTE ST 2110-20 Video Payloader
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
#include "zstreamer/elements/zst_st2110_20.h"

typedef struct {
    int width;
    int height;
    char sampling[32]; // "YCbCr-4:2:2", "YCbCr-4:4:4", "RGB"
    int rtp_pt;
    uint32_t ssrc;
    uint32_t seq;
    uint64_t packets;
    uint64_t bytes;

    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;
} st2110_20_payloader_t;

static zst_result_t
st2110_20_payloader_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_20_payloader_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->src_pad && s->src_pad->peer) {
            zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_USER);
            if (out) {
                out->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(s->src_pad, out);
                zst_buffer_unref(out);
            }
        }
        return ZST_OK;
    }

    int bpp = 2; // Default for YCbCr-4:2:2 8-bit
    if (strcmp(s->sampling, "RGB") == 0 || strcmp(s->sampling, "YCbCr-4:4:4") == 0) {
        bpp = 3;
    }
    if (s->width > 0 && s->height > 0 && buf->memory.size > 0) {
        bpp = buf->memory.size / (s->width * s->height);
        if (bpp == 0) bpp = 2;
    }

    int max_payload_bytes = 1376;
    max_payload_bytes -= (max_payload_bytes % bpp); // Pixel alignment

    for (int y = 0; y < s->height; y++) {
        int x_offset_pixels = 0;
        int pixels_per_line = s->width;
        
        while (x_offset_pixels < pixels_per_line) {
            int pixels_to_send = pixels_per_line - x_offset_pixels;
            int max_pixels = max_payload_bytes / bpp;
            if (pixels_to_send > max_pixels) {
                pixels_to_send = max_pixels;
            }
            int bytes_to_send = pixels_to_send * bpp;
            
            bool is_last_packet = (y == s->height - 1) && ((x_offset_pixels + pixels_to_send) == pixels_per_line);
            
            zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_MEMORY);
            if (!out) return ZST_ERROR;
            zst_buffer_alloc_memory(out, 12 + 8 + bytes_to_send);
            
            uint8_t* data = out->memory.data;
            
            // 1. RTP Header (12 bytes)
            data[0] = 0x80; // V=2, P=0, X=0, CC=0
            data[1] = (s->rtp_pt & 0x7F) | (is_last_packet ? 0x80 : 0x00); // M bit
            uint16_t rtp_seq = s->seq & 0xFFFF;
            data[2] = rtp_seq >> 8;
            data[3] = rtp_seq & 0xFF;
            uint32_t ts = buf->pts; // Assuming 90kHz timestamp is mapped to buf->pts
            data[4] = ts >> 24;
            data[5] = (ts >> 16) & 0xFF;
            data[6] = (ts >> 8) & 0xFF;
            data[7] = ts & 0xFF;
            uint32_t ssrc = s->ssrc;
            data[8] = ssrc >> 24;
            data[9] = (ssrc >> 16) & 0xFF;
            data[10] = (ssrc >> 8) & 0xFF;
            data[11] = ssrc & 0xFF;
            
            // 2. Extended Sequence Number (2 bytes)
            uint16_t ext_seq = (s->seq >> 16) & 0xFFFF;
            data[12] = ext_seq >> 8;
            data[13] = ext_seq & 0xFF;
            
            // 3. Line Header (6 bytes)
            uint16_t length_field = bytes_to_send;
            data[14] = length_field >> 8;
            data[15] = length_field & 0xFF;
            uint16_t f_line = (0 << 15) | (y & 0x7FFF); // F=0
            data[16] = f_line >> 8;
            data[17] = f_line & 0xFF;
            uint16_t c_offset = (0 << 15) | (x_offset_pixels & 0x7FFF); // C=0
            data[18] = c_offset >> 8;
            data[19] = c_offset & 0xFF;
            
            // 4. Payload Data
            uint8_t* src_ptr = buf->memory.data + (y * s->width * bpp) + (x_offset_pixels * bpp);
            memcpy(data + 20, src_ptr, bytes_to_send);
            
            out->pts = buf->pts;
            if (is_last_packet) {
                out->flags |= (buf->flags & ZST_BUFFER_FLAG_EOS);
            }
            
            zst_pad_push(s->src_pad, out);
            zst_buffer_unref(out);
            
            s->seq++;
            s->packets++;
            s->bytes += (12 + 8 + bytes_to_send);
            
            x_offset_pixels += pixels_to_send;
        }
    }

    return ZST_OK;
}

static zst_result_t
st2110_20_payloader_open(zst_element_t* el)
{
    st2110_20_payloader_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    s->seq = 0;
    s->packets = 0;
    s->bytes = 0;
    return ZST_OK;
}

static zst_result_t
st2110_20_payloader_close(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_caps_t*
st2110_20_payloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad && pad->direction == ZST_PAD_SRC) {
        zst_caps_struct_t* c = calloc(1, sizeof(*c));
        if (c) {
            strncpy(c->media_type, "application/x-rtp", sizeof(c->media_type) - 1);
            c->type = ZST_CAPS_ANY;
            zst_caps_append(caps, c);
        }
    } else {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "I422_BE"));
    }
    return caps;
}

static zst_result_t
st2110_20_payloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_20_payloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "width") == 0) {
        s->width = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "height") == 0) {
        s->height = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "sampling") == 0) {
        strncpy(s->sampling, value, sizeof(s->sampling) - 1);
        return ZST_OK;
    }
    if (strcmp(name, "rtp-pt") == 0) {
        s->rtp_pt = atoi(value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
st2110_20_payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_20_payloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%d", s->width);
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%d", s->height);
    } else if (strcmp(name, "sampling") == 0) {
        snprintf(value_out, max_len, "%s", s->sampling);
    } else if (strcmp(name, "rtp-pt") == 0) {
        snprintf(value_out, max_len, "%d", s->rtp_pt);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_payloader_ops = {
    .name = "st2110_20_payloader",
    .open = st2110_20_payloader_open,
    .close = st2110_20_payloader_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = st2110_20_payloader_get_caps,
    .set_property = st2110_20_payloader_set_property,
    .get_property = st2110_20_payloader_get_property,
};

zst_element_t*
zst_st2110_20_payloader_create(void)
{
    st2110_20_payloader_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->width = 1920;
    s->height = 1080;
    strncpy(s->sampling, "YCbCr-4:2:2", sizeof(s->sampling) - 1);
    s->rtp_pt = 96;

    zst_element_t* el = zst_element_create(&g_payloader_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }

    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!s->sink_pad || !s->src_pad) {
        if (s->sink_pad) zst_pad_destroy(s->sink_pad);
        if (s->src_pad) zst_pad_destroy(s->src_pad);
        zst_element_destroy(el);
        return NULL;
    }

    s->sink_pad->push = st2110_20_payloader_pad_push;
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    ZST_LOG_INFO("st2110_20_pay", "created ST2110-20 payloader element");
    return el;
}
