/*=============================================================================
    opus_encoder.c — FFmpeg libavcodec Opus audio encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    int             sample_rate;
    int             channels;
    int             flushing;
    int             flush_done;
    zst_buffer_pool_t* pool;

    int             bitrate;
} opus_encoder_t;

static zst_result_t
opus_open(zst_element_t* el)
{
    opus_encoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->sample_rate = 48000;
    s->channels = 2;
    s->flushing = 0;
    s->flush_done = 0;
    s->pool = NULL;
    s->bitrate = 128000;
    return ZST_OK;
}

static zst_result_t
opus_close(zst_element_t* el)
{
    opus_encoder_t* s = el->priv;
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
    return ZST_OK;
}

static zst_result_t
opus_init_encoder(opus_encoder_t* s, const zst_audio_frame_t* in_frame)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec || !in_frame) return ZST_ERROR;

    s->sample_rate = (int)in_frame->sample_rate;
    s->channels = (int)in_frame->channels;
    if (s->sample_rate <= 0 || s->channels <= 0 || s->channels > 2) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    enum AVSampleFormat selected_fmt = AV_SAMPLE_FMT_FLTP;
    if (codec->sample_fmts) {
        int found = 0;
        for (const enum AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++) {
            if (*p == AV_SAMPLE_FMT_FLTP) {
                selected_fmt = AV_SAMPLE_FMT_FLTP;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (const enum AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++) {
                if (*p == AV_SAMPLE_FMT_FLT) {
                    selected_fmt = AV_SAMPLE_FMT_FLT;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            for (const enum AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++) {
                if (*p == AV_SAMPLE_FMT_S16) {
                    selected_fmt = AV_SAMPLE_FMT_S16;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            selected_fmt = codec->sample_fmts[0];
        }
    }

    s->codec_ctx->sample_rate = s->sample_rate;
    s->codec_ctx->time_base = (AVRational){1, s->sample_rate};
    s->codec_ctx->sample_fmt = selected_fmt;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_default(&s->codec_ctx->ch_layout, s->channels);
#else
    s->codec_ctx->channels = s->channels;
    s->codec_ctx->channel_layout = s->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
    s->codec_ctx->bit_rate = s->bitrate > 0 ? s->bitrate : 128000;

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
    s->frame->nb_samples = s->codec_ctx->frame_size > 0 ? s->codec_ctx->frame_size : 1024;
    s->frame->format = selected_fmt;
    s->frame->sample_rate = s->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_copy(&s->frame->ch_layout, &s->codec_ctx->ch_layout);
#else
    s->frame->channels = s->channels;
    s->frame->channel_layout = s->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif

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
        .buffer_size = 16384, // Safe upper bound for Opus packet (typical < 8192)
        .buffer_type = ZST_BUFFER_AUDIO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        av_frame_free(&s->frame);
        s->frame = NULL;
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    s->flushing = 0;
    s->flush_done = 0;
    return ZST_OK;
}

static zst_result_t
opus_make_eos(zst_buffer_t** out)
{
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
    if (!eos_buf) return ZST_ERROR;
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
    *out = eos_buf;
    return ZST_OK;
}

static zst_result_t
opus_receive_packet(opus_encoder_t* s, zst_buffer_t** out)
{
    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return ZST_ERROR;

    int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
    if (ret == 0) {
        zst_buffer_t* pkt = NULL;
        if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
            av_packet_free(&av_pkt);
            return ZST_ERROR;
        }

        memcpy(pkt->memory.data, av_pkt->data, av_pkt->size);
        pkt->memory.size = av_pkt->size;
        pkt->pts = av_rescale_q(av_pkt->pts, s->codec_ctx->time_base, (AVRational){1, 1000000000});
        pkt->dts = av_rescale_q(av_pkt->dts, s->codec_ctx->time_base, (AVRational){1, 1000000000});
        pkt->duration = av_rescale_q(av_pkt->duration > 0 ? av_pkt->duration : s->codec_ctx->frame_size,
                                     s->codec_ctx->time_base, (AVRational){1, 1000000000});
        *out = pkt;
        av_packet_free(&av_pkt);
        return ZST_OK;
    }

    av_packet_free(&av_pkt);
    if (ret == AVERROR(EAGAIN)) return ZST_AGAIN;
    if (ret == AVERROR_EOF) return ZST_EOF;
    return ZST_ERROR;
}

static zst_result_t
opus_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    opus_encoder_t* s = el->priv;
    if (!in || !out) return ZST_ERROR;
    *out = NULL;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (!s->initialized || s->flush_done) {
            return opus_make_eos(out);
        }
        if (!s->flushing) {
            int sr = avcodec_send_frame(s->codec_ctx, NULL);
            if (sr < 0 && sr != AVERROR(EAGAIN)) return ZST_ERROR;
            s->flushing = 1;
        }
        zst_result_t r = opus_receive_packet(s, out);
        if (r == ZST_OK) return ZST_OK;
        if (r == ZST_AGAIN) {
            *out = NULL;
            return ZST_OK;
        }
        if (r == ZST_EOF) {
            s->flush_done = 1;
            return opus_make_eos(out);
        }
        return r;
    }

    zst_audio_frame_t* a_frame = in->payload;
    if (!a_frame || !a_frame->data) return ZST_ERROR;

    if (!s->initialized) {
        if (opus_init_encoder(s, a_frame) != ZST_OK) return ZST_ERROR;
    }

    if ((int)a_frame->sample_rate != s->sample_rate || (int)a_frame->channels != s->channels) {
        return ZST_ERROR;
    }

    if (av_frame_make_writable(s->frame) < 0) return ZST_ERROR;
    s->frame->nb_samples = (int)a_frame->nb_samples;

    /* Convert S16 interleaved to target format. */
    int16_t* src = (int16_t*)a_frame->data;
    if (s->codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        float* dst_l = (float*)s->frame->data[0];
        float* dst_r = s->channels > 1 ? (float*)s->frame->data[1] : NULL;
        for (uint32_t i = 0; i < a_frame->nb_samples; i++) {
            dst_l[i] = (float)src[i * s->channels] / 32768.0f;
            if (dst_r) dst_r[i] = (float)src[i * s->channels + 1] / 32768.0f;
        }
    } else if (s->codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
        float* dst = (float*)s->frame->data[0];
        for (uint32_t i = 0; i < a_frame->nb_samples * s->channels; i++) {
            dst[i] = (float)src[i] / 32768.0f;
        }
    } else if (s->codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
        memcpy(s->frame->data[0], a_frame->data, a_frame->nb_samples * s->channels * sizeof(int16_t));
    } else {
        return ZST_ERROR;
    }

    s->frame->pts = av_rescale_q(in->pts, (AVRational){1, 1000000000}, s->codec_ctx->time_base);

    int send_ret = avcodec_send_frame(s->codec_ctx, s->frame);
    if (send_ret == AVERROR(EAGAIN)) {
        return opus_receive_packet(s, out) == ZST_OK ? ZST_OK : ZST_AGAIN;
    }
    if (send_ret < 0) return ZST_ERROR;

    zst_result_t r = opus_receive_packet(s, out);
    if (r == ZST_AGAIN) return ZST_OK;
    return r;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    opus_encoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
