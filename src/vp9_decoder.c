/*=============================================================================
    vp9_decoder.c — FFmpeg libavcodec VP9 video decoder implementation
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
    int             format;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;
    int             threads;
} vp9_decoder_t;

static zst_result_t
vp9_open(zst_element_t* el)
{
    vp9_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->width = 0;
    s->height = 0;
    s->format = AV_PIX_FMT_NONE;
    s->threads = 0;
    return ZST_OK;
}

static zst_result_t
vp9_close(zst_element_t* el)
{
    vp9_decoder_t* s = el->priv;
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
vp9_init_decoder(vp9_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
    if (!codec) {
        fprintf(stderr, "vp9_decoder: VP9 decoder not found (libvpx-vp9 needed)\n");
        return ZST_ERROR;
    }

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    if (s->threads > 0) {
        s->codec_ctx->thread_count = s->threads;
    } else {
        s->codec_ctx->thread_count = 0;
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

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
vp9_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    vp9_decoder_t* s = el->priv;

    if (!in || !in->memory.data || in->memory.size == 0) {
        return ZST_OK;
    }

    if (!s->initialized) {
        if (vp9_init_decoder(s) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    pkt->data = (uint8_t*)in->memory.data;
    pkt->size = (int)in->memory.size;
    pkt->pts = in->pts;
    pkt->dts = in->dts;
    pkt->duration = in->duration;

    int ret = avcodec_send_packet(s->codec_ctx, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ZST_OK;
    }

    ret = avcodec_receive_frame(s->codec_ctx, s->frame);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ZST_OK;
    }

    /* Update dimensions if changed */
    s->width = s->frame->width;
    s->height = s->frame->height;
    s->format = s->frame->format;

    /* Calculate frame size */
    size_t frame_size = 0;
    switch (s->frame->format) {
    case AV_PIX_FMT_YUV420P:
        frame_size = (size_t)s->frame->width * s->frame->height * 3 / 2;
        break;
    default:
        frame_size = (size_t)s->frame->width * s->frame->height * 3;
        break;
    }

    /* Create output buffer */
    zst_buffer_t* obuf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!obuf) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }

    obuf->memory.type = ZST_MEMORY_CPU;
    obuf->memory.size = frame_size;
    obuf->memory.data = malloc(frame_size);
    if (!obuf->memory.data) {
        zst_buffer_unref(obuf);
        av_packet_free(&pkt);
        return ZST_ERROR;
    }

    /* Copy Y plane */
    size_t offset = 0;
    for (int i = 0; i < s->frame->height; i++) {
        memcpy(obuf->memory.data + offset,
               s->frame->data[0] + i * s->frame->linesize[0],
               s->frame->width);
        offset += s->frame->width;
    }

    /* Copy U plane */
    int uv_h = s->frame->height / 2;
    int uv_w = s->frame->width / 2;
    for (int i = 0; i < uv_h; i++) {
        memcpy(obuf->memory.data + offset,
               s->frame->data[1] + i * s->frame->linesize[1],
               uv_w);
        offset += uv_w;
    }

    /* Copy V plane */
    for (int i = 0; i < uv_h; i++) {
        memcpy(obuf->memory.data + offset,
               s->frame->data[2] + i * s->frame->linesize[2],
               uv_w);
        offset += uv_w;
    }

    obuf->pts = s->frame->pts;
    obuf->dts = s->frame->pkt_dts;
    obuf->duration = s->frame->duration;

    *out = obuf;
    av_packet_free(&pkt);
    return ZST_OK;
}

static zst_result_t
vp9_set_property(zst_element_t* el, const char* name, const char* value)
{
    vp9_decoder_t* s = el->priv;
    if (strcmp(name, "threads") == 0) {
        s->threads = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
vp9_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    vp9_decoder_t* s = el->priv;
    if (strcmp(name, "threads") == 0) {
        snprintf(value_out, max_len, "%d", s->threads);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name        = "vp9dec",
    .open        = vp9_open,
    .close       = vp9_close,
    .process     = vp9_process,
    .set_property = vp9_set_property,
    .get_property = vp9_get_property,
};

zst_element_t*
zst_vp9_decoder_create(void)
{
    zst_element_t* el;
    vp9_decoder_t* priv;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

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

zst_element_t* zst_zst_vp9_decoder_create(void);

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "vp9dec") == 0) {
        return zst_zst_vp9_decoder_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "vp9_decoder_plugin",
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
