/*=============================================================================
    audio_resampler.c — Audio sample rate, format, and channel converter
    with optional ASRC (Asynchronous Sample Rate Conversion) drift compensation.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libavutil/mathematics.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_audio_resampler.h"

/* ── ASRC mode constants (mirrors header) ────────────────────────────── */
#define ASRC_MODE_NONE 0
#define ASRC_MODE_PTS  1

/* Defaults */
#define ASRC_DEFAULT_MAX_DRIFT_PPM       1000   /* 0.1 % */
#define ASRC_DEFAULT_CHECK_INTERVAL       4     /* buffers between drift checks */
#define NANOS_PER_SEC                    1000000000LL

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

    /* ── ASRC / drift-compensation state ───────────────────────────── */
    int    asrc_mode;               /* ASRC_MODE_NONE or ASRC_MODE_PTS */
    double max_drift_ppm;           /* sanity limit on drift (parts per million) */
    int    drift_check_interval;    /* check drift every N buffers */

    int64_t last_input_pts;         /* PTS of the last processed input buffer */
    double  cum_drift_output;       /* accumulated drift in output-sample units */
    int     buf_count_since_check;  /* buffers processed since last drift check */

    /* ── Rational rate override (for fractional target rates) ──────── */
    int     rate_numer;             /* target rate numerator (0 = use target_sample_rate) */
    int     rate_denom;             /* target rate denominator (0 = 1) */
    int     comp_sample_delta;      /* pre-computed swr_set_compensation delta */
    int     comp_distance;          /* pre-computed swr_set_compensation distance */

    /* Statistics (read-only) */
    int64_t total_input_samples;
    int64_t total_output_samples;
    int     drift_adjust_count;
} audio_resampler_t;

typedef struct {
    uint8_t* sample_buf;
    uint8_t** plane_pointers;
} resampler_buf_priv_t;

/* ── Format helpers ──────────────────────────────────────────────────── */

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
    switch (zst_format) {
        case 0: return AV_SAMPLE_FMT_S16;   /* ZST_AUDIO_FMT_S16LE */
        case 1: return AV_SAMPLE_FMT_S32;   /* ZST_AUDIO_FMT_S32LE */
        case 3: return AV_SAMPLE_FMT_FLT;   /* ZST_AUDIO_FMT_F32LE */
        case 4: return AV_SAMPLE_FMT_U8;    /* ZST_AUDIO_FMT_U8    */
        case 5: return AV_SAMPLE_FMT_S16P;  /* ZST_AUDIO_FMT_S16P  */
        case 6: return AV_SAMPLE_FMT_S32P;  /* ZST_AUDIO_FMT_S32P  */
        case 7: return AV_SAMPLE_FMT_FLTP;  /* ZST_AUDIO_FMT_F32P  */
        default: return (enum AVSampleFormat)zst_format;
    }
}

static const char*
format_to_name(enum AVSampleFormat fmt)
{
    switch (fmt) {
        case AV_SAMPLE_FMT_S16: return "S16";
        case AV_SAMPLE_FMT_S16P: return "S16P";
        case AV_SAMPLE_FMT_S32: return "S32";
        case AV_SAMPLE_FMT_S32P: return "S32P";
        case AV_SAMPLE_FMT_FLT: return "F32";
        case AV_SAMPLE_FMT_FLTP: return "F32P";
        default: return "UNKNOWN";
    }
}

/* ── libswresample context allocator ─────────────────────────────────── */

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

/* ── Lifecycle ───────────────────────────────────────────────────────── */

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

    /* Reset ASRC state */
    s->last_input_pts = 0;
    s->cum_drift_output = 0.0;
    s->buf_count_since_check = 0;
    s->total_input_samples = 0;
    s->total_output_samples = 0;
    s->drift_adjust_count = 0;

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

/* ── Caps ────────────────────────────────────────────────────────────── */

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

/* ── Linear interpolation fallback (for unsupported swresample scenarios) ── */

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

/* ── Buffer destructors ──────────────────────────────────────────────── */

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

/* ── ASRC drift compensation ───────────────────────────────────────────
 *
 * Called AFTER swr_convert().  Uses buffer PTS to detect sample-clock
 * drift and calls swr_set_compensation() to correct it.
 *
 * Returns the drift in output-sample units (positive = source fast,
 * we over-produced output; negative = source slow, under-produced).
 * Zero means no significant drift.
 * ─────────────────────────────────────────────────────────────────────── */