opus_set_property(zst_element_t* el, const char* name, const char* value)
{
    opus_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized) return ZST_ERROR;

    if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoi(value);
        if (s->bitrate < 8000) s->bitrate = 8000;
        if (s->bitrate > 512000) s->bitrate = 512000;
        return ZST_OK;
    } else if (strcmp(name, "sample-rate") == 0) {
        s->sample_rate = atoi(value);
        if (s->sample_rate < 8000) s->sample_rate = 8000;
        if (s->sample_rate > 192000) s->sample_rate = 192000;
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        s->channels = atoi(value);
        if (s->channels < 1) s->channels = 1;
        if (s->channels > 2) s->channels = 2;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
opus_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    opus_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%d", s->bitrate);
    } else if (strcmp(name, "sample-rate") == 0) {
        snprintf(value_out, max_len, "%d", s->sample_rate);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->channels);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "opusenc",
    .open    = opus_open,
    .close   = opus_close,
    .process = opus_process,
    .set_property = opus_set_property,
    .get_property = opus_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_opus_encoder_create(void)
{
    zst_element_t* el;
    opus_encoder_t* priv;
    zst_pad_t* sink;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    src  = zst_pad_create("src",  ZST_PAD_SRC);

    zst_element_add_pad(el, sink);
    zst_element_add_pad(el, src);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "opusenc") == 0) {
        return zst_opus_encoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_opusenc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-opus" }
};

static const zst_element_desc_t g_opusenc_elements[] = {
    {
        .name = "opusenc",
        .long_name = "Opus Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw audio to Opus",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_opusenc_pads,
        .nb_pads = sizeof(g_opusenc_pads) / sizeof(g_opusenc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "opusencoder_plugin",
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
        *nb_elements_out = sizeof(g_opusenc_elements) / sizeof(g_opusenc_elements[0]);
    }
    return g_opusenc_elements;
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
