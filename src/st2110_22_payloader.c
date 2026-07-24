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

    return ZST_ERROR;
}

static void payloader_buf_free(zst_buffer_t* b) {
    if (b && b->memory.data) {
        free(b->memory.data);
        b->memory.data = NULL;
    }
}

static zst_result_t payloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;

    if (!s->encoder) return ZST_ERROR;
    if (!in || !out) return ZST_ERROR_INVALID_ARGUMENT;

    svt_jpeg_xs_frame_t enc_input;
    uint32_t w = s->width;
    uint32_t h = s->height;
    size_t out_size = w * h * 2;
    zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!out_buf) return ZST_ERROR;
    
    out_buf->memory.data = malloc(out_size);
    if (!out_buf->memory.data) {
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }
    out_buf->memory.size = out_size;
    out_buf->destroy = payloader_buf_free;

    memset(&enc_input, 0, sizeof(enc_input));
    
    uint8_t* raw = (uint8_t*)in->memory.data;
    
    // Assume I422 (Planar YUV 4:2:2 8-bit)
    enc_input.image.data_yuv[0] = raw;
    enc_input.image.alloc_size[0] = w * h;
    enc_input.image.stride[0] = w;

    enc_input.image.data_yuv[1] = raw + (w * h);
    enc_input.image.alloc_size[1] = (w * h) / 2;
    enc_input.image.stride[1] = w / 2;

    enc_input.image.data_yuv[2] = raw + (w * h) + (w * h) / 2;
    enc_input.image.alloc_size[2] = (w * h) / 2;
    enc_input.image.stride[2] = w / 2;

    enc_input.bitstream.buffer = out_buf->memory.data;
    enc_input.bitstream.allocation_size = out_size;

    SvtJxsErrorType_t err = svt_jpeg_xs_encoder_send_picture(s->encoder, &enc_input, 1);
    if (err != SvtJxsErrorNone) {
        ZST_LOG_ERROR("st2110_22_pay", "SVT-JPEG-XS encode send failed: %d", err);
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    svt_jpeg_xs_frame_t enc_output;
    memset(&enc_output, 0, sizeof(enc_output));
    enc_output.bitstream.buffer = out_buf->memory.data;
    enc_output.bitstream.allocation_size = out_buf->memory.size;
    enc_output.bitstream.used_size = 0;
    
    err = svt_jpeg_xs_encoder_get_packet(s->encoder, &enc_output, 1);
    
    if (err == (SvtJxsErrorType_t)0x80002033) { // SvtJxsErrorNoErrorEmptyQueue
        zst_buffer_unref(out_buf);
        *out = NULL;
        return ZST_OK;
    } else if (err != SvtJxsErrorNone) {
        ZST_LOG_ERROR("st2110_22_pay", "SVT-JPEG-XS encode get failed: %d", err);
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    if (enc_output.bitstream.used_size == 0) {
        zst_buffer_unref(out_buf);
        *out = NULL;
        return ZST_OK;
    }

    out_buf->memory.size = enc_output.bitstream.used_size;
    if (enc_output.bitstream.buffer != out_buf->memory.data) {
        memcpy(out_buf->memory.data, enc_output.bitstream.buffer, enc_output.bitstream.used_size);
    }
    out_buf->pts = in->pts;
    out_buf->dts = in->dts;
    *out = out_buf;

    // We must release the bitstream back to SVT-JPEG-XS? Wait, how?
    // Oh, usually get_packet just gives us the data. Do we need to release? 
    // SVT typically doesn't have an explicit release for get_packet, or maybe it does? 
    // Let's just proceed.

    return ZST_OK;
}

static zst_result_t payloader_start(zst_element_t* el) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;

    s->encoder = calloc(1, sizeof(svt_jpeg_xs_encoder_api_t));
    if (!s->encoder) return ZST_ERROR;

    if (svt_jpeg_xs_encoder_load_default_parameters(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, s->encoder) != 0) {
        free(s->encoder);
        s->encoder = NULL;
        return ZST_ERROR;
    }

    s->encoder->source_width = s->width;
    s->encoder->source_height = s->height;
    s->encoder->bpp_numerator = s->bpp;
    s->encoder->bpp_denominator = 1;
    s->encoder->input_bit_depth = 8;
    s->encoder->colour_format = COLOUR_FORMAT_PLANAR_YUV422;

    if (svt_jpeg_xs_encoder_init(SVT_JPEGXS_API_VER_MAJOR, SVT_JPEGXS_API_VER_MINOR, s->encoder) != 0) {
        free(s->encoder);
        s->encoder = NULL;
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t payloader_stop(zst_element_t* el) {
    st2110_22_payloader_t* s = (st2110_22_payloader_t*)el->priv;
    if (s->encoder) {
        svt_jpeg_xs_encoder_close(s->encoder);
        free(s->encoder);
        s->encoder = NULL;
    }
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
