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


static void depayloader_buf_free(zst_buffer_t* b) {
    if (b && b->memory.data) {
        free(b->memory.data);
        b->memory.data = NULL;
    }
}

static zst_result_t depayloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;

    if (!in || !out) return ZST_ERROR_INVALID_ARGUMENT;
    if (!s->decoder) return ZST_ERROR;

    if (!s->config.components[0].byte_size) {
        // Init decoder on first packet
        SvtJxsErrorType_t err = svt_jpeg_xs_decoder_init(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, 
                                                         s->decoder, in->memory.data, in->memory.size, &s->config);
        if (err != SvtJxsErrorNone) {
            ZST_LOG_ERROR("st2110_22_depay", "SVT-JPEG-XS decoder init failed: %d", err);
            return ZST_ERROR;
        }
    }

    if (in->memory.size == 0) {
        *out = NULL;
        return ZST_OK;
    }

    svt_jpeg_xs_frame_t dec_input;
    memset(&dec_input, 0, sizeof(dec_input));
    dec_input.bitstream.buffer = in->memory.data;
    dec_input.bitstream.allocation_size = in->memory.size;
    dec_input.bitstream.used_size = in->memory.size;

    // Allocate buffer for decoded YUV frame (I422)
    uint32_t w = s->config.components[0].width;
    uint32_t h = s->config.components[0].height;
    size_t out_size = w * h * 2;
    zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!out_buf) return ZST_ERROR;
    
    out_buf->memory.data = malloc(out_size);
    if (!out_buf->memory.data) {
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }
    
    uint8_t* raw = (uint8_t*)out_buf->memory.data;
    
    dec_input.image.data_yuv[0] = raw;
    dec_input.image.alloc_size[0] = w * h;
    dec_input.image.stride[0] = w;

    dec_input.image.data_yuv[1] = raw + (w * h);
    dec_input.image.alloc_size[1] = (w * h) / 2;
    dec_input.image.stride[1] = w / 2;

    dec_input.image.data_yuv[2] = raw + (w * h) + (w * h) / 2;
    dec_input.image.alloc_size[2] = (w * h) / 2;
    dec_input.image.stride[2] = w / 2;

    SvtJxsErrorType_t err = svt_jpeg_xs_decoder_send_frame(s->decoder, &dec_input, 1);
    if (err != SvtJxsErrorNone) {
        ZST_LOG_ERROR("st2110_22_depay", "SVT-JPEG-XS decoder send frame failed: %d", err);
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    svt_jpeg_xs_frame_t dec_output;
    memset(&dec_output, 0, sizeof(dec_output));
    
    err = svt_jpeg_xs_decoder_get_frame(s->decoder, &dec_output, 1);
    if (err == (SvtJxsErrorType_t)0x80002033) { // SvtJxsErrorNoErrorEmptyQueue
        zst_buffer_unref(out_buf);
        *out = NULL;
        return ZST_OK;
    } else if (err != SvtJxsErrorNone) {
        ZST_LOG_ERROR("st2110_22_depay", "SVT-JPEG-XS decoder get frame failed: %d", err);
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    out_buf->memory.size = out_size;
    out_buf->pts = in->pts;
    out_buf->dts = in->dts;
    out_buf->destroy = depayloader_buf_free;
    *out = out_buf;

    return ZST_OK;
}

static zst_result_t depayloader_start(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;

    s->decoder = calloc(1, sizeof(svt_jpeg_xs_decoder_api_t));
    if (!s->decoder) return ZST_ERROR;

    // Decoder initialization is usually deferred until we have a bitstream codestream header
    // But we allocate the structure here.
    return ZST_OK;
}

static zst_result_t depayloader_stop(zst_element_t* el) {
    st2110_22_depayloader_t* s = (st2110_22_depayloader_t*)el->priv;
    if (s->decoder) {
        // Assume svt_jpeg_xs_decoder_close exists
        svt_jpeg_xs_decoder_close(s->decoder);
        free(s->decoder);
        s->decoder = NULL;
    }
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
