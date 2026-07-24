/*=============================================================================
    alsa_source.c — ALSA audio source with mock synthetic fallback
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

/* Audio format codes (mirrors audio_test_src.c) */
#define ALSA_FMT_S16LE  0u
#define ALSA_FMT_S32LE  1u
#define ALSA_FMT_F32LE  3u
#define ALSA_FMT_U8     4u

typedef struct {
    snd_pcm_t*      handle;
    int             is_mock;
    uint64_t        sample_count;
    uint32_t        sample_rate;
    uint32_t        channels;
    char            sample_format[16];
    uint32_t        format_code;
    uint32_t        bytes_per_sample;
    uint32_t        latency_us;   /* ALSA period/latency in microseconds */

    zst_buffer_pool_t* pool;

    char            device[128];
} alsa_source_t;

/* ── Format helpers ────────────────────────────────────────────────────── */

static uint32_t
format_code_from_str(const char* fmt)
{
    if (!fmt) return ALSA_FMT_S16LE;
    if (strcmp(fmt, "S32LE") == 0 || strcmp(fmt, "S32") == 0) return ALSA_FMT_S32LE;
    if (strcmp(fmt, "F32LE") == 0 || strcmp(fmt, "F32") == 0) return ALSA_FMT_F32LE;
    if (strcmp(fmt, "U8") == 0) return ALSA_FMT_U8;
    return ALSA_FMT_S16LE;
}

static uint32_t
bytes_per_sample_for_code(uint32_t code)
{
    switch (code) {
        case ALSA_FMT_U8:    return 1;
        case ALSA_FMT_S16LE: return 2;
        case ALSA_FMT_S32LE: return 4;
        case ALSA_FMT_F32LE: return 4;
    }
    return 2;
}

static snd_pcm_format_t
alsa_format_from_code(uint32_t code)
{
    switch (code) {
        case ALSA_FMT_U8:    return SND_PCM_FORMAT_U8;
        case ALSA_FMT_S16LE: return SND_PCM_FORMAT_S16_LE;
        case ALSA_FMT_S32LE: return SND_PCM_FORMAT_S32_LE;
        case ALSA_FMT_F32LE: return SND_PCM_FORMAT_FLOAT_LE;
    }
    return SND_PCM_FORMAT_S16_LE;
}

/* ── Buffer management ─────────────────────────────────────────────────── */

static void
alsa_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}

