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
    int channels;
    int sample_rate;
    int bit_depth;
    int rtp_pt;

    uint32_t ssrc;
    uint16_t seq;
    uint32_t ts;

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
} st2110_30_payloader_t;

static zst_result_t
payloader_open(zst_element_t* el)
{
    st2110_30_payloader_t* s = el->priv;
    s->seq = rand() & 0xFFFF;
    s->ssrc = 0x21103000 | (rand() & 0xFFF);
    return ZST_OK;
}

static zst_result_t
payloader_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    st2110_30_payloader_t* s = el->priv;
    *out = NULL;
    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = in;
        return ZST_OK;
    }

    int bytes_per_sample = s->bit_depth / 8;
    if (bytes_per_sample <= 0) bytes_per_sample = 3;
    int frame_size = s->channels * bytes_per_sample;
    if (frame_size <= 0) {
        zst_buffer_unref(in);
        return ZST_ERROR;
    }

    size_t in_size = 0;
    uint8_t* in_data = NULL;
    
    if (in->type == ZST_BUFFER_AUDIO_FRAME && in->payload) {
        zst_audio_frame_t* in_frame = in->payload;
        in_size = in_frame->nb_samples * frame_size;
        in_data = in_frame->data;
    } else {
        in_size = in->memory.size;
        in_data = in->memory.data;
    }

    /* MTU chunking logic (~1400 bytes payload) */
    size_t max_payload = 1400 - (1400 % frame_size);
    if (max_payload == 0) max_payload = frame_size;

    size_t offset = 0;
    while (offset < in_size) {
        size_t chunk = in_size - offset;
        if (chunk > max_payload) chunk = max_payload;

        zst_buffer_t* rtp = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
        if (!rtp) break;
        
        rtp->memory.size = 12 + chunk;
        rtp->memory.data = malloc(rtp->memory.size);
        rtp->memory.release = free;
        rtp->memory.priv = rtp->memory.data;

        uint8_t* out_data = rtp->memory.data;
        out_data[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
        out_data[1] = s->rtp_pt & 0x7F; /* M=0 */
        
        uint16_t seq = htons(s->seq++);
        memcpy(out_data + 2, &seq, 2);
        
        uint64_t sample_offset = offset / frame_size;
        uint32_t current_rtp_ts = (uint32_t)(in->pts * s->sample_rate / 1000000000ULL) + sample_offset;
        uint32_t ts = htonl(current_rtp_ts);
        memcpy(out_data + 4, &ts, 4);
        
        uint32_t ssrc = htonl(s->ssrc);
        memcpy(out_data + 8, &ssrc, 4);

        if (in_data) {
            memcpy(out_data + 12, in_data + offset, chunk);
        } else {
            memset(out_data + 12, 0, chunk);
        }

        rtp->pts = in->pts + (offset / frame_size) * (1000000000ULL / s->sample_rate);
        rtp->dts = rtp->pts;

        if (offset + chunk >= in_size) {
            *out = rtp;
        } else {
            zst_pad_push(s->srcpad, rtp);
        }
        
        offset += chunk;
    }

    zst_buffer_unref(in);
    return ZST_OK;
}

static zst_result_t
payloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_30_payloader_t* s = el->priv;
    if (strcmp(name, "channels") == 0) {
        s->channels = atoi(value);
    } else if (strcmp(name, "sample-rate") == 0) {
        s->sample_rate = atoi(value);
    } else if (strcmp(name, "bit-depth") == 0) {
        s->bit_depth = atoi(value);
    } else if (strcmp(name, "rtp-pt") == 0) {
        s->rtp_pt = atoi(value);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_30_payloader_t* s = el->priv;
    if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->channels);
    } else if (strcmp(name, "sample-rate") == 0) {
        snprintf(value_out, max_len, "%d", s->sample_rate);
    } else if (strcmp(name, "bit-depth") == 0) {
        snprintf(value_out, max_len, "%d", s->bit_depth);
    } else if (strcmp(name, "rtp-pt") == 0) {
        snprintf(value_out, max_len, "%d", s->rtp_pt);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_caps_t*
payloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    st2110_30_payloader_t* s = el->priv;
    zst_caps_t* caps = NULL;
    
    if (pad == s->sinkpad) {
        caps = zst_caps_create();
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->channels, s->sample_rate, "S24LE"));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->channels, s->sample_rate, "S16LE"));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", s->channels, s->sample_rate, "S32LE"));
    } else if (pad == s->srcpad) {
        caps = zst_caps_new_simple("application/x-rtp");
        zst_caps_set_string(caps, "media", "audio");
        zst_caps_set_int(caps, "payload", s->rtp_pt);
        zst_caps_set_int(caps, "clock-rate", s->sample_rate);
    }
    return caps;
}

static zst_element_ops_t payloader_ops = {
    .name = "st2110_30_payloader",
    .open = payloader_open,
    .process = payloader_process,
    .set_property = payloader_set_property,
    .get_property = payloader_get_property,
    .get_caps = payloader_get_caps,
};

zst_element_t*
zst_st2110_30_payloader_create(void)
{
    st2110_30_payloader_t* priv = calloc(1, sizeof(*priv));
    priv->channels = 2;
    priv->sample_rate = 48000;
    priv->bit_depth = 24;
    priv->rtp_pt = 97;

    zst_element_t* el = zst_element_create(&payloader_ops, priv);

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}
