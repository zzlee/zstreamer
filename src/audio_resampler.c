/*=============================================================================
    audio_resampler.c — Audio sample rate and format conversion element
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libavutil/mathematics.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_audio_resampler.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    int target_sample_rate;
    int target_channels;
    char target_format[32];

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;

    struct SwrContext* swr_ctx;
    int current_in_rate;
    int current_in_channels;
    enum AVSampleFormat current_in_format;

    int current_out_rate;
    int current_out_channels;
    enum AVSampleFormat current_out_format;

    zst_buffer_pool_t* pool;
} audio_resampler_t;

typedef struct {
    uint8_t* sample_buf;
    uint8_t** plane_pointers;
} resampler_buf_priv_t;

static enum AVSampleFormat
sample_format_from_str(const char* name)
{
    if (!name || name[0] == '\0') return AV_SAMPLE_FMT_NONE;
    if (strcmp(name, "S16LE") == 0 || strcmp(name, "S16") == 0) return AV_SAMPLE_FMT_S16;
    if (strcmp(name, "S16P") == 0) return AV_SAMPLE_FMT_S16P;
    if (strcmp(name, "S32LE") == 0 || strcmp(name, "S32") == 0) return AV_SAMPLE_FMT_S32;
    if (strcmp(name, "S32P") == 0) return AV_SAMPLE_FMT_S32P;
    if (strcmp(name, "F32LE") == 0 || strcmp(name, "F32") == 0) return AV_SAMPLE_FMT_FLT;
    if (strcmp(name, "F32P") == 0 || strcmp(name, "FLTP") == 0) return AV_SAMPLE_FMT_FLTP;
    return AV_SAMPLE_FMT_NONE;
}

static enum AVSampleFormat
get_av_sample_format(uint32_t zst_format)
{
    if (zst_format == 0) return AV_SAMPLE_FMT_S16;
    return (enum AVSampleFormat)zst_format;
}

static struct SwrContext*
resampler_alloc_context(
    int out_rate, enum AVSampleFormat out_fmt, int out_channels,
    int in_rate, enum AVSampleFormat in_fmt, int in_channels)
{
    struct SwrContext* swr = NULL;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_default(&in_ch_layout, in_channels);
    av_channel_layout_default(&out_ch_layout, out_channels);
    int ret = swr_alloc_set_opts2(&swr,
                                  &out_ch_layout, out_fmt, out_rate,
                                  &in_ch_layout, in_fmt, in_rate,
                                  0, NULL);
    av_channel_layout_uninit(&in_ch_layout);
    av_channel_layout_uninit(&out_ch_layout);
    if (ret < 0) {
        return NULL;
    }
#else
    int64_t in_ch_layout = av_get_default_channel_layout(in_channels);
    int64_t out_ch_layout = av_get_default_channel_layout(out_channels);
    swr = swr_alloc_set_opts(NULL,
                             out_ch_layout, out_fmt, out_rate,
                             in_ch_layout, in_fmt, in_rate,
                             0, NULL);
#endif
    return swr;
}

static zst_result_t
resampler_open(zst_element_t* el)
{
    audio_resampler_t* s = el->priv;
    s->swr_ctx = NULL;
    s->current_in_rate = 0;
    s->current_in_channels = 0;
    s->current_in_format = AV_SAMPLE_FMT_NONE;
    s->current_out_rate = 0;
    s->current_out_channels = 0;
    s->current_out_format = AV_SAMPLE_FMT_NONE;
    return ZST_OK;
}

static zst_result_t
resampler_close(zst_element_t* el)
{
    audio_resampler_t* s = el->priv;
    if (s->swr_ctx) {
        swr_free(&s->swr_ctx);
        s->swr_ctx = NULL;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_caps_t*
resampler_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    audio_resampler_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        /* Sink pad accepts any raw audio */
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
    } else if (pad == s->srcpad) {
        /* Src pad offers the target configuration, or propagates downstream */
        int rate = s->target_sample_rate;
        int ch = s->target_channels;
        const char* fmt = s->target_format;

        if (rate <= 0 && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_AUDIO) {
                rate = st->audio.sample_rate;
            }
        }
        if (ch <= 0 && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_AUDIO) {
                ch = st->audio.channels;
            }
        }
        if ((!fmt || fmt[0] == '\0') && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_AUDIO) {
                fmt = st->audio.format;
            }
        }

        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", ch, rate, fmt ? fmt : ""));
    }
    return caps;
}

