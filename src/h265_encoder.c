/*=============================================================================
    h265_encoder.c — FFmpeg libavcodec H.265 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_h265_encoder.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

#ifndef AV_INPUT_BUFFER_PADDING_SIZE
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#endif

typedef struct h265_pending_packet {
    zst_buffer_t* buf;
    struct h265_pending_packet* next;
} h265_pending_packet_t;

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    int             draining;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            preset[32];
    char            tune[32];
    char            profile[32];
    char            level[16];
    double          crf;
    int64_t         bitrate;
    int             gop_size;
    int             keyint_min;

    h265_pending_packet_t* pending_head;
    h265_pending_packet_t* pending_tail;
} h265_encoder_t;

static void
h265_pending_clear(h265_encoder_t* s)
{
    h265_pending_packet_t* p = s->pending_head;
    while (p) {
        h265_pending_packet_t* next = p->next;
        zst_buffer_unref(p->buf);
        free(p);
        p = next;
    }
    s->pending_head = NULL;
    s->pending_tail = NULL;
}

static zst_result_t
h265_pending_push(h265_encoder_t* s, zst_buffer_t* buf)
{
    h265_pending_packet_t* node = calloc(1, sizeof(*node));
    if (!node) return ZST_ERROR;
    node->buf = buf;
    if (s->pending_tail) {
        s->pending_tail->next = node;
    } else {
        s->pending_head = node;
    }
    s->pending_tail = node;
    return ZST_OK;
}

static zst_buffer_t*
h265_pending_pop(h265_encoder_t* s)
{
    h265_pending_packet_t* node = s->pending_head;
    if (!node) return NULL;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    zst_buffer_t* buf = node->buf;
    free(node);
    return buf;
}

static int
h265_is_config_property(const char* name)
{
    return strcmp(name, "preset") == 0 ||
           strcmp(name, "tune") == 0 ||
           strcmp(name, "crf") == 0 ||
           strcmp(name, "bitrate") == 0 ||
           strcmp(name, "gop-size") == 0 ||
           strcmp(name, "gop") == 0 ||
           strcmp(name, "keyint") == 0 ||
           strcmp(name, "keyframe-interval") == 0 ||
           strcmp(name, "keyint-min") == 0 ||
           strcmp(name, "profile") == 0 ||
           strcmp(name, "level") == 0;
}

static zst_result_t
h265_open(zst_element_t* el)
{
    h265_encoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->draining = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    h265_pending_clear(s);
    return ZST_OK;
}

static zst_result_t
h265_close(zst_element_t* el)
{
    h265_encoder_t* s = el->priv;
    h265_pending_clear(s);
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->frame) {
        av_frame_free(&s->frame);
        s->frame = NULL;
    }
    s->initialized = 0;
    s->draining = 0;
    return ZST_OK;
}

static void
h265_apply_private_options(h265_encoder_t* s)
{
    if (!s->codec_ctx || !s->codec_ctx->priv_data) return;

    if (s->preset[0]) av_opt_set(s->codec_ctx->priv_data, "preset", s->preset, 0);
    if (s->tune[0]) av_opt_set(s->codec_ctx->priv_data, "tune", s->tune, 0);
    if (s->profile[0]) av_opt_set(s->codec_ctx->priv_data, "profile", s->profile, 0);

    if (s->bitrate <= 0 && s->crf >= 0.0) {
        char crf_buf[32];
        snprintf(crf_buf, sizeof(crf_buf), "%.3f", s->crf);
        av_opt_set(s->codec_ctx->priv_data, "crf", crf_buf, 0);
    }

    char params[256];
    int n = snprintf(params, sizeof(params), "repeat-headers=1:keyint=%d:min-keyint=%d",
                     s->gop_size > 0 ? s->gop_size : 30,
                     s->keyint_min > 0 ? s->keyint_min : 1);
    if (s->level[0] && n > 0 && (size_t)n < sizeof(params)) {
        snprintf(params + n, sizeof(params) - (size_t)n, ":level=%s", s->level);
    }
    av_opt_set(s->codec_ctx->priv_data, "x265-params", params, 0);
}

static zst_result_t
h265_init_encoder(h265_encoder_t* s, uint32_t width, uint32_t height)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->codec_ctx->width = width;
    s->codec_ctx->height = height;
    s->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    s->codec_ctx->time_base = (AVRational){1, 30};
    s->codec_ctx->framerate = (AVRational){30, 1};
    s->codec_ctx->bit_rate = s->bitrate > 0 ? s->bitrate : 0;
    s->codec_ctx->max_b_frames = 0;
    s->codec_ctx->gop_size = s->gop_size > 0 ? s->gop_size : 30;
    s->codec_ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER; /* keep VPS/SPS/PPS in Annex-B packets */

    h265_apply_private_options(s);

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
    s->frame->width = s->codec_ctx->width;
    s->frame->height = s->codec_ctx->height;
    s->frame->pts = 0;

    if (av_frame_get_buffer(s->frame, 0) < 0) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = (size_t)width * height * 3 / 2 + AV_INPUT_BUFFER_PADDING_SIZE,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->width = width;
    s->height = height;
    s->initialized = 1;
    s->draining = 0;
    return ZST_OK;
}

