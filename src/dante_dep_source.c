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
    uint32_t block_samples;
    uint32_t reconnect_ms;
    uint32_t expected_sample_rate;
    bool endpoint_ready;
    dep_context_t* context;
    dep_endpoint_t endpoint;
} dep_source_t;

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

static zst_result_t source_open(zst_element_t* element)
{
    dep_source_t* source = element->priv;
    if (dep_endpoint_init(&source->endpoint, DEP_ENDPOINT_RX, source->channels,
                          source->queue_periods, source->block_samples, 0,
                          source->reconnect_ms) != ZST_OK) return ZST_ERROR;
    source->endpoint_ready = true;
    source->context = dep_context_acquire(source->shm_name);
    if (!source->context ||
        dep_context_attach(source->context, &source->endpoint) != ZST_OK) {
        if (source->context) dep_context_release(source->context);
        source->context = NULL;
        dep_endpoint_deinit(&source->endpoint);
        source->endpoint_ready = false;
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t source_close(zst_element_t* element)
{
    dep_source_t* source = element->priv;
    dep_context_t* context = source->context;
    if (source->endpoint_ready) {
        dep_endpoint_deinit(&source->endpoint);
        source->endpoint_ready = false;
    }
    source->context = NULL;
    dep_context_release(context);
    return ZST_OK;
}

static zst_result_t source_start(zst_element_t* element)
{
    dep_source_t* source = element->priv;
    return source->endpoint_ready ? dep_endpoint_start(&source->endpoint) : ZST_ERROR;
}

static zst_result_t source_stop(zst_element_t* element)
{
    dep_source_t* source = element->priv;
    if (source->endpoint_ready) dep_endpoint_stop(&source->endpoint);
    return ZST_OK;
}

static void source_buffer_destroy(zst_buffer_t* buffer)
{
    free(buffer->payload);
    buffer->payload = NULL;
}

static zst_result_t source_process(zst_element_t* element, zst_buffer_t* input,
                                   zst_buffer_t** output)
{
    dep_source_t* source = element->priv;
    int32_t* samples;
    uint32_t frames;
    uint32_t rate;
    uint64_t pts;
    zst_buffer_t* buffer;
    zst_audio_frame_t* audio;
    (void)input;
    if (!output || !source->endpoint_ready) return ZST_ERROR;
    *output = NULL;
    zst_result_t result = dep_endpoint_read(&source->endpoint, &samples, &frames,
                                            &pts, &rate);
    if (result != ZST_OK) return result;
    if (!samples || frames == 0 || rate == 0 || source->channel_count == 0)
        return ZST_ERROR;
    if (source->expected_sample_rate && rate != source->expected_sample_rate)
        return ZST_ERROR;
    uint64_t sample_count = (uint64_t)frames * source->channel_count;
    if (sample_count > SIZE_MAX / sizeof(*samples)) return ZST_ERROR;
    size_t payload_size = (size_t)sample_count * sizeof(*samples);

    buffer = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    audio = calloc(1, sizeof(*audio));
    if (!buffer || !audio) {
        free(samples);
        free(audio);
        zst_buffer_unref(buffer);
        return ZST_ERROR;
    }
    buffer->memory.type = ZST_MEMORY_CPU;
    buffer->memory.data = samples;
    buffer->memory.size = payload_size;
    buffer->memory.priv = samples;
    buffer->memory.release = free;
    buffer->payload = audio;
    buffer->destroy = source_buffer_destroy;
    buffer->pts = pts;
    buffer->dts = pts;
    buffer->duration = (uint64_t)frames * UINT64_C(1000000000) / rate;
    audio->sample_rate = rate;
    audio->channels = source->channel_count;
    audio->format = DEP_AUDIO_FORMAT_S32LE;
    audio->nb_samples = frames;
    audio->data = samples;
    *output = buffer;
    return ZST_OK;
}

static zst_caps_t* source_caps(zst_element_t* element, zst_pad_t* pad,
                               const zst_caps_t* filter)
{
    dep_source_t* source = element->priv;
    uint32_t rate = source->endpoint_ready
        ? dep_endpoint_sample_rate(&source->endpoint) : 0;
    zst_caps_t* caps;
    (void)pad;
    (void)filter;
    if (!rate) rate = source->expected_sample_rate;
    caps = zst_caps_create();
    if (!caps || (rate && zst_caps_append(caps, zst_caps_struct_create_audio(
            "audio/x-raw", (int)source->channel_count, (int)rate, "S32LE")
            ) != ZST_OK)) {
        zst_caps_destroy(caps);
        return NULL;
    }
    return caps;
}

static zst_result_t source_set_property(zst_element_t* element, const char* name,
                                        const char* value)
{
    dep_source_t* source = element->priv;
    uint32_t number;
    if (!name || !value || atomic_load_explicit(&element->state, memory_order_acquire) != ZST_STATE_NULL)
        return ZST_ERROR;
    if (strcmp(name, "shm-name") == 0) {
        if (!*value || strlen(value) >= sizeof(source->shm_name)) return ZST_ERROR;
        strcpy(source->shm_name, value);
        return ZST_OK;
    }
    if (strcmp(name, "channels") == 0) {
        if (strlen(value) >= sizeof(source->channels) ||
            !channel_list_count(value, &number)) return ZST_ERROR;
        strcpy(source->channels, value);
        source->channel_count = number;
        return ZST_OK;
    }
    if (strcmp(name, "queue-periods") == 0) {
        if (!parse_uint(value, 1, 4096, &number)) return ZST_ERROR;
        source->queue_periods = number;
        return ZST_OK;
    }
    if (strcmp(name, "block-samples") == 0) {
        if (!parse_uint(value, 0, UINT32_MAX, &number)) return ZST_ERROR;
        source->block_samples = number;
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-interval-ms") == 0) {
        if (!parse_uint(value, 1, 60000, &number)) return ZST_ERROR;
        source->reconnect_ms = number;
        return ZST_OK;
    }
    if (strcmp(name, "expected-sample-rate") == 0) {
        if (!parse_uint(value, 1, UINT32_MAX, &number)) return ZST_ERROR;
        source->expected_sample_rate = number;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t source_get_property(zst_element_t* element, const char* name,
                                        char* output, size_t size)
{
    dep_source_t* source = element->priv;
    if (!name || !output || size == 0) return ZST_ERROR;
#define RETURN_TEXT(value) do { snprintf(output, size, "%s", (value)); return ZST_OK; } while (0)
#define RETURN_NUMBER(value) do { snprintf(output, size, "%llu", (unsigned long long)(value)); return ZST_OK; } while (0)
    if (strcmp(name, "shm-name") == 0) RETURN_TEXT(source->shm_name);
    if (strcmp(name, "channels") == 0) RETURN_TEXT(source->channels);
    if (strcmp(name, "queue-periods") == 0) RETURN_NUMBER(source->queue_periods);
    if (strcmp(name, "block-samples") == 0) RETURN_NUMBER(source->block_samples);
    if (strcmp(name, "reconnect-interval-ms") == 0) RETURN_NUMBER(source->reconnect_ms);
    if (strcmp(name, "expected-sample-rate") == 0) RETURN_NUMBER(source->expected_sample_rate);
    if (strcmp(name, "sample-rate") == 0)
        RETURN_NUMBER(source->endpoint_ready ? dep_endpoint_sample_rate(&source->endpoint) : 0);
    if (strcmp(name, "active") == 0)
        RETURN_TEXT(source->endpoint_ready && dep_endpoint_is_active(&source->endpoint) ? "true" : "false");
    if (strcmp(name, "periods") == 0 || strcmp(name, "period-count") == 0 ||
        strcmp(name, "resets") == 0 || strcmp(name, "reset-count") == 0 ||
        strcmp(name, "overruns") == 0 || strcmp(name, "overrun-count") == 0 ||
        strcmp(name, "underflows") == 0 || strcmp(name, "underflow-count") == 0)
        RETURN_NUMBER(source->endpoint_ready ? dep_endpoint_stat(&source->endpoint, name) : 0);
#undef RETURN_TEXT
#undef RETURN_NUMBER
    return ZST_ERROR;
}

static zst_element_ops_t source_ops = {
    .name = "dantedepaudiosrc",
    .open = source_open,
    .close = source_close,
    .start = source_start,
    .stop = source_stop,
    .process = source_process,
    .get_caps = source_caps,
    .set_property = source_set_property,
    .get_property = source_get_property
};

zst_element_t* zst_dante_dep_audio_source_create(void)
{
    dep_source_t* source = calloc(1, sizeof(*source));
    zst_element_t* element;
    zst_pad_t* pad;
    if (!source) return NULL;
    strcpy(source->shm_name, "DanteEP");
    strcpy(source->channels, "0,1");
    source->channel_count = 2;
    source->queue_periods = 8;
    source->reconnect_ms = 100;
    element = zst_element_create(&source_ops, source);
    if (!element) {
        free(source);
        return NULL;
    }
    pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        zst_element_destroy(element);
        return NULL;
    }
    return element;
}