static double
asrc_detect_drift(audio_resampler_t* s,
                  int64_t pts, int in_samples, int out_samples,
                  int in_rate, int out_rate)
{
    if (s->asrc_mode != ASRC_MODE_PTS || pts <= 0)
        return 0.0;

    /* Need at least one previous PTS to compute delta. */
    if (s->last_input_pts <= 0 || pts <= s->last_input_pts) {
        s->last_input_pts = pts;
        return 0.0;
    }

    int64_t delta_pts = pts - s->last_input_pts;
    s->last_input_pts = pts;

    /* Expected input samples for this PTS span at the nominal rate. */
    double expected_input_samples = (double)delta_pts * (double)in_rate / (double)NANOS_PER_SEC;

    /* Sanity: if the PTS delta is absurdly small or huge, skip. */
    if (expected_input_samples < 1.0 || expected_input_samples > (double)in_samples * 10.0)
        return 0.0;

    /* Drift in input-sample domain.  Positive = source produced more
     * samples than expected (running fast). */
    double drift_input = (double)in_samples - expected_input_samples;

    /* Apply sanity limit based on max-drift-ppm.
     * The maximum plausible drift per buffer at nominal rate. */
    double max_plausible = expected_input_samples * s->max_drift_ppm / 1.0e6;
    if (fabs(drift_input) > max_plausible * 2.0) {
        /* Likely a PTS discontinuity / seek — reset. */
        return 0.0;
    }

    /* Convert to output-sample domain. */
    return drift_input * (double)out_rate / (double)in_rate;
}

static void
asrc_apply_compensation(audio_resampler_t* s, int next_out_samples, int out_rate)
{
    if (s->asrc_mode != ASRC_MODE_PTS || !s->swr_ctx)
        return;

    s->buf_count_since_check++;

    if (s->buf_count_since_check < s->drift_check_interval)
        return;

    s->buf_count_since_check = 0;

    /* If accumulated drift is at least 1 output sample, compensate. */
    if (fabs(s->cum_drift_output) < 1.0)
        return;

    int sample_delta = (int)s->cum_drift_output;

    /* Clamp compensation to a reasonable fraction of the output block
     * to avoid audible glitches from abrupt correction. */
    int max_delta = next_out_samples / 4;
    if (max_delta < 1) max_delta = 1;
    if (sample_delta > max_delta)  sample_delta = max_delta;
    if (sample_delta < -max_delta) sample_delta = -max_delta;

    int ret = swr_set_compensation(s->swr_ctx, sample_delta, next_out_samples);
    if (ret == 0) {
        s->cum_drift_output -= (double)sample_delta;
        s->drift_adjust_count++;

        ZST_LOG_DEBUG("audioresampler", "drift compensation: delta=%d over %d out-samples "
                      "(cum_remaining=%.1f, total_adjusts=%d, out_rate=%d)",
                      sample_delta, next_out_samples,
                      s->cum_drift_output, s->drift_adjust_count, out_rate);
    }
}

