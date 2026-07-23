/*=============================================================================
    st2110_22_payloader.c — SMPTE ST 2110-22 (JPEG XS) Video Payloader
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
#include "SvtJpegxsEnc.h"

typedef struct {
    int width;
    int height;
    int fps_num;
    int fps_den;
    int bpp;
    int rtp_pt;
    uint32_t ssrc;
    uint16_t seq;
    uint64_t packets;
    uint64_t bytes;

    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;

    svt_jpeg_xs_encoder_api_t* encoder;
    svt_jpeg_xs_image_config_t config;

    // Buffer tracking for SVT-JPEG-XS
} st2110_22_payloader_t;

static zst_result_t payloader_set_property(zst_element_t* el, const char* name, const char* value) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;

    if (strcmp(name, "width") == 0) {
        s->width = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "height") == 0) {
        s->height = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "fps-num") == 0) {
        s->fps_num = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "fps-den") == 0) {
        s->fps_den = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "bpp") == 0) {
        s->bpp = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "rtp-pt") == 0) {
        s->rtp_pt = atoi(value);
        return ZST_OK;
    }

    return ZST_ERROR_NOT_IMPLEMENTED;
}

static zst_result_t payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;

    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%d", s->width);
        return ZST_OK;
    }
    if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%d", s->height);
        return ZST_OK;
    }
    if (strcmp(name, "fps-num") == 0) {
        snprintf(value_out, max_len, "%d", s->fps_num);
        return ZST_OK;
    }
    if (strcmp(name, "fps-den") == 0) {
        snprintf(value_out, max_len, "%d", s->fps_den);
        return ZST_OK;
    }
    if (strcmp(name, "bpp") == 0) {
        snprintf(value_out, max_len, "%d", s->bpp);
        return ZST_OK;
    }
    if (strcmp(name, "rtp-pt") == 0) {
        snprintf(value_out, max_len, "%d", s->rtp_pt);
        return ZST_OK;
    }

    return ZST_ERROR_NOT_IMPLEMENTED;
}

static zst_result_t payloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el;
    (void)in;
    (void)out;
    // Process using SVT-JPEG-XS
    // Dummy stub implementation - packetization not implemented

    return ZST_OK;
}

static zst_result_t payloader_start(zst_element_t* el) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;
    // Init encoder here
    // svt_jpeg_xs_encoder_get_image_config(&s->config);
    // s->config.source_width = s->width;
    // s->config.source_height = s->height;
    // s->config.bpp = s->bpp;
    // Init config ...
    (void)s;

    // svt_jpeg_xs_encoder_api_init(&s->encoder, &s->config);
    return ZST_OK;
}

static zst_result_t payloader_stop(zst_element_t* el) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;
    // svt_jpeg_xs_encoder_api_close(s->encoder);
    (void)s;
    return ZST_OK;
}

static void payloader_free(zst_element_t* el) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;
    if (s->sink_pad) zst_pad_destroy(s->sink_pad);
    if (s->src_pad) zst_pad_destroy(s->src_pad);
    free(s);
}

static const zst_element_ops_t g_payloader_ops = {
    .name = "st2110_22_payloader",
    .start = payloader_start,
    .stop = payloader_stop,
    .process = payloader_process,
    .set_property = payloader_set_property,
    .get_property = payloader_get_property,
};

zst_element_t* zst_st2110_22_payloader_create(void) {
    st2110_22_payloader_t* s = calloc(1, sizeof(st2110_22_payloader_t));
    if (!s) {
        return NULL;
    }

    // Default configuration
    s->width = 1920;
    s->height = 1080;
    s->fps_num = 60;
    s->fps_den = 1;
    s->bpp = 3;
    s->rtp_pt = 96;
    s->ssrc = 0x12345678;

    zst_element_t* el = zst_element_create(&g_payloader_ops, s);
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
