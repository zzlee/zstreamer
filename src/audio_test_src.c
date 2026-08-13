/*=============================================================================
    audio_test_src.c — Synthetic audio test signal source
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

/* zstreamer internal audio format codes.
 * Code 0 is special-cased in the resampler to AV_SAMPLE_FMT_S16.
 * Other codes should ideally match FFmpeg AVSampleFormat values
 * so the resampler can cast directly. */
#define ZST_AUDIO_FMT_S16LE  0u   /* interleaved S16LE (default) */
#define ZST_AUDIO_FMT_S32LE  1u   /* interleaved S32LE */
#define ZST_AUDIO_FMT_F32LE  3u   /* interleaved F32LE */
#define ZST_AUDIO_FMT_U8     4u   /* interleaved unsigned 8-bit */
#define ZST_AUDIO_FMT_S16P   5u   /* planar S16 */
#define ZST_AUDIO_FMT_S32P   6u   /* planar S32 */
#define ZST_AUDIO_FMT_F32P   7u   /* planar F32 */

typedef enum {
    WAVE_SINE,
    WAVE_SQUARE,
    WAVE_WHITE_NOISE,
    WAVE_PINK_NOISE,
    WAVE_SILENCE,
    WAVE_STEREO_TONE
} audio_wave_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t samples_per_buffer;
    char sample_format[32];
    audio_wave_t wave;
    double frequency;
    double volume;
    int64_t num_samples;
    int64_t num_buffers;
    bool loop;
    bool use_clock;
    bool real_time_pacing;

    bool has_base_time;
    zst_time_t base_time;

    uint64_t sample_count;
    uint64_t buffer_count;
    bool stopped;
    double phase;
    uint32_t rng_state;

    /* Pink-noise filter state (Paul Kellet style approximation). */
    double pink_b0;
    double pink_b1;
    double pink_b2;
    double pink_b3;
    double pink_b4;
    double pink_b5;
    double pink_b6;

    zst_buffer_pool_t* pool;
} audio_test_src_t;

static double
zst_abs_double(double x)
{
    return x < 0.0 ? -x : x;
}

static double
fast_sine_from_phase(double phase)
{
    /* phase is one cycle in [0, 1). Use a compact approximation to avoid libm. */
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double x = phase * two_pi;
    if (x > pi) x -= two_pi;

    const double b = 4.0 / pi;
    const double c = -4.0 / (pi * pi);
    const double p = 0.225;

    double y = b * x + c * x * zst_abs_double(x);
    y = p * (y * zst_abs_double(y) - y) + y;
    return y;
}

static uint32_t
lcg_next(audio_test_src_t* s)
{
    s->rng_state = s->rng_state * 1664525u + 1013904223u;
    return s->rng_state;
}

static double
white_noise_sample(audio_test_src_t* s)
{
    uint32_t v = lcg_next(s);
    return ((double)v / 2147483648.0) - 1.0;
}

static double
pink_noise_sample(audio_test_src_t* s)
{
    double white = white_noise_sample(s);

    s->pink_b0 = 0.99886 * s->pink_b0 + white * 0.0555179;
    s->pink_b1 = 0.99332 * s->pink_b1 + white * 0.0750759;
    s->pink_b2 = 0.96900 * s->pink_b2 + white * 0.1538520;
    s->pink_b3 = 0.86650 * s->pink_b3 + white * 0.3104856;
    s->pink_b4 = 0.55000 * s->pink_b4 + white * 0.5329522;
    s->pink_b5 = -0.7616 * s->pink_b5 - white * 0.0168980;

    double pink = s->pink_b0 + s->pink_b1 + s->pink_b2 + s->pink_b3 +
                  s->pink_b4 + s->pink_b5 + s->pink_b6 + white * 0.5362;
    s->pink_b6 = white * 0.115926;

    /* The filter has gain > 1. Scale and clamp to a stable [-1, 1] range. */
    pink *= 0.11;
    if (pink > 1.0) pink = 1.0;
    if (pink < -1.0) pink = -1.0;
    return pink;
}

static bool
is_planar_format(const char* fmt)
{
    return (strcmp(fmt, "S16P") == 0 || strcmp(fmt, "S32P") == 0 ||
            strcmp(fmt, "F32P") == 0 || strcmp(fmt, "FLTP") == 0);
}

