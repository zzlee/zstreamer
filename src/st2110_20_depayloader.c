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
} st2110_20_depayloader_t;

static zst_result_t
st2110_20_depayloader_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_20_depayloader_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    /* Dummy implementation: just drop the buffer or pass it through as dummy raw video */
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

    ZST_LOG_DEBUG("st2110_20_depay", "Depayloaded packet of size %zu (dummy)", buf->memory.size);
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