static zst_result_t
h265_queue_av_packet(h265_encoder_t* s, const AVPacket* av_pkt)
{
    zst_buffer_t* pkt = NULL;
    if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
        return ZST_ERROR;
    }

    if ((size_t)av_pkt->size > pkt->memory.size) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }

    memcpy(pkt->memory.data, av_pkt->data, av_pkt->size);
    pkt->memory.size = (size_t)av_pkt->size;
    if (av_pkt->pts != AV_NOPTS_VALUE) pkt->pts = (zst_time_t)av_pkt->pts;
    if (av_pkt->dts != AV_NOPTS_VALUE) pkt->dts = (zst_time_t)av_pkt->dts;
    if (av_pkt->duration > 0) pkt->duration = (zst_time_t)av_pkt->duration;

    if (h265_pending_push(s, pkt) != ZST_OK) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
h265_receive_packets(h265_encoder_t* s)
{
    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return ZST_ERROR;

    while (1) {
        int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&av_pkt);
            return ZST_OK;
        }
        if (ret < 0) {
            av_packet_free(&av_pkt);
            return ZST_ERROR;
        }

        zst_result_t qret = h265_queue_av_packet(s, av_pkt);
        av_packet_unref(av_pkt);
        if (qret != ZST_OK) {
            av_packet_free(&av_pkt);
            return qret;
        }
    }
}

static zst_result_t
h265_queue_eos(h265_encoder_t* s)
{
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!eos_buf) return ZST_ERROR;
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
    if (h265_pending_push(s, eos_buf) != ZST_OK) {
        zst_buffer_unref(eos_buf);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
h265_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h265_encoder_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;

    zst_buffer_t* pending = h265_pending_pop(s);
    if (pending) {
        *out = pending;
        return ZST_OK;
    }

    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->initialized && !s->draining) {
            s->draining = 1;
            int ret = avcodec_send_frame(s->codec_ctx, NULL);
            if (ret < 0 && ret != AVERROR_EOF) return ZST_ERROR;
            if (h265_receive_packets(s) != ZST_OK) return ZST_ERROR;
        }
        if (h265_queue_eos(s) != ZST_OK) return ZST_ERROR;
        *out = h265_pending_pop(s);
        return ZST_OK;
    }

    zst_video_frame_t* frame = in->payload;
    if (!frame || !frame->plane[0] || !frame->plane[1] || !frame->plane[2]) return ZST_ERROR;

    if (!s->initialized) {
        if (h265_init_encoder(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        return ZST_ERROR;
    }

    if (av_frame_make_writable(s->frame) < 0) {
        return ZST_ERROR;
    }
    s->frame->pts = (int64_t)in->pts;

    int y_linesize = s->frame->linesize[0];
    int uv_linesize = s->frame->linesize[1];
    int width = (int)s->width;
    int height = (int)s->height;
    int half_width = width / 2;
    int half_height = height / 2;

    for (int row = 0; row < height; row++) {
        memcpy(s->frame->data[0] + (size_t)row * y_linesize,
               frame->plane[0] + (size_t)row * frame->stride[0],
               (size_t)width);
    }

    for (int row = 0; row < half_height; row++) {
        memcpy(s->frame->data[1] + (size_t)row * uv_linesize,
               frame->plane[1] + (size_t)row * frame->stride[1],
               (size_t)half_width);
        memcpy(s->frame->data[2] + (size_t)row * uv_linesize,
               frame->plane[2] + (size_t)row * frame->stride[2],
               (size_t)half_width);
    }

    if (avcodec_send_frame(s->codec_ctx, s->frame) < 0) {
        return ZST_ERROR;
    }

    if (h265_receive_packets(s) != ZST_OK) {
        return ZST_ERROR;
    }

    *out = h265_pending_pop(s);
    return ZST_OK;
}

static zst_caps_t*
h265_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    h265_encoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           30.0,
                                                           s->profile[0] ? s->profile : "main"));
    }
    return caps;
}