static size_t
alsa_buffer_size(alsa_source_t* s)
{
    return 1024 * s->channels * s->bytes_per_sample;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

static zst_result_t
alsa_open(zst_element_t* el)
{
    alsa_source_t* s = el->priv;

    if (s->sample_rate == 0) s->sample_rate = 44100;
    if (s->channels == 0)    s->channels = 2;
    s->sample_count = 0;
    s->handle = NULL;

    /* Resolve format code */
    s->format_code = format_code_from_str(s->sample_format);
    s->bytes_per_sample = bytes_per_sample_for_code(s->format_code);

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = alsa_buffer_size(s),
        .buffer_type = ZST_BUFFER_AUDIO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    const char* dev_name = s->device[0] ? s->device : "default";
    int err = snd_pcm_open(&s->handle, dev_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        ZST_LOG_WARN("alsasrc", "Failed to open default ALSA capture device: %s. Falling back to synthetic source.", snd_strerror(err));
        s->is_mock = 1;
        s->handle = NULL;
        return ZST_OK;
    }

    err = snd_pcm_set_params(s->handle,
                             alsa_format_from_code(s->format_code),
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             s->channels,
                             s->sample_rate,
                             1, // soft resample
                             s->latency_us);
    if (err < 0) {
        ZST_LOG_WARN("alsasrc", "Failed to set parameters: %s. Falling back to synthetic source.", snd_strerror(err));
        snd_pcm_close(s->handle);
        s->handle = NULL;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->is_mock = 0;
    return ZST_OK;
}

static zst_result_t
alsa_close(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    if (s->handle) {
        snd_pcm_close(s->handle);
        s->handle = NULL;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    return ZST_OK;
}

static zst_result_t
alsa_start(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    if (s->handle && !s->is_mock) {
        snd_pcm_prepare(s->handle);
    }
    return ZST_OK;
}

/* ── Mock signal generation ────────────────────────────────────────────── */

static void
mock_generate_s16(alsa_source_t* s, uint8_t* data, uint32_t nb_samples)
{
    int16_t* pcm = (int16_t*)data;
    for (uint32_t i = 0; i < nb_samples; i++) {
        int phase = (s->sample_count + i) % 100;
        int16_t val = (phase < 50) ? 8000 : -8000;
        for (uint32_t ch = 0; ch < s->channels; ch++) {
            pcm[i * s->channels + ch] = val;
        }
    }
}

static void
mock_generate_s32(alsa_source_t* s, uint8_t* data, uint32_t nb_samples)
{
    int32_t* pcm = (int32_t*)data;
    for (uint32_t i = 0; i < nb_samples; i++) {
        int phase = (s->sample_count + i) % 100;
        int32_t val = (phase < 50) ? 0x20000000 : -0x20000000;
        for (uint32_t ch = 0; ch < s->channels; ch++) {
            pcm[i * s->channels + ch] = val;
        }
    }
}

static void
mock_generate_f32(alsa_source_t* s, uint8_t* data, uint32_t nb_samples)
{
    float* pcm = (float*)data;
    for (uint32_t i = 0; i < nb_samples; i++) {
        int phase = (s->sample_count + i) % 100;
        float val = (phase < 50) ? 0.5f : -0.5f;
        for (uint32_t ch = 0; ch < s->channels; ch++) {
            pcm[i * s->channels + ch] = val;
        }
    }
}

static void
mock_generate_u8(alsa_source_t* s, uint8_t* data, uint32_t nb_samples)
{
    for (uint32_t i = 0; i < nb_samples; i++) {
        int phase = (s->sample_count + i) % 100;
        uint8_t val = (phase < 50) ? 200 : 56;
        for (uint32_t ch = 0; ch < s->channels; ch++) {
            data[i * s->channels + ch] = val;
        }
    }
}

static void
mock_generate(alsa_source_t* s, uint8_t* data, uint32_t nb_samples)
{
    switch (s->format_code) {
        case ALSA_FMT_S32LE: mock_generate_s32(s, data, nb_samples); return;
        case ALSA_FMT_F32LE: mock_generate_f32(s, data, nb_samples); return;
        case ALSA_FMT_U8:    mock_generate_u8(s, data, nb_samples); return;
        default:             mock_generate_s16(s, data, nb_samples); return;
    }
}

/* ── Process ───────────────────────────────────────────────────────────── */

static zst_result_t
alsa_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    alsa_source_t* s = el->priv;

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        return ZST_ERROR;
    }

    uint32_t nb_samples = 1024;
    size_t data_size = alsa_buffer_size(s);
    uint8_t* raw_data = buf->memory.data;

    zst_audio_frame_t* frame = buf->payload;
    if (!frame) {
        frame = calloc(1, sizeof(*frame));
        if (!frame) {
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
        buf->payload = frame;
        buf->destroy = alsa_buf_free;
    }

    frame->sample_rate = s->sample_rate;
    frame->channels = s->channels;
    frame->format = s->format_code;
    frame->nb_samples = nb_samples;
    frame->data = raw_data;

    if (s->is_mock) {
        mock_generate(s, raw_data, nb_samples);
        s->sample_count += nb_samples;

        /* Simulate timing based on sample rate */
        zst_time_t dur_ns = nb_samples * 1000000000ULL / s->sample_rate;
        struct timespec ts = {
            .tv_sec = dur_ns / 1000000000ULL,
            .tv_nsec = dur_ns % 1000000000ULL
        };
        nanosleep(&ts, NULL);
    } else {
        snd_pcm_sframes_t frames = snd_pcm_readi(s->handle, raw_data, nb_samples);
        if (frames < 0) {
            if (frames == -EPIPE) {
                snd_pcm_prepare(s->handle);
            }
            memset(raw_data, 0, data_size);
        } else if (frames < (snd_pcm_sframes_t)nb_samples) {
            size_t read_bytes = frames * s->channels * s->bytes_per_sample;
            memset(raw_data + read_bytes, 0, data_size - read_bytes);
        }
        s->sample_count += nb_samples;
    }

    /* Set nanosecond PTS */
    if (el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = (s->sample_count - nb_samples) * 1000000000ULL / s->sample_rate;
    }
    buf->duration = nb_samples * 1000000000ULL / s->sample_rate;

    *out = buf;
    return ZST_OK;
}

/* ── Clock ─────────────────────────────────────────────────────────────── */

static zst_time_t
alsa_clock_get_time(zst_clock_t* clock)
{
    alsa_source_t* s = clock->priv;
    if (s->sample_rate == 0) return 0;
    return s->sample_count * 1000000000ULL / s->sample_rate;
}

static void
alsa_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    (void)clock;
    (void)time;
}

static void
alsa_clock_destroy(zst_clock_t* clock)
{
    (void)clock;
}

static zst_clock_t*
alsa_provide_clock(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    clock->refcount = 1;
    clock->get_time = alsa_clock_get_time;
    clock->wait     = alsa_clock_wait;
    clock->destroy  = alsa_clock_destroy;
    clock->priv     = s;

    return clock;
}

/* ── Pool ──────────────────────────────────────────────────────────────── */

static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    return s->pool;
}