/* ── Process ─────────────────────────────────────────────────────────── */

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

    /* ── Resolve target output configuration ────────────────────────── */
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

    /* ── Passthrough check ──────────────────────────────────────────── */
    /* Always go through the resampler when ASRC mode is active
     * (PTS drift detection) or when rational rate override is set,
     * even if nominal integer rates match. */
    if (in_frame->sample_rate == (uint32_t)out_rate &&
        in_frame->channels == (uint32_t)out_channels &&
        in_sample_fmt == out_sample_fmt &&
        s->asrc_mode != ASRC_MODE_PTS &&
        (s->rate_numer <= 0 || s->rate_denom <= 0)) {

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

        /* Update ASRC tracking (passthrough means no conversion needed) */
        if (s->asrc_mode == ASRC_MODE_PTS && in->pts > 0) {
            s->last_input_pts = in->pts;
        }

        *out = out_buf;
        return ZST_OK;
    }

    /* ── Reallocate SwrContext if parameters changed ────────────────── */
    int expected_swr_out_rate = out_rate;
    if (s->rate_numer > 0 && s->rate_denom > 0) {
        double target_rate = (double)s->rate_numer / (double)s->rate_denom;
        expected_swr_out_rate = (int)(target_rate + 0.5);
        if (expected_swr_out_rate == (int)in_frame->sample_rate) {
            expected_swr_out_rate += (target_rate > in_frame->sample_rate) ? 1 : -1;
        }
    }
    if (s->swr_ctx && (s->current_in_rate != (int)in_frame->sample_rate ||
                       s->current_in_channels != (int)in_frame->channels ||
                       s->current_in_format != in_sample_fmt ||
                       s->current_out_rate != expected_swr_out_rate ||
                       s->current_out_channels != out_channels ||
                       s->current_out_format != out_sample_fmt)) {
        swr_free(&s->swr_ctx);
        s->swr_ctx = NULL;
        /* Reset ASRC state on reconfiguration */
        s->last_input_pts = 0;
        s->cum_drift_output = 0.0;
        s->buf_count_since_check = 0;
    }

    if (!s->swr_ctx) {
        /* Determine the swr output rate. When rational rate override
         * is active, the target rate may be fractional. We round to
         * the nearest integer for the swr context (swr only accepts
         * integer rates), then use swr_set_compensation() to fine-tune
         * to the exact fractional target. If the rounded rate equals
         * the input rate, we nudge by 1 to force swr to create a
         * resampling filter (otherwise swr uses a memcpy path that
         * ignores set_compensation). */
        int swr_out_rate = out_rate;

        if (s->rate_numer > 0 && s->rate_denom > 0) {
            double target_rate = (double)s->rate_numer / (double)s->rate_denom;
            swr_out_rate = (int)(target_rate + 0.5);
            /* Force resampling if rounded rate equals input rate */
            if (swr_out_rate == (int)in_frame->sample_rate) {
                swr_out_rate += (target_rate > in_frame->sample_rate) ? 1 : -1;
            }
            /* Re-compute compensation for the chosen swr_out_rate */
            double in_rate_d = (double)in_frame->sample_rate;
            double actual_ratio = (double)swr_out_rate / in_rate_d;
            double target_ratio = target_rate / in_rate_d;
            double comp_frac = target_ratio - actual_ratio;
            /* swr_set_compensation: add sample_delta/distance to ratio.
             * Fix distance at a reasonable large value for smooth adj. */
            int dist = 4800000;
            int delta = (int)(comp_frac * (double)dist + 0.5);
            if (delta != 0 && dist > 0) {
                s->comp_sample_delta = delta;
                s->comp_distance = dist;
            } else {
                s->comp_sample_delta = 0;
                s->comp_distance = 0;
            }
        }

        s->swr_ctx = resampler_alloc_context(
            swr_out_rate, out_sample_fmt, out_channels,
            in_frame->sample_rate, in_sample_fmt, in_frame->channels
        );
        if (s->swr_ctx && swr_init(s->swr_ctx) >= 0) {
            s->current_in_rate = (int)in_frame->sample_rate;
            s->current_in_channels = (int)in_frame->channels;
            s->current_in_format = in_sample_fmt;
            s->current_out_rate = swr_out_rate;
            s->current_out_channels = out_channels;
            s->current_out_format = out_sample_fmt;
        } else if (s->swr_ctx) {
            swr_free(&s->swr_ctx);
            s->swr_ctx = NULL;
        }
    }

    /* ── Calculate output sample count ──────────────────────────────── */
    /* Determine the actual resampling ratio. When rational rate override
     * is active, the swr context has ratio = numer / (in_rate * denom),
     * so we must use the scaled rates for the output size calculation. */
    int rescale_out_rate = out_rate;
    int rescale_in_rate  = (int)in_frame->sample_rate;
    if (s->rate_numer > 0 && s->rate_denom > 0) {
        rescale_out_rate = s->rate_numer;
        rescale_in_rate  = (int)in_frame->sample_rate * s->rate_denom;
    }

    int out_samples = 0;
    if (s->swr_ctx) {
        int64_t delay = swr_get_delay(s->swr_ctx, in_frame->sample_rate);
        out_samples = av_rescale_rnd(
            delay + in_frame->nb_samples,
            rescale_out_rate,
            rescale_in_rate,
            AV_ROUND_UP
        );
    } else {
        out_samples = av_rescale_rnd(
            in_frame->nb_samples,
            rescale_out_rate,
            rescale_in_rate,
            AV_ROUND_UP
        );
    }

    /* ── Buffer pool ────────────────────────────────────────────────── */
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
        /* Use our pooled memory instead of the freshly allocated one */
        av_freep(&out_data_planes[0]);
        av_samples_fill_arrays(out_data_planes, &out_linesize, out_data,
                               out_channels, out_samples, out_sample_fmt, 1);
    } else {
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }

    /* ── Perform resampling ─────────────────────────────────────────── */
    int converted_samples = 0;

    if (s->swr_ctx) {
        /* Apply fixed rate-override compensation if set.
         * We reapply before every swr_convert so that the compensation
         * never expires — a new call restarts the distance counter. */
        if (s->comp_distance > 0 && s->comp_sample_delta != 0) {
            swr_set_compensation(s->swr_ctx, s->comp_sample_delta, s->comp_distance);
        }

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

        /* ── ASRC drift detection & compensation ────────────────────── */
        if (s->asrc_mode == ASRC_MODE_PTS && in->pts > 0) {
            double drift = asrc_detect_drift(s, in->pts,
                                             (int)in_frame->nb_samples,
                                             converted_samples,
                                             (int)in_frame->sample_rate,
                                             out_rate);
            s->cum_drift_output += drift;
            s->total_input_samples += in_frame->nb_samples;
            s->total_output_samples += converted_samples;

            asrc_apply_compensation(s, converted_samples, out_rate);
        }
    } else {
        /* Synthetic Fallback (Linear Interpolation for S16 Interleaved) */
        if (in_sample_fmt == AV_SAMPLE_FMT_S16 && out_sample_fmt == AV_SAMPLE_FMT_S16) {
            linear_resample_s16_interleaved(
                (const int16_t*)in_frame->data, in_frame->sample_rate,
                in_frame->channels, in_frame->nb_samples,
                (int16_t*)out_data_planes[0], out_rate,
                out_channels, out_samples
            );
            converted_samples = out_samples;
        } else {
            /* Unsupported formats in fallback: mute */
            memset(out_data_planes[0], 0, out_linesize);
            converted_samples = out_samples;
        }
    }

    /* ── Set output frame metadata ──────────────────────────────────── */
    /* Use the actual swr output rate (may differ from caps due to
     * rational rate override). */
    out_frame->sample_rate = s->current_out_rate > 0 ? s->current_out_rate : out_rate;
    out_frame->channels = out_channels;
    out_frame->format = out_sample_fmt;
    out_frame->nb_samples = converted_samples;

    /* Free any previous plane pointers in the pooled buffer */
    if (buf_priv->plane_pointers) {
        av_free(buf_priv->plane_pointers);
    }
    buf_priv->sample_buf = NULL;  /* Memory managed by pool */
    buf_priv->plane_pointers = out_data_planes;

    if (av_sample_fmt_is_planar(out_sample_fmt)) {
        out_frame->data = buf_priv->plane_pointers;
    } else {
        out_frame->data = buf_priv->plane_pointers[0];
    }

    out_buf->pts = in->pts;
    out_buf->dts = in->dts;
    int duration_rate = s->current_out_rate > 0 ? s->current_out_rate : out_rate;
    out_buf->duration = av_rescale_rnd(converted_samples, NANOS_PER_SEC, duration_rate, AV_ROUND_UP);
    out_buf->flags = in->flags;

    *out = out_buf;
    return ZST_OK;
}

