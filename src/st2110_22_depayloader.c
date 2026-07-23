/*=============================================================================
    st2110_22_depayloader.c — SMPTE ST 2110-22 (JPEG XS) Video Depayloader
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
#include "zstreamer/elements/zst_st2110_22.h"
#include "SvtJpegxsDec.h"

typedef struct {
    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;

    svt_jpeg_xs_decoder_api_t* decoder;
    svt_jpeg_xs_image_config_t config;

} st2110_22_depayloader_t;


static zst_result_t depayloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el;
    (void)in;
    (void)out;
    // Depayload RTP packet, decode using SVT-JPEG-XS
    // Dummy implementation

    return ZST_OK;
}

static zst_result_t depayloader_start(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    // Init decoder here
    // svt_jpeg_xs_decoder_api_get_default_config(&s->config);
    // svt_jpeg_xs_decoder_api_init(&s->decoder, &s->config);
    (void)s;
    return ZST_OK;
}

static zst_result_t depayloader_stop(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    // svt_jpeg_xs_decoder_api_close(s->decoder);
    (void)s;
    return ZST_OK;
}

static void depayloader_free(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    if (s->sink_pad) zst_pad_destroy(s->sink_pad);
    if (s->src_pad) zst_pad_destroy(s->src_pad);
    free(s);
}

static const zst_element_ops_t g_depayloader_ops = {
    .name = "st2110_22_depayloader",
    .start = depayloader_start,
    .stop = depayloader_stop,
    .process = depayloader_process,
};

zst_element_t* zst_st2110_22_depayloader_create(void) {
    st2110_22_depayloader_t* s = calloc(1, sizeof(st2110_22_depayloader_t));
    if (!s) {
        return NULL;
    }

    zst_element_t* el = zst_element_create(&g_depayloader_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }

    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);

    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    return el;
}