static uint32_t
format_code_from_string(const char* format)
{
    if (!format) return ZST_AUDIO_FMT_S16LE;
    if (strcmp(format, "S16LE") == 0 || strcmp(format, "S16") == 0) return ZST_AUDIO_FMT_S16LE;
    if (strcmp(format, "S32LE") == 0 || strcmp(format, "S32") == 0) return ZST_AUDIO_FMT_S32LE;
    if (strcmp(format, "F32LE") == 0 || strcmp(format, "F32") == 0) return ZST_AUDIO_FMT_F32LE;
    if (strcmp(format, "U8") == 0)   return ZST_AUDIO_FMT_U8;
    if (strcmp(format, "S16P") == 0) return ZST_AUDIO_FMT_S16P;
    if (strcmp(format, "S32P") == 0) return ZST_AUDIO_FMT_S32P;
    if (strcmp(format, "F32P") == 0 || strcmp(format, "FLTP") == 0) return ZST_AUDIO_FMT_F32P;
    return ZST_AUDIO_FMT_S16LE;
}

static size_t
bytes_per_sample_for_format(const char* format)
{
    if (!format) return sizeof(int16_t);
    if (strcmp(format, "U8") == 0)   return 1;
    if (strcmp(format, "S16LE") == 0 || strcmp(format, "S16") == 0 ||
        strcmp(format, "S16P") == 0) return sizeof(int16_t);
    if (strcmp(format, "S32LE") == 0 || strcmp(format, "S32") == 0 ||
        strcmp(format, "S32P") == 0 ||
        strcmp(format, "F32LE") == 0 || strcmp(format, "F32") == 0 ||
        strcmp(format, "F32P") == 0 || strcmp(format, "FLTP") == 0) return sizeof(float);
    return sizeof(int16_t);
}

static const char*
wave_to_string(audio_wave_t wave)
{
    switch (wave) {
        case WAVE_SINE: return "sine";
        case WAVE_SQUARE: return "square";
        case WAVE_WHITE_NOISE: return "white-noise";
        case WAVE_PINK_NOISE: return "pink-noise";
        case WAVE_SILENCE: return "silence";
        case WAVE_STEREO_TONE: return "stereo-tone";
    }
    return "sine";
}

static bool
string_to_wave(const char* value, audio_wave_t* out)
{
    if (strcmp(value, "sine") == 0) {
        *out = WAVE_SINE;
    } else if (strcmp(value, "square") == 0) {
        *out = WAVE_SQUARE;
    } else if (strcmp(value, "white-noise") == 0 || strcmp(value, "white_noise") == 0 || strcmp(value, "noise") == 0) {
        *out = WAVE_WHITE_NOISE;
    } else if (strcmp(value, "pink-noise") == 0 || strcmp(value, "pink_noise") == 0) {
        *out = WAVE_PINK_NOISE;
    } else if (strcmp(value, "silence") == 0 || strcmp(value, "silent") == 0) {
        *out = WAVE_SILENCE;
    } else if (strcmp(value, "stereo-tone") == 0 || strcmp(value, "stereo_tone") == 0 ||
               strcmp(value, "stereo") == 0 || strcmp(value, "stereo-ident") == 0 ||
               strcmp(value, "stereo_ident") == 0 || strcmp(value, "stereo-1k-440") == 0 ||
               strcmp(value, "stereo_1k_440") == 0) {
        *out = WAVE_STEREO_TONE;
    } else {
        return false;
    }
    return true;
}

static void
audio_test_src_reset_signal_state(audio_test_src_t* s)
{
    s->phase = 0.0;
    s->rng_state = 0x12345678u;
    s->pink_b0 = 0.0;
    s->pink_b1 = 0.0;
    s->pink_b2 = 0.0;
    s->pink_b3 = 0.0;
    s->pink_b4 = 0.0;
    s->pink_b5 = 0.0;
    s->pink_b6 = 0.0;
}

static size_t
audio_test_src_buffer_size(const audio_test_src_t* s)
{
    return (size_t)s->samples_per_buffer * s->channels * bytes_per_sample_for_format(s->sample_format);
}

