#define _POSIX_C_SOURCE 200809L

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
    int L; // Columns
    int D; // Rows
} st2110_fec_t;

static zst_result_t
fec_encoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    st2110_fec_t* s = el->priv;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    // TODO: Implement SMPTE ST 2022-5 FEC (Forward Error Correction) encoding.
    // For now, act as a passthrough.

    if (s->srcpad && s->srcpad->peer) {
        *out = in;
    } else {
        zst_buffer_unref(in);
    }
    return ZST_OK;
}

static zst_result_t
fec_decoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    st2110_fec_t* s = el->priv;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    // TODO: Implement SMPTE ST 2022-5 FEC decoding/recovery.
    // For now, act as a passthrough.

    if (s->srcpad && s->srcpad->peer) {
        *out = in;
    } else {
        zst_buffer_unref(in);
    }
    return ZST_OK;
}

static zst_result_t
fec_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_fec_t* s = el->priv;
    if (strcmp(name, "L") == 0) {
        s->L = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "D") == 0) {
        s->D = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
fec_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_fec_t* s = el->priv;
    if (strcmp(name, "L") == 0) {
        snprintf(value_out, max_len, "%d", s->L);
        return ZST_OK;
    } else if (strcmp(name, "D") == 0) {
        snprintf(value_out, max_len, "%d", s->D);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t encoder_ops = {
    .name = "st2110_fec_encoder",
    .process = fec_encoder_process,
    .set_property = fec_set_property,
    .get_property = fec_get_property,
};

static zst_element_ops_t decoder_ops = {
    .name = "st2110_fec_decoder",
    .process = fec_decoder_process,
    .set_property = fec_set_property,
    .get_property = fec_get_property,
};

zst_element_t*
zst_st2110_fec_encoder_create(void)
{
    st2110_fec_t* priv = calloc(1, sizeof(*priv));
    priv->L = 10;
    priv->D = 10;

    zst_element_t* el = zst_element_create(&encoder_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

zst_element_t*
zst_st2110_fec_decoder_create(void)
{
    st2110_fec_t* priv = calloc(1, sizeof(*priv));
    priv->L = 10;
    priv->D = 10;

    zst_element_t* el = zst_element_create(&decoder_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}
