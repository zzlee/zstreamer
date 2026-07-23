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
    svt_jpeg_xs_decoder_config_t config;

} st2110_22_depayloader_t;


static zst_result_t depayloader_process(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;

    zst_buffer_t* buf = NULL;
    zst_result_t res = zst_pad_pull(s->sink_pad, &buf);
    if (res != ZST_OK) {
        return res;
    }

    // Depayload RTP packet, decode using SVT-JPEG-XS
    // Dummy implementation

    zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t depayloader_change_state(zst_element_t* el, zst_state_t new_state) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    zst_state_t old_state = el->state;

    if (old_state == ZST_STATE_NULL && new_state == ZST_STATE_READY) {
        // Init decoder here
        svt_jpeg_xs_decoder_api_get_default_config(&s->config);
        // svt_jpeg_xs_decoder_api_init(&s->decoder, &s->config);
    } else if (old_state == ZST_STATE_READY && new_state == ZST_STATE_NULL) {
        // svt_jpeg_xs_decoder_api_close(s->decoder);
    }

    el->state = new_state;
    return ZST_OK;
}

static void depayloader_destroy(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    free(s);
    free(el);
}

zst_element_t* zst_st2110_22_depayloader_create(void) {
    zst_element_t* el = calloc(1, sizeof(zst_element_t));
    if (!el) return NULL;

    st2110_22_depayloader_t* s = calloc(1, sizeof(st2110_22_depayloader_t));
    if (!s) {
        free(el);
        return NULL;
    }
    el->priv = s;

    el->name = strdup("st2110_22_depayloader");
    el->process = depayloader_process;
    el->change_state = depayloader_change_state;
    el->destroy = depayloader_destroy;

    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK, el);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC, el);

    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    return el;
}