static void
linear_resample_s16_interleaved(
    const int16_t* in_data, int in_rate, int in_channels, int in_samples,
    int16_t* out_data, int out_rate, int out_channels, int out_samples)
{
    double ratio = (double)in_rate / out_rate;
    for (int i = 0; i < out_samples; i++) {
        double pos = i * ratio;
        int idx = (int)pos;
        double frac = pos - idx;

        if (idx >= in_samples - 1) {
            for (int ch = 0; ch < out_channels; ch++) {
                int in_ch = ch < in_channels ? ch : (in_channels - 1);
                out_data[i * out_channels + ch] = in_data[(in_samples - 1) * in_channels + in_ch];
            }
        } else {
            for (int ch = 0; ch < out_channels; ch++) {
                int in_ch = ch < in_channels ? ch : (in_channels - 1);
                int16_t sample1 = in_data[idx * in_channels + in_ch];
                int16_t sample2 = in_data[(idx + 1) * in_channels + in_ch];
                out_data[i * out_channels + ch] = (int16_t)((1.0 - frac) * sample1 + frac * sample2);
            }
        }
    }
}

static void
resampler_passthrough_destroy(zst_buffer_t* b)
{
    if (b && b->payload) {
        free(b->payload);
        b->payload = NULL;
    }
}

static void
resampler_scaled_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        resampler_buf_priv_t* priv = buf->metadata;
        if (priv) {
            if (priv->plane_pointers) {
                av_free(priv->plane_pointers);
            }
            free(priv);
        }
        if (buf->payload) {
            free(buf->payload);
        }
    }
}

