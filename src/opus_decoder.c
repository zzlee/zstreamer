/*=============================================================================
    opus_decoder.c — FFmpeg libavcodec Opus audio decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

typedef struct {
    uint8_t** plane_pointers;
} opusdec_buf_priv_t;

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    int             channels;
    int             sample_rate;
    int             format;
    int             nb_samples;
    size_t          buffer_size;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    int             threads;
} opus_decoder_t;

static const char*
opusdec_sample_fmt_to_string(int fmt)
{
    switch ((enum AVSampleFormat)fmt) {
    case AV_SAMPLE_FMT_U8:   return "U8";
    case AV_SAMPLE_FMT_S16:  return "S16LE";
    case AV_SAMPLE_FMT_S32:  return "S32LE";
    case AV_SAMPLE_FMT_FLT:  return "F32LE";
    case AV_SAMPLE_FMT_DBL:  return "F64LE";
    case AV_SAMPLE_FMT_U8P:  return "U8P";
    case AV_SAMPLE_FMT_S16P: return "S16P";
    case AV_SAMPLE_FMT_S32P: return "S32P";
    case AV_SAMPLE_FMT_FLTP: return "F32P";
    case AV_SAMPLE_FMT_DBLP: return "F64P";
    default:                 return "";
    }
}

static void
opusdec_buf_free(zst_buffer_t* buf)
{
    if (!buf) return;

    opusdec_buf_priv_t* priv = buf->metadata;
    if (priv) {
        if (priv->plane_pointers) {
            av_free(priv->plane_pointers);
            priv->plane_pointers = NULL;
        }
        free(priv);
        buf->metadata = NULL;
    }

    if (buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static zst_result_t
opusdec_open(zst_element_t* el)
{
    opus_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->channels = 0;
    s->sample_rate = 0;
    s->format = AV_SAMPLE_FMT_NONE;
    s->nb_samples = 0;
    s->buffer_size = 0;
    s->threads = 0; /* auto */
    return ZST_OK;
}

