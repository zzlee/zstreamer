#define _POSIX_C_SOURCE 200809L

#include "dante_dep_context.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEP_AUDIO_FORMAT_S32LE 1u

typedef struct {
    char shm_name[256];
    char channels[1024];
    uint32_t channel_count;
    uint32_t queue_periods;
    uint32_t tx_lead_us;
    uint32_t reconnect_ms;
    uint32_t expected_sample_rate;
    bool endpoint_ready;
    dep_context_t* context;
    dep_endpoint_t endpoint;
} dep_sink_t;

static bool parse_uint(const char* text, uint32_t minimum, uint32_t maximum,
                       uint32_t* value_out)
{
    char* end;
    unsigned long value;
    if (!text || !*text || *text == '-' || *text == '+') return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum) return false;
    *value_out = (uint32_t)value;
    return true;
}

static bool channel_list_count(const char* text, uint32_t* count_out)
{
    uint32_t count = 0;
    const char* cursor = text;
    uint32_t seen[256];
    if (!text || !*text) return false;
    while (*cursor) {
        char* end;
        unsigned long value;
        if (count == 256 || *cursor == '-' || *cursor == '+' ||
            *cursor == ' ' || *cursor == '\t') return false;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno || end == cursor || value > UINT32_MAX ||
            (*end != ',' && *end != '\0')) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (seen[i] == (uint32_t)value) return false;
        seen[count++] = (uint32_t)value;
        if (!*end) break;
        cursor = end + 1;
        if (!*cursor) return false;
    }
    *count_out = count;
    return count != 0;
}