static zst_result_t
audio_test_src_create_pool(audio_test_src_t* s)
{
    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = audio_test_src_buffer_size(s),
        .buffer_type = ZST_BUFFER_AUDIO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    return s->pool ? ZST_OK : ZST_ERROR;
}

static zst_result_t
audio_test_src_recreate_pool_if_open(audio_test_src_t* s)
{
    if (!s->pool) return ZST_OK;
    zst_buffer_pool_destroy(s->pool);
    s->pool = NULL;
    return audio_test_src_create_pool(s);
}

static void
audio_test_src_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static zst_result_t
audio_test_src_open(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;

    s->sample_count = 0;
    s->buffer_count = 0;
    s->stopped = false;
    audio_test_src_reset_signal_state(s);

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    return audio_test_src_create_pool(s);
}

static zst_result_t
audio_test_src_close(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_result_t
audio_test_src_start(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;
    s->stopped = false;
    s->has_base_time = false;
    return ZST_OK;
}

static zst_result_t
audio_test_src_stop(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;
    s->stopped = true;
    return ZST_OK;
}

static double
audio_test_src_next_sample(audio_test_src_t* s)
{
    double sample = 0.0;

    switch (s->wave) {
        case WAVE_SINE:
            sample = fast_sine_from_phase(s->phase);
            break;
        case WAVE_SQUARE:
            sample = s->phase < 0.5 ? 1.0 : -1.0;
            break;
        case WAVE_WHITE_NOISE:
            sample = white_noise_sample(s);
            break;
        case WAVE_STEREO_TONE:
            sample = fast_sine_from_phase(s->phase);
            break;
    }

    double step = s->sample_rate > 0 ? s->frequency / (double)s->sample_rate : 0.0;
    s->phase += step;
    while (s->phase >= 1.0) s->phase -= 1.0;
    while (s->phase < 0.0) s->phase += 1.0;

    sample *= s->volume;
    if (sample > 1.0) sample = 1.0;
    if (sample < -1.0) sample = -1.0;
    return sample;
}

static double
audio_test_src_get_channel_sample(audio_test_src_t* s, uint64_t global_sample_index, double mono_sample, uint32_t ch)
{
    if (s->wave == WAVE_STEREO_TONE) {
        double freq = (ch % 2 == 0) ? 1000.0 : 440.0;
        double sr = s->sample_rate > 0 ? (double)s->sample_rate : 48000.0;
        double t = (double)global_sample_index / sr;
        double phase = t * freq;
        phase -= (int64_t)phase;
        if (phase < 0.0) phase += 1.0;
        double sample = fast_sine_from_phase(phase) * s->volume;
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        return sample;
    }
    return mono_sample;
}

static zst_result_t
audio_test_src_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    audio_test_src_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;

    if (s->stopped) {
        return ZST_EOF;
    }

    bool hit_buffer_limit = s->num_buffers >= 0 && s->buffer_count >= (uint64_t)s->num_buffers;
    bool hit_sample_limit = s->num_samples >= 0 && s->sample_count >= (uint64_t)s->num_samples;
    if (hit_buffer_limit || hit_sample_limit) {
        if (s->loop) {
            s->sample_count = 0;
            s->buffer_count = 0;
            s->has_base_time = false;
            audio_test_src_reset_signal_state(s);
        } else {
            return ZST_EOF;
        }
    }

    uint32_t nb_samples = s->samples_per_buffer;
    if (s->num_samples >= 0) {
        uint64_t remaining = (uint64_t)s->num_samples - s->sample_count;
        if (remaining < nb_samples) nb_samples = (uint32_t)remaining;
        if (nb_samples == 0) {
            return ZST_EOF;
        }
    }

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) return ZST_ERROR;

    uint8_t* raw_data = buf->memory.data;
    size_t bytes_per_sample = bytes_per_sample_for_format(s->sample_format);
    size_t data_size = (size_t)nb_samples * s->channels * bytes_per_sample;
    buf->memory.size = data_size;

    zst_audio_frame_t* frame = buf->payload;
    if (!frame) {
        frame = calloc(1, sizeof(*frame));
        if (!frame) {
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
        buf->payload = frame;
        buf->destroy = audio_test_src_buf_free;
    }

    frame->sample_rate = s->sample_rate;
    frame->channels = s->channels;
    frame->format = format_code_from_string(s->sample_format);
    frame->nb_samples = nb_samples;
    frame->data = raw_data;

    uint64_t start_sample = s->sample_count;

    if (is_planar_format(s->sample_format)) {
        /* Planar: each channel stored in a separate contiguous block */
        if (frame->format == ZST_AUDIO_FMT_F32P) {
            float* base = (float*)raw_data;
            uint32_t plane_samples = nb_samples;
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                float* ch_buf = base + ch * plane_samples;
                for (uint32_t i = 0; i < nb_samples; i++) {
                    double mono_sample = fast_sine_from_phase(s->phase);
                    ch_buf[i] = (float)audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                }
            }
        } else if (frame->format == ZST_AUDIO_FMT_S32P) {
            int32_t* base = (int32_t*)raw_data;
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                int32_t* ch_buf = base + ch * nb_samples;
                for (uint32_t i = 0; i < nb_samples; i++) {
                    double mono_sample = fast_sine_from_phase(s->phase);
                    double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                    ch_buf[i] = (int32_t)(sample * 2147483647.0);
                }
            }
        } else {
            /* S16P (planar S16) */
            int16_t* base = (int16_t*)raw_data;
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                int16_t* ch_buf = base + ch * nb_samples;
                for (uint32_t i = 0; i < nb_samples; i++) {
                    double mono_sample = fast_sine_from_phase(s->phase);
                    double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                    ch_buf[i] = (int16_t)(sample * 32767.0);
                }
            }
        }
    } else if (frame->format == ZST_AUDIO_FMT_F32LE) {
        float* pcm = (float*)raw_data;
        for (uint32_t i = 0; i < nb_samples; i++) {
            double mono_sample = audio_test_src_next_sample(s);
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                pcm[i * s->channels + ch] = (float)sample;
            }
        }
    } else if (frame->format == ZST_AUDIO_FMT_S32LE) {
        int32_t* pcm = (int32_t*)raw_data;
        for (uint32_t i = 0; i < nb_samples; i++) {
            double mono_sample = audio_test_src_next_sample(s);
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                pcm[i * s->channels + ch] = (int32_t)(sample * 2147483647.0);
            }
        }
    } else if (frame->format == ZST_AUDIO_FMT_U8) {
        uint8_t* pcm = (uint8_t*)raw_data;
        for (uint32_t i = 0; i < nb_samples; i++) {
            double mono_sample = audio_test_src_next_sample(s);
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                pcm[i * s->channels + ch] = (uint8_t)((sample + 1.0) * 127.5);
            }
        }
    } else {
        /* S16LE (default) */
        int16_t* pcm = (int16_t*)raw_data;
        for (uint32_t i = 0; i < nb_samples; i++) {
            double mono_sample = audio_test_src_next_sample(s);
            for (uint32_t ch = 0; ch < s->channels; ch++) {
                double sample = audio_test_src_get_channel_sample(s, start_sample + i, mono_sample, ch);
                pcm[i * s->channels + ch] = (int16_t)(sample * 32767.0);
            }
        }
    }

    s->sample_count += nb_samples;
    s->buffer_count++;

    if (s->use_clock && el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = start_sample * 1000000000ULL / s->sample_rate;
    }
    buf->dts = buf->pts;
    buf->duration = (uint64_t)nb_samples * 1000000000ULL / s->sample_rate;

    if (s->real_time_pacing && el->clock) {
        zst_time_t current_time = zst_clock_get_time(el->clock);
        if (!s->has_base_time) {
            s->base_time = current_time;
            s->has_base_time = true;
        }
        zst_time_t expected_offset = start_sample * 1000000000ULL / s->sample_rate;
        zst_time_t expected_time = s->base_time + expected_offset;
        if (expected_time > current_time) {
            zst_clock_wait(el->clock, expected_time - current_time);
        }
    }

    *out = buf;
    return ZST_OK;
}