static zst_result_t
opusdec_close(zst_element_t* el)
{
    opus_decoder_t* s = el->priv;
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
opusdec_init_decoder(opus_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    if (s->threads > 0) {
        s->codec_ctx->thread_count = s->threads;
    } else {
        s->codec_ctx->thread_count = 0; /* auto-detect */
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
opusdec_update_pool(opus_decoder_t* s, int channels, int sample_rate, int format, int nb_samples)
{
    int size = av_samples_get_buffer_size(NULL, channels, nb_samples, (enum AVSampleFormat)format, 1);
    if (size < 0) {
        int bps = av_get_bytes_per_sample((enum AVSampleFormat)format);
        if (bps <= 0) bps = 4;
        size = nb_samples * channels * bps;
    }
    if (size <= 0) return ZST_ERROR;

    if (s->pool &&
        s->channels == channels &&
        s->sample_rate == sample_rate &&
        s->format == format &&
        s->nb_samples >= nb_samples &&
        s->buffer_size >= (size_t)size) {
        return ZST_OK;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = (size_t)size,
        .buffer_type = ZST_BUFFER_AUDIO_FRAME
    };

    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->channels = channels;
    s->sample_rate = sample_rate;
    s->format = format;
    s->nb_samples = nb_samples;
    s->buffer_size = (size_t)size;

    return ZST_OK;
}

static zst_result_t
opusdec_emit_buffer(zst_element_t* el, zst_buffer_t* buf, zst_buffer_t** out)
{
    if (!buf) return ZST_ERROR;

    if (el->nb_src_pads > 0 && el->src_pads[0]->peer) {
        zst_result_t ret = zst_pad_push(el->src_pads[0], buf);
        zst_buffer_unref(buf);
        return ret;
    }

    if (out && *out == NULL) {
        *out = buf;
        return ZST_OK;
    }

    zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t
opusdec_emit_frame(zst_element_t* el, opus_decoder_t* s, zst_buffer_t** out)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    int channels = s->frame->ch_layout.nb_channels;
#else
    int channels = s->frame->channels;
#endif
    if (channels <= 0 || s->frame->sample_rate <= 0 || s->frame->nb_samples <= 0) {
        return ZST_ERROR;
    }

    if (opusdec_update_pool(s, channels, s->frame->sample_rate, s->frame->format, s->frame->nb_samples) != ZST_OK) {
        return ZST_ERROR;
    }

    zst_buffer_t* abuf = NULL;
    if (zst_buffer_pool_acquire(s->pool, &abuf, 0, 0) != ZST_OK) {
        return ZST_ERROR;
    }

    zst_audio_frame_t* a_frame = abuf->payload;
    if (!a_frame) {
        a_frame = calloc(1, sizeof(*a_frame));
        if (!a_frame) {
            zst_buffer_unref(abuf);
            return ZST_ERROR;
        }
        abuf->payload = a_frame;
    }

    opusdec_buf_priv_t* buf_priv = abuf->metadata;
    if (!buf_priv) {
        buf_priv = calloc(1, sizeof(*buf_priv));
        if (!buf_priv) {
            zst_buffer_unref(abuf);
            return ZST_ERROR;
        }
        abuf->metadata = buf_priv;
        abuf->destroy = opusdec_buf_free;
    }

    if (buf_priv->plane_pointers) {
        av_free(buf_priv->plane_pointers);
        buf_priv->plane_pointers = NULL;
    }

    int pointer_count = av_sample_fmt_is_planar((enum AVSampleFormat)s->frame->format) ? channels : 1;
    buf_priv->plane_pointers = av_calloc((size_t)pointer_count + 1, sizeof(*buf_priv->plane_pointers));
    if (!buf_priv->plane_pointers) {
        zst_buffer_unref(abuf);
        return ZST_ERROR;
    }

    int linesize = 0;
    int size = av_samples_get_buffer_size(&linesize, channels, s->frame->nb_samples,
                                          (enum AVSampleFormat)s->frame->format, 1);
    if (size < 0) {
        zst_buffer_unref(abuf);
        return ZST_ERROR;
    }

    int fill_ret = av_samples_fill_arrays(buf_priv->plane_pointers, &linesize,
                                          abuf->memory.data, channels,
                                          s->frame->nb_samples,
                                          (enum AVSampleFormat)s->frame->format, 1);
    if (fill_ret < 0) {
        zst_buffer_unref(abuf);
        return ZST_ERROR;
    }

    int copy_ret = av_samples_copy(buf_priv->plane_pointers,
                                   (uint8_t* const*)s->frame->extended_data,
                                   0, 0, s->frame->nb_samples,
                                   channels,
                                   (enum AVSampleFormat)s->frame->format);
    if (copy_ret < 0) {
        zst_buffer_unref(abuf);
        return ZST_ERROR;
    }

    int64_t pts = s->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = s->frame->pts;
    if (pts != AV_NOPTS_VALUE) {
        abuf->pts = (zst_time_t)pts;
        abuf->dts = (zst_time_t)pts;
    }
    abuf->duration = (zst_time_t)((uint64_t)s->frame->nb_samples * 1000000000ULL /
                                  (uint64_t)s->frame->sample_rate);
    abuf->memory.size = (size_t)size;

    a_frame->channels = (uint32_t)channels;
    a_frame->sample_rate = (uint32_t)s->frame->sample_rate;
    a_frame->format = (uint32_t)s->frame->format;
    a_frame->nb_samples = (uint32_t)s->frame->nb_samples;
    if (av_sample_fmt_is_planar((enum AVSampleFormat)s->frame->format)) {
        a_frame->data = buf_priv->plane_pointers;
    } else {
        a_frame->data = buf_priv->plane_pointers[0];
    }

    return opusdec_emit_buffer(el, abuf, out);
}

static zst_result_t
opusdec_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    opus_decoder_t* s = el->priv;
    if (out) *out = NULL;

    if (!in) {
        return ZST_ERROR;
    }

    if (!s->initialized) {
        if (opusdec_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    AVPacket* av_pkt = NULL;
    AVPacket stack_pkt = {0};
    if (!(in->flags & ZST_BUFFER_FLAG_EOS)) {
        if (!in->memory.data || in->memory.size == 0) return ZST_ERROR;
        stack_pkt.data = in->memory.data;
        stack_pkt.size = (int)in->memory.size;
        stack_pkt.pts = in->pts;
        stack_pkt.dts = in->dts;
        stack_pkt.duration = in->duration;
        av_pkt = &stack_pkt;
    }

    int ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    if (ret < 0 && ret != AVERROR_EOF) {
        avcodec_flush_buffers(s->codec_ctx);
        return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
    }

    while (1) {
        ret = avcodec_receive_frame(s->codec_ctx, s->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            avcodec_flush_buffers(s->codec_ctx);
            return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
        }

        zst_result_t emit_ret = opusdec_emit_frame(el, s, out);
        av_frame_unref(s->frame);
        if (emit_ret != ZST_OK) {
            return emit_ret;
        }
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
        return opusdec_emit_buffer(el, eos_buf, out);
    }

    return ZST_OK;
}

static zst_result_t
opusdec_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = opusdec_process(pad->parent, buf, &out);
    if (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
        } else {
            zst_buffer_unref(out);
        }
    }
    return ret;
}

static zst_caps_t*
opusdec_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    opus_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-opus", 0, 0, ""));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw",
                                                           s->channels,
                                                           s->sample_rate,
                                                           opusdec_sample_fmt_to_string(s->format)));
    }

    return caps;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    opus_decoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
opusdec_set_property(zst_element_t* el, const char* name, const char* value)
{
    opus_decoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "threads") == 0) {
        s->threads = atoi(value);
        if (s->threads < 0) s->threads = 0;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
opusdec_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    opus_decoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "threads") == 0) {
        snprintf(value_out, max_len, "%d", s->threads);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name     = "opusdec",
    .open     = opusdec_open,
    .close    = opusdec_close,
    .process  = opusdec_process,
    .get_caps = opusdec_get_caps,
    .set_property = opusdec_set_property,
    .get_property = opusdec_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_opus_decoder_create(void)
{
    zst_element_t* el;
    opus_decoder_t* priv;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    el = zst_element_create(&g_ops, priv);
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
    priv->sinkpad->push = opusdec_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "opusdec") == 0) {
        return zst_opus_decoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_opusdec_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-opus" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};

static const zst_element_desc_t g_opusdec_elements[] = {
    {
        .name = "opusdec",
        .long_name = "Opus Decoder",
        .category = "Codec/Decoder",
        .description = "Decodes Opus audio frames",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_opusdec_pads,
        .nb_pads = sizeof(g_opusdec_pads) / sizeof(g_opusdec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "opusdecoder_plugin",
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
        *nb_elements_out = sizeof(g_opusdec_elements) / sizeof(g_opusdec_elements[0]);
    }
    return g_opusdec_elements;
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