/* ── Pool accessor ───────────────────────────────────────────────────── */

static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    audio_resampler_t* s = el->priv;
    return s->pool;
}

/* ── Properties ──────────────────────────────────────────────────────── */

static int
asrc_mode_from_str(const char* value)
{
    if (strcmp(value, "pts") == 0)     return ASRC_MODE_PTS;
    return ASRC_MODE_NONE;
}

static const char*
asrc_mode_to_str(int mode)
{
    switch (mode) {
        case ASRC_MODE_PTS:  return "pts";
        default:             return "none";
    }
}

static zst_result_t
resampler_set_property(zst_element_t* el, const char* name, const char* value)
{
    audio_resampler_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "sample-rate") == 0) {
        s->target_sample_rate = atoi(value);
        if (s->target_sample_rate < 0) s->target_sample_rate = 0;
        /* Clear rational override when sample-rate is set directly */
        s->rate_numer = 0;
        s->rate_denom = 0;
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        s->target_channels = atoi(value);
        if (s->target_channels < 0) s->target_channels = 0;
        return ZST_OK;
    } else if (strcmp(name, "format") == 0) {
        snprintf(s->target_format, sizeof(s->target_format), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "asrc-mode") == 0) {
        int mode = asrc_mode_from_str(value);
        s->asrc_mode = mode;
        /* Reset tracking state on mode change */
        s->last_input_pts = 0;
        s->cum_drift_output = 0.0;
        s->buf_count_since_check = 0;
        ZST_LOG_INFO("[audioresampler] asrc-mode set to '%s'", asrc_mode_to_str(mode));
        return ZST_OK;
    } else if (strcmp(name, "max-drift-ppm") == 0) {
        double v = strtod(value, NULL);
        if (v < 1.0) v = 1.0;
        if (v > 100000.0) v = 100000.0;  /* 10% sanity cap */
        s->max_drift_ppm = v;
        return ZST_OK;
    } else if (strcmp(name, "drift-check-interval") == 0) {
        int v = atoi(value);
        if (v < 1) v = 1;
        s->drift_check_interval = v;
        return ZST_OK;
    } else if (strcmp(name, "rate-numer") == 0) {
        s->rate_numer = atoi(value);
        if (s->rate_numer < 0) s->rate_numer = 0;
        /* Force swr recreation so compensation is recomputed */
        s->current_in_rate = -1;
        return ZST_OK;
    } else if (strcmp(name, "rate-denom") == 0) {
        s->rate_denom = atoi(value);
        if (s->rate_denom < 0) s->rate_denom = 0;
        /* Force swr recreation so compensation is recomputed */
        s->current_in_rate = -1;
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
resampler_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    audio_resampler_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "sample-rate") == 0) {
        snprintf(value_out, max_len, "%d", s->target_sample_rate);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->target_channels);
    } else if (strcmp(name, "format") == 0) {
        snprintf(value_out, max_len, "%s", s->target_format);
    } else if (strcmp(name, "asrc-mode") == 0) {
        snprintf(value_out, max_len, "%s", asrc_mode_to_str(s->asrc_mode));
    } else if (strcmp(name, "max-drift-ppm") == 0) {
        snprintf(value_out, max_len, "%.0f", s->max_drift_ppm);
    } else if (strcmp(name, "drift-check-interval") == 0) {
        snprintf(value_out, max_len, "%d", s->drift_check_interval);
    } else if (strcmp(name, "rate-numer") == 0) {
        snprintf(value_out, max_len, "%d", s->rate_numer);
    } else if (strcmp(name, "rate-denom") == 0) {
        snprintf(value_out, max_len, "%d", s->rate_denom);
    } else if (strcmp(name, "total-input-samples") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->total_input_samples);
    } else if (strcmp(name, "total-output-samples") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->total_output_samples);
    } else if (strcmp(name, "drift-adjust-count") == 0) {
        snprintf(value_out, max_len, "%d", s->drift_adjust_count);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

/* ── Ops vtable ──────────────────────────────────────────────────────── */

static zst_element_ops_t g_ops = {
    .name     = "audioresampler",
    .open     = resampler_open,
    .close    = resampler_close,
    .process  = resampler_process,
    .get_caps = resampler_get_caps,
    .set_property = resampler_set_property,
    .get_property = resampler_get_property,
    .get_pool = element_get_pool
};

/* ── Public constructor ──────────────────────────────────────────────── */

zst_element_t*
zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format)
{
    return zst_audio_resampler_create_with_config(
        target_sample_rate, target_channels, target_format, ZST_ASRC_MODE_NONE, ASRC_DEFAULT_MAX_DRIFT_PPM);
}

