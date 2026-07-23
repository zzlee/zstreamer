#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_st2110_30.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"

typedef struct {
    int expected_channels;
    int expected_sample_rate;

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
} st2110_30_depayloader_t;

static void depayloader_free_buffer(zst_buffer_t* b) {
    if (b && b->payload) {
        free(b->payload);
        b->payload = NULL;
    }
}

static zst_result_t
depayloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    st2110_30_depayloader_t* s = el->priv;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    if (in->memory.size < 12) {
        zst_buffer_unref(in);
        return ZST_OK; /* Ignore broken packets */
    }

    uint8_t* in_data = in->memory.data;
    size_t payload_len = in->memory.size - 12;

    if (payload_len == 0) {
        zst_buffer_unref(in);
        return ZST_OK;
    }

    zst_buffer_t* audio = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    if (!audio) {
        zst_buffer_unref(in);
        return ZST_ERROR;
    }
    audio->memory.size = payload_len;
    audio->memory.data = malloc(payload_len);
    audio->memory.release = free;
    audio->memory.priv = audio->memory.data;

    memcpy(audio->memory.data, in_data + 12, payload_len);

    uint32_t ts_net;
    memcpy(&ts_net, in_data + 4, 4);
    uint32_t rtp_ts = ntohl(ts_net);
    
    /* Attempt to reconstruct PTS from RTP TS */
    if (s->expected_sample_rate > 0) {
        audio->pts = (uint64_t)rtp_ts * 1000000000ULL / s->expected_sample_rate;
    } else {
        audio->pts = in->pts;
    }
    audio->dts = audio->pts;

    zst_audio_frame_t* frame = calloc(1, sizeof(*frame));
    frame->sample_rate = s->expected_sample_rate;
    frame->channels = s->expected_channels;
    frame->format = 0;
    
    /* Assume 24-bit (3 bytes per sample) for default length calculation */
    int bytes_per_sample = 3; 
    frame->nb_samples = payload_len / (s->expected_channels * bytes_per_sample);
    if (frame->nb_samples == 0 && payload_len > 0) {
        frame->nb_samples = 1;
    }
    frame->data = audio->memory.data;
    
    audio->payload = frame;
    audio->destroy = depayloader_free_buffer;

    *out = audio;

    zst_buffer_unref(in);
    return ZST_OK;
}

static zst_result_t
depayloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_30_depayloader_t* s = el->priv;
    if (strcmp(name, "expected-channels") == 0) {
        s->expected_channels = atoi(value);
    } else if (strcmp(name, "expected-sample-rate") == 0) {
        s->expected_sample_rate = atoi(value);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
depayloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_30_depayloader_t* s = el->priv;
    if (strcmp(name, "expected-channels") == 0) {
        snprintf(value_out, max_len, "%d", s->expected_channels);
    } else if (strcmp(name, "expected-sample-rate") == 0) {
        snprintf(value_out, max_len, "%d", s->expected_sample_rate);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_caps_t*
depayloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    st2110_30_depayloader_t* s = el->priv;
    zst_caps_t* caps = NULL;
    
    if (pad == s->sinkpad) {
        caps = zst_caps_new_simple("application/x-rtp");
        zst_caps_set_string(caps, "media", "audio");
    } else if (pad == s->srcpad) {
        caps = zst_caps_create();
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->expected_channels, s->expected_sample_rate, "S24LE"));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->expected_channels, s->expected_sample_rate, "S16LE"));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->expected_channels, s->expected_sample_rate, "S32LE"));
    }
    return caps;
}

static zst_element_ops_t depayloader_ops = {
    .name = "st2110_30_depayloader",
    .process = depayloader_process,
    .set_property = depayloader_set_property,
    .get_property = depayloader_get_property,
    .get_caps = depayloader_get_caps,
};

zst_element_t*
zst_st2110_30_depayloader_create(void)
{
    st2110_30_depayloader_t* priv = calloc(1, sizeof(*priv));
    priv->expected_channels = 2;
    priv->expected_sample_rate = 48000;

    zst_element_t* el = zst_element_create(&depayloader_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}