static zst_result_t sink_open(zst_element_t* element)
{
    dep_sink_t* sink = element->priv;
    if (dep_endpoint_init(&sink->endpoint, DEP_ENDPOINT_TX, sink->channels,
                          sink->queue_periods, 0, sink->tx_lead_us,
                          sink->reconnect_ms) != ZST_OK) return ZST_ERROR;
    sink->endpoint_ready = true;
    sink->context = dep_context_acquire(sink->shm_name);
    if (!sink->context || dep_context_attach(sink->context, &sink->endpoint) != ZST_OK) {
        if (sink->context) dep_context_release(sink->context);
        sink->context = NULL;
        dep_endpoint_deinit(&sink->endpoint);
        sink->endpoint_ready = false;
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t sink_close(zst_element_t* element)
{
    dep_sink_t* sink = element->priv;
    dep_context_t* context = sink->context;
    if (sink->endpoint_ready) {
        dep_endpoint_deinit(&sink->endpoint);
        sink->endpoint_ready = false;
    }
    sink->context = NULL;
    dep_context_release(context);
    return ZST_OK;
}

static zst_result_t sink_start(zst_element_t* element)
{
    dep_sink_t* sink = element->priv;
    return sink->endpoint_ready ? dep_endpoint_start(&sink->endpoint) : ZST_ERROR;
}

static zst_result_t sink_stop(zst_element_t* element)
{
    dep_sink_t* sink = element->priv;
    if (sink->endpoint_ready) dep_endpoint_stop(&sink->endpoint);
    return ZST_OK;
}

static zst_result_t sink_process(zst_element_t* element, zst_buffer_t* input,
                                 zst_buffer_t** output)
{
    dep_sink_t* sink = element->priv;
    zst_audio_frame_t* audio;
    const int32_t* samples;
    uint32_t frames;
    size_t required;
    if (output) *output = NULL;
    if (!input || !sink->endpoint_ready || sink->channel_count == 0) return ZST_ERROR;
    audio = input->payload;
    if (audio) {
        if (audio->format != DEP_AUDIO_FORMAT_S32LE ||
            audio->channels != sink->channel_count || !audio->data ||
            input->memory.data != audio->data) return ZST_ERROR;
        uint32_t configured_rate = dep_endpoint_sample_rate(&sink->endpoint);
        if (configured_rate && audio->sample_rate != configured_rate) return ZST_ERROR;
        if (sink->expected_sample_rate &&
            audio->sample_rate != sink->expected_sample_rate) return ZST_ERROR;
        samples = audio->data;
        frames = audio->nb_samples;
    } else {
        samples = input->memory.data;
        if (!samples || input->memory.size % (sink->channel_count * sizeof(*samples)) != 0)
            return ZST_ERROR;
        size_t count = input->memory.size / (sink->channel_count * sizeof(*samples));
        if (count > UINT32_MAX) return ZST_ERROR;
        frames = (uint32_t)count;
    }
    if (frames == 0 || (uint64_t)frames * sink->channel_count >
        SIZE_MAX / sizeof(*samples)) return ZST_ERROR;
    required = (size_t)((uint64_t)frames * sink->channel_count * sizeof(*samples));
    if (input->memory.size != required) return ZST_ERROR;
    return dep_endpoint_write(&sink->endpoint, samples, frames);
}

static zst_caps_t* sink_caps(zst_element_t* element, zst_pad_t* pad,
                             const zst_caps_t* filter)
{
    dep_sink_t* sink = element->priv;
    uint32_t rate = sink->endpoint_ready ? dep_endpoint_sample_rate(&sink->endpoint) : 0;
    zst_caps_t* caps;
    (void)pad;
    (void)filter;
    if (!rate) rate = sink->expected_sample_rate;
    caps = zst_caps_create();
    if (!caps || (rate && zst_caps_append(caps, zst_caps_struct_create_audio(
            "audio/x-raw", (int)sink->channel_count, (int)rate, "S32LE")
            ) != ZST_OK)) {
        zst_caps_destroy(caps);
        return NULL;
    }
    return caps;
}

static zst_result_t sink_set_property(zst_element_t* element, const char* name,
                                      const char* value)
{
    dep_sink_t* sink = element->priv;
    uint32_t number;
    if (!name || !value || atomic_load_explicit(&element->state, memory_order_acquire) != ZST_STATE_NULL)
        return ZST_ERROR;
    if (strcmp(name, "shm-name") == 0) {
        if (!*value || strlen(value) >= sizeof(sink->shm_name)) return ZST_ERROR;
        strcpy(sink->shm_name, value);
        return ZST_OK;
    }
    if (strcmp(name, "channels") == 0) {
        if (strlen(value) >= sizeof(sink->channels) ||
            !channel_list_count(value, &number)) return ZST_ERROR;
        strcpy(sink->channels, value);
        sink->channel_count = number;
        return ZST_OK;
    }
    if (strcmp(name, "queue-periods") == 0) {
        if (!parse_uint(value, 1, 4096, &number)) return ZST_ERROR;
        sink->queue_periods = number;
        return ZST_OK;
    }
    if (strcmp(name, "tx-lead-us") == 0) {
        if (!parse_uint(value, 0, UINT32_MAX, &number)) return ZST_ERROR;
        sink->tx_lead_us = number;
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-interval-ms") == 0) {
        if (!parse_uint(value, 1, 60000, &number)) return ZST_ERROR;
        sink->reconnect_ms = number;
        return ZST_OK;
    }
    if (strcmp(name, "expected-sample-rate") == 0) {
        if (!parse_uint(value, 1, UINT32_MAX, &number)) return ZST_ERROR;
        sink->expected_sample_rate = number;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t sink_get_property(zst_element_t* element, const char* name,
                                      char* output, size_t size)
{
    dep_sink_t* sink = element->priv;
    if (!name || !output || size == 0) return ZST_ERROR;
#define RETURN_TEXT(value) do { snprintf(output, size, "%s", (value)); return ZST_OK; } while (0)
#define RETURN_NUMBER(value) do { snprintf(output, size, "%llu", (unsigned long long)(value)); return ZST_OK; } while (0)
    if (strcmp(name, "shm-name") == 0) RETURN_TEXT(sink->shm_name);
    if (strcmp(name, "channels") == 0) RETURN_TEXT(sink->channels);
    if (strcmp(name, "queue-periods") == 0) RETURN_NUMBER(sink->queue_periods);
    if (strcmp(name, "tx-lead-us") == 0) RETURN_NUMBER(sink->tx_lead_us);
    if (strcmp(name, "reconnect-interval-ms") == 0) RETURN_NUMBER(sink->reconnect_ms);
    if (strcmp(name, "expected-sample-rate") == 0) RETURN_NUMBER(sink->expected_sample_rate);
    if (strcmp(name, "sample-rate") == 0)
        RETURN_NUMBER(sink->endpoint_ready ? dep_endpoint_sample_rate(&sink->endpoint) : 0);
    if (strcmp(name, "active") == 0)
        RETURN_TEXT(sink->endpoint_ready && dep_endpoint_is_active(&sink->endpoint) ? "true" : "false");
    if (strcmp(name, "periods") == 0 || strcmp(name, "period-count") == 0 ||
        strcmp(name, "resets") == 0 || strcmp(name, "reset-count") == 0 ||
        strcmp(name, "overruns") == 0 || strcmp(name, "overrun-count") == 0 ||
        strcmp(name, "underflows") == 0 || strcmp(name, "underflow-count") == 0)
        RETURN_NUMBER(sink->endpoint_ready ? dep_endpoint_stat(&sink->endpoint, name) : 0);
#undef RETURN_TEXT
#undef RETURN_NUMBER
    return ZST_ERROR;
}

static zst_element_ops_t sink_ops = {
    .name = "dantedepaudiosink",
    .open = sink_open,
    .close = sink_close,
    .start = sink_start,
    .stop = sink_stop,
    .process = sink_process,
    .get_caps = sink_caps,
    .set_property = sink_set_property,
    .get_property = sink_get_property
};

zst_element_t* zst_dante_dep_audio_sink_create(void)
{
    dep_sink_t* sink = calloc(1, sizeof(*sink));
    zst_element_t* element;
    zst_pad_t* pad;
    if (!sink) return NULL;
    strcpy(sink->shm_name, "DanteEP");
    strcpy(sink->channels, "0,1");
    sink->channel_count = 2;
    sink->queue_periods = 8;
    sink->tx_lead_us = 2000;
    sink->reconnect_ms = 100;
    element = zst_element_create(&sink_ops, sink);
    if (!element) {
        free(sink);
        return NULL;
    }
    pad = zst_pad_create("sink", ZST_PAD_SINK);
    if (!pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        zst_element_destroy(element);
        return NULL;
    }
    return element;
}
