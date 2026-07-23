#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_st2110_40.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"

typedef struct {
    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
} st2110_40_depayloader_t;

static zst_result_t
depayloader_open(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_result_t
depayloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)el;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    /* Basic depayloader stub */
    size_t in_size = in->memory.size;
    uint8_t* in_data = in->memory.data;

    if (in_size <= 12) {
        zst_buffer_unref(in);
        return ZST_OK; /* Drop invalid or empty RTP packet */
    }

    size_t payload_size = in_size - 12;
    zst_buffer_t* data_buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!data_buf) {
        zst_buffer_unref(in);
        return ZST_ERROR;
    }

    data_buf->memory.size = payload_size;
    data_buf->memory.data = malloc(payload_size);
    data_buf->memory.release = free;
    data_buf->memory.priv = data_buf->memory.data;

    memcpy(data_buf->memory.data, in_data + 12, payload_size);

    data_buf->pts = in->pts;
    data_buf->dts = in->dts;

    *out = data_buf;
    zst_buffer_unref(in);

    return ZST_OK;
}

static zst_caps_t*
depayloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    st2110_40_depayloader_t* s = el->priv;
    zst_caps_t* caps = NULL;

    if (pad == s->sinkpad) {
        caps = zst_caps_new_simple("application/x-rtp");
    } else if (pad == s->srcpad) {
        caps = zst_caps_create();
    }
    return caps;
}

static zst_element_ops_t depayloader_ops = {
    .name = "st2110_40_depayloader",
    .open = depayloader_open,
    .process = depayloader_process,
    .get_caps = depayloader_get_caps,
};

zst_element_t*
zst_st2110_40_depayloader_create(void)
{
    st2110_40_depayloader_t* priv = calloc(1, sizeof(*priv));

    zst_element_t* el = zst_element_create(&depayloader_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}
