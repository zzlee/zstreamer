/*=============================================================================
    st2110_20_depayloader.c — SMPTE ST 2110-20 Video Depayloader
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
    int expected_line_length;
    int reorder_buffer_depth;
    int reorder_timeout_ms;
    
    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;

    zst_buffer_t* current_frame;
    uint32_t current_ts;
    int max_y;
    int bpp;
} st2110_20_depayloader_t;

static zst_result_t
st2110_20_depayloader_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_20_depayloader_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->src_pad && s->src_pad->peer) {
            zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
            if (out) {
                out->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(s->src_pad, out);
                zst_buffer_unref(out);
            }
        }
        return ZST_OK;
    }

    if (buf->memory.size < 20) return ZST_ERROR;

    uint8_t* data = buf->memory.data;
    bool m_bit = (data[1] & 0x80) != 0;
    uint32_t ts = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

    if (!s->current_frame || s->current_ts != ts) {
        if (s->current_frame) {
            if (s->expected_line_length > 0) {
                s->current_frame->memory.size = s->expected_line_length * (s->max_y + 1);
            } else {
                s->current_frame->memory.size = (1920 * s->bpp) * (s->max_y + 1);
            }
            zst_pad_push(s->src_pad, s->current_frame);
            zst_buffer_unref(s->current_frame);
        }

        s->current_frame = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!s->current_frame) return ZST_ERROR;

        s->current_frame->memory.size = 8 * 1024 * 1024; // max 8MB
        s->current_frame->memory.data = malloc(s->current_frame->memory.size);
        if (!s->current_frame->memory.data) {
            zst_buffer_unref(s->current_frame);
            s->current_frame = NULL;
            return ZST_ERROR;
        }
        s->current_frame->memory.priv = s->current_frame->memory.data;
        s->current_frame->memory.release = free;
        s->current_frame->pts = ts;
        s->current_ts = ts;
        s->max_y = 0;
        s->bpp = 2; // Default for I422_BE
    }

    struct {
        int length;
        int line_no;
        int p_offset;
    } headers[32];
    int num_headers = 0;
    int header_offset = 14;

    while (header_offset + 6 <= buf->memory.size && num_headers < 32) {
        uint16_t length = (data[header_offset] << 8) | data[header_offset+1];
        uint16_t f_line = (data[header_offset+2] << 8) | data[header_offset+3];
        uint16_t c_off  = (data[header_offset+4] << 8) | data[header_offset+5];
        
        headers[num_headers].length = length;
        headers[num_headers].line_no = f_line & 0x7FFF;
        headers[num_headers].p_offset = c_off & 0x7FFF;
        bool c_bit = (c_off & 0x8000) != 0;
        
        num_headers++;
        header_offset += 6;
        
        if (!c_bit) break;
    }

    int payload_offset = header_offset;
    for (int i = 0; i < num_headers; i++) {
        int line_no = headers[i].line_no;
        int p_offset = headers[i].p_offset;
        int len = headers[i].length;
        
        if (payload_offset + len > buf->memory.size) {
            break; // Malformed packet
        }
        
        if (line_no > s->max_y) {
            s->max_y = line_no;
        }
        
        int line_stride = s->expected_line_length > 0 ? s->expected_line_length : (1920 * s->bpp);
        int dest_pos = (line_no * line_stride) + (p_offset * s->bpp);
        
        if (dest_pos + len <= s->current_frame->memory.size) {
            memcpy(s->current_frame->memory.data + dest_pos, data + payload_offset, len);
        }
        
        payload_offset += len;
    }

    if (m_bit) {
        if (s->expected_line_length > 0) {
            s->current_frame->memory.size = s->expected_line_length * (s->max_y + 1);
        } else {
            s->current_frame->memory.size = (1920 * s->bpp) * (s->max_y + 1);
        }
        s->current_frame->flags |= (buf->flags & ZST_BUFFER_FLAG_EOS);
        zst_pad_push(s->src_pad, s->current_frame);
        zst_buffer_unref(s->current_frame);
        s->current_frame = NULL;
    }

    return ZST_OK;
}

static zst_result_t
st2110_20_depayloader_open(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_result_t
st2110_20_depayloader_close(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_caps_t*
st2110_20_depayloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad && pad->direction == ZST_PAD_SINK) {
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
st2110_20_depayloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_20_depayloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "expected-line-length") == 0) {
        s->expected_line_length = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "reorder-buffer-depth") == 0) {
        s->reorder_buffer_depth = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "reorder-timeout-ms") == 0) {
        s->reorder_timeout_ms = atoi(value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
st2110_20_depayloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_20_depayloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "expected-line-length") == 0) {
        snprintf(value_out, max_len, "%d", s->expected_line_length);
    } else if (strcmp(name, "reorder-buffer-depth") == 0) {
        snprintf(value_out, max_len, "%d", s->reorder_buffer_depth);
    } else if (strcmp(name, "reorder-timeout-ms") == 0) {
        snprintf(value_out, max_len, "%d", s->reorder_timeout_ms);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_depayloader_ops = {
    .name = "st2110_20_depayloader",
    .open = st2110_20_depayloader_open,
    .close = st2110_20_depayloader_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = st2110_20_depayloader_get_caps,
    .set_property = st2110_20_depayloader_set_property,
    .get_property = st2110_20_depayloader_get_property,
};

zst_element_t*
zst_st2110_20_depayloader_create(void)
{
    st2110_20_depayloader_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->reorder_buffer_depth = 100;
    s->reorder_timeout_ms = 100;
    s->expected_line_length = 0;

    zst_element_t* el = zst_element_create(&g_depayloader_ops, s);
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

    s->sink_pad->push = st2110_20_depayloader_pad_push;
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    ZST_LOG_INFO("st2110_20_depay", "created ST2110-20 depayloader element");
    return el;
}