/* ── Properties ────────────────────────────────────────────────────────── */

static zst_result_t
alsa_set_property(zst_element_t* el, const char* name, const char* value)
{
    alsa_source_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(s->device, sizeof(s->device), "%s", value);
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
    } else if (strcmp(name, "sample-format") == 0 || strcmp(name, "format") == 0) {
        /* Normalize short aliases */
        const char* normalized = value;
        if (strcmp(value, "S16") == 0) normalized = "S16LE";
        else if (strcmp(value, "S32") == 0) normalized = "S32LE";
        else if (strcmp(value, "F32") == 0) normalized = "F32LE";
        /* Validate */
        if (strcmp(normalized, "S16LE") != 0 && strcmp(normalized, "S32LE") != 0 &&
            strcmp(normalized, "F32LE") != 0 && strcmp(normalized, "U8") != 0) {
            return ZST_ERROR;
        }
        snprintf(s->sample_format, sizeof(s->sample_format), "%s", normalized);
        return ZST_OK;
    } else if (strcmp(name, "latency") == 0) {
        s->latency_us = (uint32_t)strtoul(value, NULL, 10);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
alsa_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    alsa_source_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(value_out, max_len, "%s", s->device);
    } else if (strcmp(name, "sample-rate") == 0) {
        snprintf(value_out, max_len, "%u", s->sample_rate);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%u", s->channels);
    } else if (strcmp(name, "sample-format") == 0 || strcmp(name, "format") == 0) {
        snprintf(value_out, max_len, "%s", s->sample_format);
    } else if (strcmp(name, "latency") == 0) {
        snprintf(value_out, max_len, "%u", s->latency_us);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

/* ── Element ops ───────────────────────────────────────────────────────── */

static zst_element_ops_t g_ops = {
    .name          = "alsasrc",
    .open          = alsa_open,
    .close         = alsa_close,
    .start         = alsa_start,
    .process       = alsa_process,
    .provide_clock = alsa_provide_clock,
    .set_property  = alsa_set_property,
    .get_property  = alsa_get_property,
    .get_pool      = element_get_pool
};

/* ── Public API ────────────────────────────────────────────────────────── */

zst_element_t*
zst_alsa_source_create(void)
{
    alsa_source_t* priv = calloc(1, sizeof(alsa_source_t));
    if (!priv) return NULL;

    /* Defaults */
    snprintf(priv->sample_format, sizeof(priv->sample_format), "S16LE");
    priv->format_code = ALSA_FMT_S16LE;
    priv->bytes_per_sample = 2;
    priv->latency_us = 500000; /* 0.5s default */

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

/* ── Plugin registration ───────────────────────────────────────────────── */

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "alsasrc") == 0) {
        return zst_alsa_source_create();
    }
    return NULL;
}

static const zst_pad_template_t g_alsasrc_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/x-raw" }
};

static const zst_element_desc_t g_alsasrc_elements[] = {
    {
        .name = "alsasrc",
        .long_name = "ALSA Source",
        .category = "Source/Audio",
        .description = "Captures audio from ALSA",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_alsasrc_pads,
        .nb_pads = sizeof(g_alsasrc_pads) / sizeof(g_alsasrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "alsasource_plugin",
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
        *nb_elements_out = sizeof(g_alsasrc_elements) / sizeof(g_alsasrc_elements[0]);
    }
    return g_alsasrc_elements;
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
