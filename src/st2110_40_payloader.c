#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_st2110_40.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"

typedef struct {
    char* aux_data_type;
    int sampling_frequency;

    uint32_t ssrc;
    uint16_t seq;
    uint32_t ts;

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
} st2110_40_payloader_t;

static zst_result_t
payloader_open(zst_element_t* el)
{
    st2110_40_payloader_t* s = el->priv;
    s->seq = rand() & 0xFFFF;
    s->ssrc = 0x21104000 | (rand() & 0xFFF);
    return ZST_OK;
}

static zst_result_t
payloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    st2110_40_payloader_t* s = el->priv;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    size_t in_size = in->memory.size;
    uint8_t* in_data = in->memory.data;

    zst_buffer_t* rtp = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!rtp) {
        zst_buffer_unref(in);
        return ZST_ERROR;
    }

    rtp->memory.size = 12 + in_size;
    rtp->memory.data = malloc(rtp->memory.size);
    rtp->memory.release = free;
    rtp->memory.priv = rtp->memory.data;

    uint8_t* out_data = rtp->memory.data;
    out_data[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
    out_data[1] = 97; /* M=0, PT=97 (placeholder) */

    uint16_t seq = htons(s->seq++);
    memcpy(out_data + 2, &seq, 2);

    uint32_t current_rtp_ts = (uint32_t)(in->pts * 90000 / 1000000000ULL);
    uint32_t ts = htonl(current_rtp_ts);
    memcpy(out_data + 4, &ts, 4);

    uint32_t ssrc = htonl(s->ssrc);
    memcpy(out_data + 8, &ssrc, 4);

    if (in_data) {
        memcpy(out_data + 12, in_data, in_size);
    } else {
        memset(out_data + 12, 0, in_size);
    }

    rtp->pts = in->pts;
    rtp->dts = rtp->pts;

    *out = rtp;

    zst_buffer_unref(in);
    return ZST_OK;
}

static zst_result_t
payloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_40_payloader_t* s = el->priv;
    if (strcmp(name, "aux-data-type") == 0) {
        if (s->aux_data_type) free(s->aux_data_type);
        s->aux_data_type = strdup(value);
    } else if (strcmp(name, "sampling-frequency") == 0) {
        s->sampling_frequency = atoi(value);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_40_payloader_t* s = el->priv;
    if (strcmp(name, "aux-data-type") == 0) {
        snprintf(value_out, max_len, "%s", s->aux_data_type ? s->aux_data_type : "");
    } else if (strcmp(name, "sampling-frequency") == 0) {
        snprintf(value_out, max_len, "%d", s->sampling_frequency);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_caps_t*
payloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    st2110_40_payloader_t* s = el->priv;
    zst_caps_t* caps = NULL;

    if (pad == s->sinkpad) {
        caps = zst_caps_create();
    } else if (pad == s->srcpad) {
        caps = zst_caps_new_simple("application/x-rtp");
    }
    return caps;
}

static zst_result_t
payloader_close(zst_element_t* el)
{
    st2110_40_payloader_t* s = el->priv;
    if (s->aux_data_type) free(s->aux_data_type);
    return ZST_OK;
}

static zst_element_ops_t payloader_ops = {
    .name = "st2110_40_payloader",
    .open = payloader_open,
    .close = payloader_close,
    .process = payloader_process,
    .set_property = payloader_set_property,
    .get_property = payloader_get_property,
    .get_caps = payloader_get_caps,
};

zst_element_t*
zst_st2110_40_payloader_create(void)
{
    st2110_40_payloader_t* priv = calloc(1, sizeof(*priv));
    priv->aux_data_type = strdup("cea608");
    priv->sampling_frequency = 48000;

    zst_element_t* el = zst_element_create(&payloader_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}