static zst_caps_t*
audio_test_src_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)pad;
    (void)filter;
    audio_test_src_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;
    zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", (int)s->channels, (int)s->sample_rate, s->sample_format));
    return caps;
}

static zst_result_t
audio_test_src_set_property(zst_element_t* el, const char* name, const char* value)
{
    audio_test_src_t* s = el->priv;

    if (strcmp(name, "sample-rate") == 0 || strcmp(name, "rate") == 0) {
        int v = atoi(value);
        if (v <= 0) return ZST_ERROR;
        s->sample_rate = (uint32_t)v;
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        int v = atoi(value);
        if (v <= 0) return ZST_ERROR;
        s->channels = (uint32_t)v;
        return audio_test_src_recreate_pool_if_open(s);
    } else if (strcmp(name, "sample-format") == 0 || strcmp(name, "format") == 0) {
        /* Accept any known format string; normalize short aliases */
        const char* normalized = value;
        if (strcmp(value, "S16") == 0) normalized = "S16LE";
        else if (strcmp(value, "S32") == 0) normalized = "S32LE";
        else if (strcmp(value, "F32") == 0) normalized = "F32LE";
        else if (strcmp(value, "FLTP") == 0) normalized = "F32P";
        /* Validate */
        if (strcmp(normalized, "S16LE") != 0 && strcmp(normalized, "S32LE") != 0 &&
            strcmp(normalized, "F32LE") != 0 && strcmp(normalized, "U8") != 0 &&
            strcmp(normalized, "S16P") != 0 && strcmp(normalized, "S32P") != 0 &&
            strcmp(normalized, "F32P") != 0) {
            return ZST_ERROR;
        }
        snprintf(s->sample_format, sizeof(s->sample_format), "%s", normalized);
        return audio_test_src_recreate_pool_if_open(s);
    } else if (strcmp(name, "wave") == 0 || strcmp(name, "signal") == 0) {
        audio_wave_t wave;
        if (!string_to_wave(value, &wave)) return ZST_ERROR;
        s->wave = wave;
        return ZST_OK;
    } else if (strcmp(name, "frequency") == 0 || strcmp(name, "freq") == 0) {
        double v = atof(value);
        if (v < 0.0) return ZST_ERROR;
        s->frequency = v;
        return ZST_OK;
    } else if (strcmp(name, "volume") == 0 || strcmp(name, "amplitude") == 0) {
        double v = atof(value);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        s->volume = v;
        return ZST_OK;
    } else if (strcmp(name, "samples-per-buffer") == 0 || strcmp(name, "block-size") == 0) {
        int v = atoi(value);
        if (v <= 0) return ZST_ERROR;
        s->samples_per_buffer = (uint32_t)v;
        return audio_test_src_recreate_pool_if_open(s);
    } else if (strcmp(name, "num-samples") == 0) {
        s->num_samples = atoll(value);
        if (s->num_samples < -1) return ZST_ERROR;
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0) {
        s->num_buffers = atoll(value);
        if (s->num_buffers < -1) return ZST_ERROR;
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        s->loop = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        s->use_clock = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        s->real_time_pacing = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
audio_test_src_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    audio_test_src_t* s = el->priv;

    if (strcmp(name, "sample-rate") == 0 || strcmp(name, "rate") == 0) {
        snprintf(value_out, max_len, "%u", s->sample_rate);
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%u", s->channels);
        return ZST_OK;
    } else if (strcmp(name, "sample-format") == 0 || strcmp(name, "format") == 0) {
        snprintf(value_out, max_len, "%s", s->sample_format);
        return ZST_OK;
    } else if (strcmp(name, "wave") == 0 || strcmp(name, "signal") == 0) {
        snprintf(value_out, max_len, "%s", wave_to_string(s->wave));
        return ZST_OK;
    } else if (strcmp(name, "frequency") == 0 || strcmp(name, "freq") == 0) {
        snprintf(value_out, max_len, "%g", s->frequency);
        return ZST_OK;
    } else if (strcmp(name, "volume") == 0 || strcmp(name, "amplitude") == 0) {
        snprintf(value_out, max_len, "%g", s->volume);
        return ZST_OK;
    } else if (strcmp(name, "samples-per-buffer") == 0 || strcmp(name, "block-size") == 0) {
        snprintf(value_out, max_len, "%u", s->samples_per_buffer);
        return ZST_OK;
    } else if (strcmp(name, "num-samples") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->num_samples);
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->num_buffers);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        snprintf(value_out, max_len, "%s", s->loop ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        snprintf(value_out, max_len, "%s", s->use_clock ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        snprintf(value_out, max_len, "%s", s->real_time_pacing ? "true" : "false");
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_time_t
audio_test_src_clock_get_time(zst_clock_t* clock)
{
    audio_test_src_t* s = clock->priv;
    if (!s || s->sample_rate == 0) return 0;
    return s->sample_count * 1000000000ULL / s->sample_rate;
}

static void
audio_test_src_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    (void)clock;
    (void)time;
}

static void
audio_test_src_clock_destroy(zst_clock_t* clock)
{
    (void)clock;
}

static zst_clock_t*
audio_test_src_provide_clock(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;
    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    clock->refcount = 1;
    clock->get_time = audio_test_src_clock_get_time;
    clock->wait = audio_test_src_clock_wait;
    clock->destroy = audio_test_src_clock_destroy;
    clock->priv = s;
    return clock;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    audio_test_src_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "audiotestsrc",
    .open = audio_test_src_open,
    .close = audio_test_src_close,
    .start = audio_test_src_start,
    .stop = audio_test_src_stop,
    .process = audio_test_src_process,
    .get_caps = audio_test_src_get_caps,
    .provide_clock = audio_test_src_provide_clock,
    .set_property = audio_test_src_set_property,
    .get_property = audio_test_src_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_audio_test_src_create(void)
{
    audio_test_src_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->sample_rate = 44100;
    priv->channels = 2;
    priv->samples_per_buffer = 1024;
    snprintf(priv->sample_format, sizeof(priv->sample_format), "%s", "S16LE");
    priv->wave = WAVE_SINE;
    priv->frequency = 440.0;
    priv->volume = 0.8;
    priv->num_samples = -1;
    priv->num_buffers = -1;
    priv->loop = false;
    priv->use_clock = false;
    priv->real_time_pacing = false;
    priv->has_base_time = false;
    audio_test_src_reset_signal_state(priv);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    if (!src || zst_element_add_pad(el, src) != ZST_OK) {
        if (src) zst_pad_destroy(src);
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}

zst_element_t*
zst_audio_test_src_create_with_config(const zst_audio_test_src_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_audio_test_src_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("audiotestsrc");
    if (!el) return NULL;

    if (config->sample_rate > 0) {
        zst_element_set_property_uint(el, "sample-rate", config->sample_rate);
    }
    if (config->channels > 0) {
        zst_element_set_property_uint(el, "channels", config->channels);
    }
    if (config->sample_format) {
        zst_element_set_property_string(el, "sample-format", config->sample_format);
    }
    if (config->wave) {
        zst_element_set_property_string(el, "wave", config->wave);
    }
    if (config->frequency >= 0.0) {
        zst_element_set_property_double(el, "frequency", config->frequency);
    }
    if (config->volume >= 0.0) {
        zst_element_set_property_double(el, "volume", config->volume);
    }
    if (config->samples_per_buffer > 0) {
        zst_element_set_property_uint(el, "samples-per-buffer", config->samples_per_buffer);
    }
    zst_element_set_property_int(el, "num-samples", config->num_samples);
    zst_element_set_property_int(el, "num-buffers", config->num_buffers);
    zst_element_set_property_bool(el, "loop", config->loop);
    zst_element_set_property_bool(el, "use-clock", config->use_clock);
    zst_element_set_property_bool(el, "real-time-pacing", config->real_time_pacing);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "audiotestsrc") == 0 || strcmp(name, "audio_test_src") == 0) {
        return zst_audio_test_src_create();
    }
    return NULL;
}

static const zst_pad_template_t g_audiotestsrc_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" }
};

static const zst_element_desc_t g_audiotestsrc_elements[] = {
    {
        .name = "audiotestsrc",
        .long_name = "Audio Test Source",
        .category = "Source/Test",
        .description = "Generates synthetic audio test signals",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_audiotestsrc_pads,
        .nb_pads = sizeof(g_audiotestsrc_pads) / sizeof(g_audiotestsrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "audiotestsrc_plugin",
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
        *nb_elements_out = sizeof(g_audiotestsrc_elements) / sizeof(g_audiotestsrc_elements[0]);
    }
    return g_audiotestsrc_elements;
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