static zst_result_t
h265_set_property(zst_element_t* el, const char* name, const char* value)
{
    h265_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized && h265_is_config_property(name)) return ZST_ERROR;

    if (strcmp(name, "preset") == 0) {
        snprintf(s->preset, sizeof(s->preset), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "tune") == 0) {
        snprintf(s->tune, sizeof(s->tune), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "crf") == 0) {
        s->crf = atof(value);
        if (s->crf < 0.0) s->crf = 0.0;
        if (s->crf > 51.0) s->crf = 51.0;
        return ZST_OK;
    } else if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
        if (s->bitrate < 0) s->bitrate = 0;
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        s->gop_size = atoi(value);
        if (s->gop_size < 1) s->gop_size = 1;
        return ZST_OK;
    } else if (strcmp(name, "keyint-min") == 0) {
        s->keyint_min = atoi(value);
        if (s->keyint_min < 1) s->keyint_min = 1;
        return ZST_OK;
    } else if (strcmp(name, "profile") == 0) {
        snprintf(s->profile, sizeof(s->profile), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "level") == 0) {
        snprintf(s->level, sizeof(s->level), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
h265_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    h265_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "preset") == 0) {
        snprintf(value_out, max_len, "%s", s->preset);
    } else if (strcmp(name, "tune") == 0) {
        snprintf(value_out, max_len, "%s", s->tune);
    } else if (strcmp(name, "crf") == 0) {
        snprintf(value_out, max_len, "%.3f", s->crf);
    } else if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%" PRId64, s->bitrate);
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        snprintf(value_out, max_len, "%d", s->gop_size);
    } else if (strcmp(name, "keyint-min") == 0) {
        snprintf(value_out, max_len, "%d", s->keyint_min);
    } else if (strcmp(name, "profile") == 0) {
        snprintf(value_out, max_len, "%s", s->profile);
    } else if (strcmp(name, "level") == 0) {
        snprintf(value_out, max_len, "%s", s->level);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
h265_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    h265_encoder_t* s = pad->parent->priv;
    zst_buffer_t* out = NULL;
    zst_result_t ret = h265_process(pad->parent, buf, &out);

    while (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
            if (push_ret != ZST_OK) return ret;
        } else {
            zst_buffer_unref(out);
        }
        out = h265_pending_pop(s);
    }

    return ret;
}

static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    h265_encoder_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name    = "h265enc",
    .open    = h265_open,
    .close   = h265_close,
    .process = h265_process,
    .get_caps = h265_get_caps,
    .set_property = h265_set_property,
    .get_property = h265_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_h265_encoder_create(void)
{
    h265_encoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->preset, sizeof(priv->preset), "%s", "ultrafast");
    snprintf(priv->tune, sizeof(priv->tune), "%s", "zerolatency");
    snprintf(priv->profile, sizeof(priv->profile), "%s", "main");
    priv->level[0] = '\0';
    priv->crf = 23.0;
    priv->bitrate = 0;
    priv->gop_size = 30;
    priv->keyint_min = 1;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sinkpad->push = h265_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}



zst_element_t*
zst_h265_encoder_create_with_config(const zst_h265_encoder_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_h265_encoder_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("h265enc");
    if (!el) return NULL;

    if (config->preset) {
        zst_element_set_property_string(el, "preset", config->preset);
    }
    if (config->tune) {
        zst_element_set_property_string(el, "tune", config->tune);
    }
    zst_element_set_property_int(el, "crf", config->crf);
    if (config->bitrate > 0) {
        zst_element_set_property_int(el, "bitrate", config->bitrate);
    }
    if (config->gop_size > 0) {
        zst_element_set_property_int(el, "gop-size", config->gop_size);
    }
    zst_element_set_property_int(el, "keyint-min", config->keyint_min);
    if (config->profile) {
        zst_element_set_property_string(el, "profile", config->profile);
    }
    if (config->level) {
        zst_element_set_property_string(el, "level", config->level);
    }

    return el;
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "h265enc") == 0) {
        return zst_h265_encoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_h265enc_properties[] = {
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "ultrafast", "FFmpeg/libx265 preset" },
    { "tune", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zerolatency", "FFmpeg/libx265 tune" },
    { "crf", ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "23.0", "Constant Rate Factor (0-51); used when bitrate is 0" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Target bitrate in bits/sec; 0 enables CRF mode" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "keyint-min", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Minimum keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "HEVC profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "HEVC level (empty = encoder default)" }
};

static const zst_pad_template_t g_h265enc_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-raw" },
    { "src", ZST_PAD_SRC, "video/x-h265" }
};

static const zst_element_desc_t g_h265enc_elements[] = {
    {
        .name = "h265enc",
        .long_name = "H.265 Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw video to H.265/HEVC Annex B byte-stream packets",
        .author = "zstreamer",
        .properties = g_h265enc_properties,
        .nb_properties = sizeof(g_h265enc_properties) / sizeof(g_h265enc_properties[0]),
        .pads = g_h265enc_pads,
        .nb_pads = sizeof(g_h265enc_pads) / sizeof(g_h265enc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h265encoder_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_h265enc_elements) / sizeof(g_h265enc_elements[0]);
    }
    return g_h265enc_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