zst_element_t*
zst_audio_resampler_create_with_config(int target_sample_rate, int target_channels,
                                        const char* target_format,
                                        const char* asrc_mode, double max_drift_ppm)
{
    audio_resampler_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->target_sample_rate = target_sample_rate;
    priv->target_channels = target_channels;
    if (target_format) {
        strncpy(priv->target_format, target_format, sizeof(priv->target_format) - 1);
    }

    /* ASRC defaults */
    priv->asrc_mode = asrc_mode ? asrc_mode_from_str(asrc_mode) : ASRC_MODE_NONE;
    priv->max_drift_ppm = (max_drift_ppm > 0.0) ? max_drift_ppm : ASRC_DEFAULT_MAX_DRIFT_PPM;
    priv->drift_check_interval = ASRC_DEFAULT_CHECK_INTERVAL;
    priv->last_input_pts = 0;
    priv->cum_drift_output = 0.0;
    priv->buf_count_since_check = 0;
    priv->total_input_samples = 0;
    priv->total_output_samples = 0;
    priv->drift_adjust_count = 0;
    priv->rate_numer = 0;
    priv->rate_denom = 0;
    priv->comp_sample_delta = 0;
    priv->comp_distance = 0;

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
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};

static const zst_element_desc_t g_audioresampler_elements[] = {
    {
        .name = "audioresampler",
        .long_name = "Audio Resampler",
        .category = "Filter/Audio",
        .description = "Converts audio sample rate, channels, or format; supports optional ASRC drift compensation",
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
