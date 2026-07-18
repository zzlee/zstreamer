/*=============================================================================
    vp8_encoder.c — FFmpeg libavcodec VP8 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    uint32_t        width;
    uint32_t        height;

    int64_t         bitrate;
    int             gop_size;
    int             fps_num;
    int             fps_den;
    int             threads;
    int64_t         frame_count;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;
} vp8_encoder_t;

static zst_result_t
vp8_open(zst_element_t* el)
{
    vp8_encoder_t* s = el->priv;
    s->initialized = 0;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->frame_count = 0;
    return ZST_OK;
}

static zst_result_t
vp8_close(zst_element_t* el)
{
    vp8_encoder_t* s = el->priv;
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->frame) {
        av_frame_free(&s->frame);
        s->frame = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
vp8_init_encoder(vp8_encoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
    if (!codec) {
        fprintf(stderr, "vp8_encoder: VP8 encoder not found (libvpx needed)\n");
        return ZST_ERROR;
    }

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->codec_ctx->width = width;
    s->codec_ctx->height = height;
    s->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    s->codec_ctx->codec_id = AV_CODEC_ID_VP8;

    s->codec_ctx->framerate.num = s->fps_num > 0 ? s->fps_num : 30;
    s->codec_ctx->framerate.den = s->fps_den > 0 ? s->fps_den : 1;
    s->codec_ctx->time_base = (AVRational){s->codec_ctx->framerate.den, s->codec_ctx->framerate.num};

    s->codec_ctx->gop_size = s->gop_size > 0 ? s->gop_size : 30;

    if (s->bitrate > 0) {
        s->codec_ctx->bit_rate = s->bitrate;
    } else {
        s->codec_ctx->bit_rate = width * height * 3 * 8 * s->codec_ctx->framerate.num / s->codec_ctx->framerate.den / 20;
    }

    if (s->threads > 0) {
        s->codec_ctx->thread_count = s->threads;
    }

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->frame = av_frame_alloc();
    if (!s->frame) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }
    s->frame->format = s->codec_ctx->pix_fmt;
    s->frame->width = width;
    s->frame->height = height;

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
vp8_encode_frame(vp8_encoder_t* s, const uint8_t* yuv_data, size_t yuv_size,
                 zst_buffer_t** out)
{
    if (!s->initialized) {
        if (vp8_init_encoder(s, s->width, s->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    av_frame_make_writable(s->frame);
    size_t y_size = (size_t)s->width * s->height;
    size_t uv_size = y_size / 4;

    uint8_t* dst_y = s->frame->data[0];
    uint8_t* dst_u = s->frame->data[1];
    uint8_t* dst_v = s->frame->data[2];

    const uint8_t* src = yuv_data;
    for (uint32_t i = 0; i < s->height; i++) {
        memcpy(dst_y + i * s->frame->linesize[0], src + i * s->width, s->width);
    }
    src += y_size;
    for (uint32_t i = 0; i < s->height / 2; i++) {
        memcpy(dst_u + i * s->frame->linesize[1], src + i * (s->width / 2), s->width / 2);
    }
    src += uv_size;
    for (uint32_t i = 0; i < s->height / 2; i++) {
        memcpy(dst_v + i * s->frame->linesize[2], src + i * (s->width / 2), s->width / 2);
    }

    s->frame->pts = s->frame_count++;

    int ret = avcodec_send_frame(s->codec_ctx, s->frame);
    if (ret < 0) {
        return ZST_ERROR;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    ret = avcodec_receive_packet(s->codec_ctx, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ZST_OK;
    }

    zst_buffer_t* obuf = zst_buffer_create(ZST_BUFFER_USER);
    if (!obuf) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }

    obuf->memory.type = ZST_MEMORY_CPU;
    obuf->memory.size = pkt->size;
    obuf->memory.data = malloc(pkt->size);
    if (!obuf->memory.data) {
        zst_buffer_unref(obuf);
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    memcpy(obuf->memory.data, pkt->data, pkt->size);

    obuf->pts = s->frame->pts;
    obuf->dts = pkt->dts;
    obuf->duration = pkt->duration;

    *out = obuf;
    av_packet_free(&pkt);
    return ZST_OK;
}

static zst_result_t
vp8_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    vp8_encoder_t* s = el->priv;

    if (!in || !in->memory.data || in->memory.size == 0) {
        return ZST_OK;
    }

    size_t expected_size = (size_t)s->width * s->height * 3 / 2;

    if (s->width == 0 || s->height == 0) {
        return ZST_OK;
    }

    if (in->memory.size == expected_size) {
        return vp8_encode_frame(s, (const uint8_t*)in->memory.data, in->memory.size, out);
    }

    /* If input is VP8 (passthrough), just forward */
    if (in->memory.size > 0 && in->memory.size != expected_size) {
        *out = zst_buffer_ref(in);
        return ZST_OK;
    }

    return ZST_OK;
}

static zst_result_t
vp8_set_property(zst_element_t* el, const char* name, const char* value)
{
    vp8_encoder_t* s = el->priv;
    if (!value) return ZST_ERROR;

    if (strcmp(name, "bitrate") == 0) {
        s->bitrate = (int64_t)atol(value);
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0) {
        s->gop_size = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        const char* slash = strchr(value, '/');
        if (slash) {
            s->fps_num = atoi(value);
            s->fps_den = atoi(slash + 1);
        } else {
            s->fps_num = atoi(value);
            s->fps_den = 1;
        }
        return ZST_OK;
    } else if (strcmp(name, "threads") == 0) {
        s->threads = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "width") == 0) {
        s->width = (uint32_t)atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        s->height = (uint32_t)atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
vp8_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    vp8_encoder_t* s = el->priv;
    if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%ld", (long)s->bitrate);
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0) {
        snprintf(value_out, max_len, "%d", s->gop_size);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        snprintf(value_out, max_len, "%d/%d", s->fps_num, s->fps_den);
        return ZST_OK;
    } else if (strcmp(name, "threads") == 0) {
        snprintf(value_out, max_len, "%d", s->threads);
        return ZST_OK;
    } else if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->width);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->height);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name        = "vp8enc",
    .open        = vp8_open,
    .close       = vp8_close,
    .process     = vp8_process,
    .set_property = vp8_set_property,
    .get_property = vp8_get_property,
};

zst_element_t*
zst_vp8_encoder_create(void)
{
    zst_element_t* el;
    vp8_encoder_t* priv;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    priv->fps_num = 30;
    priv->fps_den = 1;
    priv->gop_size = 30;

    el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src", ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

zst_element_t* zst_vp8_encoder_create(void);

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "vp8enc") == 0) {
        return zst_vp8_encoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "vp8_encoder_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
        p->refcount = 1;
    }
    return p;
}
#endif /* BUILDING_PLUGIN */