static zst_result_t
resampler_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    audio_resampler_t* s = el->priv;
    if (!in) return ZST_ERROR;

    /* EOS Passthrough */
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
        return ZST_ERROR;
    }

    zst_audio_frame_t* in_frame = in->payload;
    if (!in_frame) return ZST_ERROR;

    /* Resolve target output configuration */
    int out_rate = 0;
    int out_channels = 0;
    const char* out_fmt_str = NULL;

    if (s->srcpad->caps && s->srcpad->caps->structs) {
        zst_caps_struct_t* s_caps = s->srcpad->caps->structs;
        if (s_caps->type == ZST_CAPS_AUDIO) {
            out_rate = s_caps->audio.sample_rate;
            out_channels = s_caps->audio.channels;
            out_fmt_str = s_caps->audio.format;
        }
    }

    if (out_rate <= 0) out_rate = s->target_sample_rate;
    if (out_channels <= 0) out_channels = s->target_channels;
    if (!out_fmt_str || out_fmt_str[0] == '\0') out_fmt_str = s->target_format;

    if (out_rate <= 0) out_rate = in_frame->sample_rate;
    if (out_channels <= 0) out_channels = in_frame->channels;

    enum AVSampleFormat in_sample_fmt = get_av_sample_format(in_frame->format);
    enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_NONE;
    if (out_fmt_str && out_fmt_str[0] != '\0') {
        out_sample_fmt = sample_format_from_str(out_fmt_str);
    }
    if (out_sample_fmt == AV_SAMPLE_FMT_NONE) {
        out_sample_fmt = in_sample_fmt;
    }

    /* Passthrough check */
    if (in_frame->sample_rate == (uint32_t)out_rate &&
        in_frame->channels == (uint32_t)out_channels &&
        in_sample_fmt == out_sample_fmt) {

        zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        if (!out_buf) return ZST_ERROR;

        out_buf->pts = in->pts;
        out_buf->dts = in->dts;
        out_buf->duration = in->duration;
        out_buf->flags = in->flags;

        out_buf->memory.type = in->memory.type;
        out_buf->memory.data = in->memory.data;
        out_buf->memory.size = in->memory.size;

        out_buf->memory.priv = zst_buffer_ref(in);
        out_buf->memory.release = (void (*)(void*))zst_buffer_unref;

        zst_audio_frame_t* out_frame = calloc(1, sizeof(*out_frame));
        if (!out_frame) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
        *out_frame = *in_frame;
        out_buf->payload = out_frame;
        out_buf->destroy = resampler_passthrough_destroy;

        *out = out_buf;
        return ZST_OK;
    }

    /* Reallocate swr context if input/output parameters changed */
    if (s->swr_ctx && (s->current_in_rate != (int)in_frame->sample_rate ||
                       s->current_in_channels != (int)in_frame->channels ||
                       s->current_in_format != in_sample_fmt ||
                       s->current_out_rate != out_rate ||
                       s->current_out_channels != out_channels ||
                       s->current_out_format != out_sample_fmt)) {
        swr_free(&s->swr_ctx);
        s->swr_ctx = NULL;
    }

    if (!s->swr_ctx) {
        s->swr_ctx = resampler_alloc_context(
            out_rate, out_sample_fmt, out_channels,
            in_frame->sample_rate, in_sample_fmt, in_frame->channels
        );
        if (s->swr_ctx && swr_init(s->swr_ctx) >= 0) {
            s->current_in_rate = in_frame->sample_rate;
            s->current_in_channels = in_frame->channels;
            s->current_in_format = in_sample_fmt;
            s->current_out_rate = out_rate;
            s->current_out_channels = out_channels;
            s->current_out_format = out_sample_fmt;
        } else if (s->swr_ctx) {
            swr_free(&s->swr_ctx);
            s->swr_ctx = NULL;
        }
    }

    /* Calculate output sample count */
    int out_samples = 0;
    if (s->swr_ctx) {
        int64_t delay = swr_get_delay(s->swr_ctx, in_frame->sample_rate);
        out_samples = av_rescale_rnd(
            delay + in_frame->nb_samples,
            out_rate,
            in_frame->sample_rate,
            AV_ROUND_UP
        );
    } else {
        out_samples = av_rescale_rnd(
            in_frame->nb_samples,
            out_rate,
            in_frame->sample_rate,
            AV_ROUND_UP
        );
    }

    /* Buffer pool logic for output */
    int out_linesize = 0;
    int out_size = av_samples_get_buffer_size(&out_linesize, out_channels, out_samples, out_sample_fmt, 1);
    if (out_size < 0) return ZST_ERROR;

    int needs_new_pool = 1;
    if (s->pool) {
        zst_buffer_pool_config_t cfg = zst_buffer_pool_get_config(s->pool);
        if (cfg.buffer_size == (size_t)out_size) {
            needs_new_pool = 0;
        } else {
            zst_buffer_pool_destroy(s->pool);
            s->pool = NULL;
        }
    }

    if (needs_new_pool) {
        zst_buffer_pool_config_t pool_cfg = {
            .min_buffers = 2,
            .max_buffers = 8,
            .buffer_size = out_size,
            .buffer_type = ZST_BUFFER_AUDIO_FRAME
        };
        s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
        if (!s->pool) return ZST_ERROR;
    }

    zst_buffer_t* out_buf = NULL;
    if (zst_buffer_pool_acquire(s->pool, &out_buf, 0, 0) != ZST_OK) {
        return ZST_ERROR;
    }

    uint8_t* out_data = out_buf->memory.data;

    zst_audio_frame_t* out_frame = out_buf->payload;
    resampler_buf_priv_t* buf_priv = out_buf->metadata;

    if (!out_frame) {
        out_frame = calloc(1, sizeof(*out_frame));
        if (!out_frame) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
        out_buf->payload = out_frame;
    }

    if (!buf_priv) {
        buf_priv = calloc(1, sizeof(*buf_priv));
        if (!buf_priv) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
        out_buf->metadata = buf_priv;
        out_buf->destroy = resampler_scaled_buf_free;
    }

    /* We need an array of pointers for the planes */
    uint8_t** out_data_planes = NULL;
    int alloc_ret = av_samples_alloc_array_and_samples(
        &out_data_planes,
        &out_linesize,
        out_channels,
        out_samples,
        out_sample_fmt,
        0
    );
    if (alloc_ret >= 0) {
        /* We want to use our pooled memory instead of the allocated memory */
        av_freep(&out_data_planes[0]);
        av_samples_fill_arrays(out_data_planes, &out_linesize, out_data, out_channels, out_samples, out_sample_fmt, 1);
    } else {
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    int converted_samples = 0;

    if (s->swr_ctx) {
        const uint8_t** src_data = NULL;
        if (av_sample_fmt_is_planar(in_sample_fmt)) {
            src_data = (const uint8_t**)in_frame->data;
        } else {
            src_data = (const uint8_t*[]){ (const uint8_t*)in_frame->data };
        }

        converted_samples = swr_convert(
            s->swr_ctx,
            out_data_planes,
            out_samples,
            src_data,
            in_frame->nb_samples
        );
        if (converted_samples < 0) {
            av_free(out_data_planes);
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
    } else {
        /* Synthetic Fallback (Linear Interpolation for S16 Interleaved) */
        if (in_sample_fmt == AV_SAMPLE_FMT_S16 && out_sample_fmt == AV_SAMPLE_FMT_S16) {
            linear_resample_s16_interleaved(
                (const int16_t*)in_frame->data, in_frame->sample_rate, in_frame->channels, in_frame->nb_samples,
                (int16_t*)out_data_planes[0], out_rate, out_channels, out_samples
            );
            converted_samples = out_samples;
        } else {
            /* Unsupported formats in fallback: mute */
            memset(out_data_planes[0], 0, out_linesize);
            converted_samples = out_samples;
        }
    }

    out_frame->sample_rate = out_rate;
    out_frame->channels = out_channels;
    out_frame->format = out_sample_fmt;
    out_frame->nb_samples = converted_samples;

    /* Free any previous plane pointers in the pooled buffer if present */
    if (buf_priv->plane_pointers) {
        av_free(buf_priv->plane_pointers);
    }
    buf_priv->sample_buf = NULL; // Memory managed by pool
    buf_priv->plane_pointers = out_data_planes;

    if (av_sample_fmt_is_planar(out_sample_fmt)) {
        out_frame->data = buf_priv->plane_pointers;
    } else {
        out_frame->data = buf_priv->plane_pointers[0];
    }

    out_buf->pts = in->pts;
    out_buf->dts = in->dts;
    out_buf->duration = av_rescale_rnd(converted_samples, 1000000000ULL, out_rate, AV_ROUND_UP);
    out_buf->flags = in->flags;

    *out = out_buf;
    return ZST_OK;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    audio_resampler_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name     = "audioresampler",
    .open     = resampler_open,
    .close    = resampler_close,
    .process  = resampler_process,
    .get_caps = resampler_get_caps,
    .get_pool = element_get_pool
};

zst_element_t*
zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format)
{
    audio_resampler_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->target_sample_rate = target_sample_rate;
    priv->target_channels = target_channels;
    if (target_format) {
        strncpy(priv->target_format, target_format, sizeof(priv->target_format) - 1);
    }

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}



zst_element_t*
zst_audio_resampler_create_with_config(const zst_audio_resampler_config_t* config)
{

    return zst_audio_resampler_create(config ? config->target_sample_rate : 0, config ? config->target_channels : 0, config ? config->target_sample_format : NULL);
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "audioresampler") == 0) {
        return zst_audio_resampler_create(0, 0, NULL);
    }
    return NULL;
}

static const zst_pad_template_t g_audioresampler_pads[] = {
    { "sink", ZST_PAD_SINK, "audio/x-raw" },
    { "src", ZST_PAD_SRC, "audio/x-raw" }
};

static const zst_element_desc_t g_audioresampler_elements[] = {
    {
        .name = "audioresampler",
        .long_name = "Audio Resampler",
        .category = "Filter/Audio",
        .description = "Converts audio sample rate, channels, or format",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_audioresampler_pads,
        .nb_pads = sizeof(g_audioresampler_pads) / sizeof(g_audioresampler_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "audioresampler_plugin",
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
        *nb_elements_out = sizeof(g_audioresampler_elements) / sizeof(g_audioresampler_elements[0]);
    }
    return g_audioresampler_elements;
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
