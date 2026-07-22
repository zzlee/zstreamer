/*=============================================================================
    alsa_sink.c — ALSA audio sink with mock synthetic fallback
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zstreamer/elements/zst_alsa_sink.h"

typedef struct {
    snd_pcm_t*      handle;
    int             is_mock;
    uint32_t        sample_rate;
    uint32_t        channels;
    char            device[128];
    int             started;
} alsa_sink_t;

static zst_result_t
alsa_sink_open(zst_element_t* el)
{
    alsa_sink_t* s = el->priv;

    if (s->sample_rate == 0) s->sample_rate = 44100;
    if (s->channels == 0)    s->channels = 2;
    s->handle = NULL;

    const char* dev_name = s->device[0] ? s->device : "default";
    int err = snd_pcm_open(&s->handle, dev_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        ZST_LOG_WARN("alsasink", "Failed to open ALSA playback device '%s': %s. Falling back to synthetic sink.", dev_name, snd_strerror(err));
        s->is_mock = 1;
        s->handle = NULL;
        return ZST_OK;
    }

    err = snd_pcm_set_params(s->handle,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             s->channels,
                             s->sample_rate,
                             1, // soft resample
                             500000); // 0.5s latency
    if (err < 0) {
        ZST_LOG_WARN("alsasink", "Failed to set PCM parameters: %s. Falling back to synthetic sink.", snd_strerror(err));
        snd_pcm_close(s->handle);
        s->handle = NULL;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->is_mock = 0;
    return ZST_OK;
}

static zst_result_t
alsa_sink_close(zst_element_t* el)
{
    alsa_sink_t* s = el->priv;
    if (s->handle) {
        snd_pcm_close(s->handle);
        s->handle = NULL;
    }
    return ZST_OK;
}

static zst_result_t
alsa_sink_start(zst_element_t* el)
{
    alsa_sink_t* s = el->priv;
    if (s->handle && !s->is_mock) {
        snd_pcm_prepare(s->handle);
    }
    s->started = 1;
    return ZST_OK;
}

static zst_result_t
alsa_sink_stop(zst_element_t* el)
{
    alsa_sink_t* s = el->priv;
    if (s->handle && !s->is_mock) {
        snd_pcm_drop(s->handle);
    }
    s->started = 0;
    return ZST_OK;
}

static zst_result_t
alsa_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    alsa_sink_t* s = el->priv;

    if (!in || !s->started) {
        return ZST_ERROR;
    }

    zst_audio_frame_t* frame = in->payload;
    uint8_t* raw_data = NULL;
    uint32_t nb_samples = 0;
    uint32_t sample_rate = s->sample_rate;

    if (frame) {
        raw_data = frame->data;
        nb_samples = frame->nb_samples;
        sample_rate = frame->sample_rate;
    } else {
        raw_data = in->memory.data;
        nb_samples = in->memory.size / (s->channels * sizeof(int16_t));
    }

    if (!raw_data || nb_samples == 0) {
        return ZST_OK;
    }

    if (s->is_mock) {
        /* Simulate playback timing */
        zst_time_t duration = nb_samples * 1000000000ULL / sample_rate;
        struct timespec ts = {
            .tv_sec = duration / 1000000000ULL,
            .tv_nsec = duration % 1000000000ULL
        };
        nanosleep(&ts, NULL);
    } else {
        snd_pcm_sframes_t frames = snd_pcm_writei(s->handle, raw_data, nb_samples);
        if (frames < 0) {
            if (frames == -EPIPE) {
                snd_pcm_prepare(s->handle);
                frames = snd_pcm_writei(s->handle, raw_data, nb_samples);
            }
            if (frames < 0) {
                ZST_LOG_ERROR("alsasink", "ALSA writei failed: %s", snd_strerror(frames));
                return ZST_ERROR;
            }
        }
    }

    return ZST_OK;
}

static zst_result_t
alsa_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    alsa_sink_t* s = el->priv;
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
    }
    return ZST_ERROR;
}

static zst_result_t
alsa_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    alsa_sink_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(value_out, max_len, "%s", s->device);
    } else if (strcmp(name, "sample-rate") == 0) {
        snprintf(value_out, max_len, "%u", s->sample_rate);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%u", s->channels);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name          = "alsasink",
    .open          = alsa_sink_open,
    .close         = alsa_sink_close,
    .start         = alsa_sink_start,
    .stop          = alsa_sink_stop,
    .process       = alsa_sink_process,
    .set_property  = alsa_sink_set_property,
    .get_property  = alsa_sink_get_property,
};

zst_element_t*
zst_alsa_sink_create(void)
{
    zst_element_t* el;
    alsa_sink_t* priv;
    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "alsasink") == 0) {
        return zst_alsa_sink_create();
    }
    return NULL;
}

static const zst_pad_template_t g_alsasink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-raw" }
};

static const zst_element_desc_t g_alsasink_elements[] = {
    {
        .name = "alsasink",
        .long_name = "ALSA Sink",
        .category = "Sink/Audio",
        .description = "Outputs audio to ALSA",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_alsasink_pads,
        .nb_pads = sizeof(g_alsasink_pads) / sizeof(g_alsasink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "alsasink_plugin",
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
        *nb_elements_out = sizeof(g_alsasink_elements) / sizeof(g_alsasink_elements[0]);
    }
    return g_alsasink_elements;
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
