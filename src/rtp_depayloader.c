/*=============================================================================
    rtp_depayloader.c — Generic RTP depayloader element

    Converts complete RTP packet buffers on its sink pad into codec access-unit
    buffers on its src pad. Transport is intentionally kept out of this element
    so RTP can arrive from UDP, RTSP interleaving, files, tests, etc.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_log.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_rtp_depayloader.h"

#define RTP_DEPAYLOADER_DEFAULT_PT         96
#define RTP_DEPAYLOADER_DEFAULT_RATE_VIDEO 90000
#define RTP_DEPAYLOADER_DEFAULT_RATE_AUDIO 48000
#define RTP_DEPAYLOADER_MAX_PACKET         65535u

typedef enum {
    RTP_DEPAYLOADER_CODEC_H264,
    RTP_DEPAYLOADER_CODEC_H265,
    RTP_DEPAYLOADER_CODEC_AAC,
    RTP_DEPAYLOADER_CODEC_PCM
} rtp_depayloader_codec_t;

typedef struct {
    rtp_depayloader_codec_t codec;
    uint8_t payload_type;
    uint32_t clock_rate;
    int channels;
    int sample_size;
    uint32_t expected_ssrc;
    int filter_ssrc;

    int have_seq;
    uint16_t next_seq;

    uint8_t* au_data;
    size_t au_len;
    size_t au_cap;
    uint32_t au_ts;
    uint64_t au_pts;
    int au_active;

    uint64_t packets;
    uint64_t bytes;
    uint64_t out_buffers;
    uint64_t out_bytes;
    uint64_t dropped_packets;

    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;
} rtp_depayloader_t;

typedef struct {
    const uint8_t* payload;
    size_t payload_len;
    uint8_t payload_type;
    int marker;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_packet_view_t;

static const char*
rtp_depayloader_codec_to_string(rtp_depayloader_codec_t codec)
{
    switch (codec) {
        case RTP_DEPAYLOADER_CODEC_H264: return "h264";
        case RTP_DEPAYLOADER_CODEC_H265: return "h265";
        case RTP_DEPAYLOADER_CODEC_AAC:  return "aac";
        case RTP_DEPAYLOADER_CODEC_PCM:  return "pcm";
    }
    return "h264";
}

static int
rtp_depayloader_parse_codec(const char* value, rtp_depayloader_codec_t* codec_out)
{
    if (!value || !codec_out) return 0;
    if (strcasecmp(value, "h264") == 0 || strcasecmp(value, "avc") == 0) {
        *codec_out = RTP_DEPAYLOADER_CODEC_H264;
    } else if (strcasecmp(value, "h265") == 0 || strcasecmp(value, "hevc") == 0 ||
               strcasecmp(value, "hvc1") == 0) {
        *codec_out = RTP_DEPAYLOADER_CODEC_H265;
    } else if (strcasecmp(value, "aac") == 0 || strcasecmp(value, "mpeg4-generic") == 0) {
        *codec_out = RTP_DEPAYLOADER_CODEC_AAC;
    } else if (strcasecmp(value, "pcm") == 0 || strcasecmp(value, "l16") == 0 ||
               strcasecmp(value, "raw-audio") == 0) {
        *codec_out = RTP_DEPAYLOADER_CODEC_PCM;
    } else {
        return 0;
    }
    return 1;
}

static uint64_t
rtp_depayloader_ts_to_pts(uint32_t ts, uint32_t clock_rate)
{
    if (clock_rate == 0) return 0;
    return (uint64_t)ts * 1000000000ULL / (uint64_t)clock_rate;
}

static void
rtp_depayloader_reset_au(rtp_depayloader_t* s)
{
    if (!s) return;
    free(s->au_data);
    s->au_data = NULL;
    s->au_len = 0;
    s->au_cap = 0;
    s->au_ts = 0;
    s->au_pts = 0;
    s->au_active = 0;
}

static void
rtp_depayloader_begin_au(rtp_depayloader_t* s, uint32_t ts)
{
    if (!s) return;
    if (s->au_active && s->au_ts == ts) return;
    rtp_depayloader_reset_au(s);
    s->au_ts = ts;
    s->au_pts = rtp_depayloader_ts_to_pts(ts, s->clock_rate);
    s->au_active = 1;
}

static int
rtp_depayloader_reserve_au(rtp_depayloader_t* s, size_t extra)
{
    if (!s) return 0;
    if (extra > SIZE_MAX - s->au_len) return 0;
    size_t need = s->au_len + extra;
    if (need <= s->au_cap) return 1;

    size_t cap;
    if (need > SIZE_MAX / 2u) {
        cap = need;
    } else {
        cap = need - 1;
        cap |= cap >> 1;
        cap |= cap >> 2;
        cap |= cap >> 4;
        cap |= cap >> 8;
        cap |= cap >> 16;
#if SIZE_MAX > 0xFFFFFFFF
        cap |= cap >> 32;
#endif
        cap++;
        if (cap < 256u) cap = 256u;
    }

    uint8_t* p = realloc(s->au_data, cap);
    if (!p) return 0;
    s->au_data = p;
    s->au_cap = cap;
    return 1;
}

static int
rtp_depayloader_append_au(rtp_depayloader_t* s, const uint8_t* data, size_t len)
{
    if (!s || (!data && len > 0)) return 0;
    if (len == 0) return 1;
    if (!rtp_depayloader_reserve_au(s, len)) return 0;
    memcpy(s->au_data + s->au_len, data, len);
    s->au_len += len;
    return 1;
}

static int
rtp_depayloader_append_start_code(rtp_depayloader_t* s)
{
    static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
    return rtp_depayloader_append_au(s, start_code, sizeof(start_code));
}

static uint32_t
rtp_depayloader_output_type(const rtp_depayloader_t* s)
{
    if (!s) return ZST_BUFFER_USER;
    switch (s->codec) {
        case RTP_DEPAYLOADER_CODEC_H264:
        case RTP_DEPAYLOADER_CODEC_H265:
            return ZST_BUFFER_VIDEO_PACKET;
        case RTP_DEPAYLOADER_CODEC_AAC:
        case RTP_DEPAYLOADER_CODEC_PCM:
            return ZST_BUFFER_AUDIO_PACKET;
    }
    return ZST_BUFFER_USER;
}

static zst_result_t
rtp_depayloader_push_bytes(rtp_depayloader_t* s, uint8_t* data, size_t len,
                           uint64_t pts, uint64_t duration)
{
    if (!s || !data || len == 0) {
        free(data);
        return ZST_ERROR;
    }

    zst_buffer_t* out = zst_buffer_create(rtp_depayloader_output_type(s));
    if (!out) {
        free(data);
        return ZST_ERROR;
    }

    out->pts = pts;
    out->dts = pts;
    out->duration = duration;
    out->memory.type = ZST_MEMORY_CPU;
    out->memory.data = data;
    out->memory.size = len;
    out->memory.priv = data;
    out->memory.release = free;

    s->out_buffers++;
    s->out_bytes += len;

    zst_result_t ret = ZST_OK;
    if (s->src_pad && s->src_pad->peer) {
        ret = zst_pad_push(s->src_pad, out);
    }
    zst_buffer_unref(out);
    return ret;
}

static zst_result_t
rtp_depayloader_push_au(rtp_depayloader_t* s)
{
    if (!s || !s->au_active || !s->au_data || s->au_len == 0) {
        rtp_depayloader_reset_au(s);
        return ZST_OK;
    }

    uint8_t* data = s->au_data;
    size_t len = s->au_len;
    uint64_t pts = s->au_pts;

    s->au_data = NULL;
    s->au_len = 0;
    s->au_cap = 0;
    s->au_active = 0;

    return rtp_depayloader_push_bytes(s, data, len, pts, 0);
}

static int
rtp_depayloader_parse_rtp(const zst_buffer_t* buf, rtp_packet_view_t* out)
{
    if (!buf || !out || !buf->memory.data || buf->memory.size < 12 ||
        buf->memory.size > RTP_DEPAYLOADER_MAX_PACKET) {
        return 0;
    }

    const uint8_t* p = (const uint8_t*)buf->memory.data;
    size_t size = buf->memory.size;
    if ((p[0] & 0xc0) != 0x80) return 0;

    uint8_t cc = (uint8_t)(p[0] & 0x0f);
    size_t hlen = 12u + (size_t)cc * 4u;
    if (hlen > size) return 0;

    if (p[0] & 0x10) {
        if (hlen + 4u > size) return 0;
        uint16_t ext_words = (uint16_t)(((uint16_t)p[hlen + 2] << 8) | p[hlen + 3]);
        hlen += 4u + (size_t)ext_words * 4u;
        if (hlen > size) return 0;
    }

    out->payload = p + hlen;
    out->payload_len = size - hlen;
    out->payload_type = (uint8_t)(p[1] & 0x7f);
    out->marker = (p[1] & 0x80) ? 1 : 0;
    out->seq = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    out->timestamp = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                     ((uint32_t)p[6] << 8) | p[7];
    out->ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
                ((uint32_t)p[10] << 8) | p[11];
    return 1;
}

static zst_result_t
rtp_depayloader_depay_h264(rtp_depayloader_t* s, const rtp_packet_view_t* rtp)
{
    if (!s || !rtp || !rtp->payload || rtp->payload_len == 0) return ZST_ERROR;

    const uint8_t* p = rtp->payload;
    size_t len = rtp->payload_len;
    uint8_t nal_type = (uint8_t)(p[0] & 0x1f);

    if (nal_type >= 1 && nal_type <= 23) {
        rtp_depayloader_begin_au(s, rtp->timestamp);
        if (!rtp_depayloader_append_start_code(s) ||
            !rtp_depayloader_append_au(s, p, len)) {
            rtp_depayloader_reset_au(s);
            return ZST_ERROR;
        }
        return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
    }

    if (nal_type == 24) { /* STAP-A */
        size_t off = 1;
        rtp_depayloader_begin_au(s, rtp->timestamp);
        while (off + 2u <= len) {
            uint16_t nal_len = (uint16_t)(((uint16_t)p[off] << 8) | p[off + 1]);
            off += 2u;
            if (nal_len == 0 || off + nal_len > len) break;
            if (!rtp_depayloader_append_start_code(s) ||
                !rtp_depayloader_append_au(s, p + off, nal_len)) {
                rtp_depayloader_reset_au(s);
                return ZST_ERROR;
            }
            off += nal_len;
        }
        return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
    }

    if (nal_type == 28) { /* FU-A */
        if (len < 2) return ZST_ERROR;
        uint8_t fu_header = p[1];
        int start = (fu_header & 0x80) ? 1 : 0;
        uint8_t nal_header = (uint8_t)((p[0] & 0xe0) | (fu_header & 0x1f));

        if (start) {
            rtp_depayloader_begin_au(s, rtp->timestamp);
            if (!rtp_depayloader_append_start_code(s) ||
                !rtp_depayloader_append_au(s, &nal_header, 1)) {
                rtp_depayloader_reset_au(s);
                return ZST_ERROR;
            }
        } else if (!s->au_active || s->au_ts != rtp->timestamp) {
            s->dropped_packets++;
            return ZST_OK;
        }

        if (!rtp_depayloader_append_au(s, p + 2, len - 2u)) {
            rtp_depayloader_reset_au(s);
            return ZST_ERROR;
        }
        return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
    }

    /* Unknown H.264 payload type: pass through as a NAL-like payload. */
    rtp_depayloader_begin_au(s, rtp->timestamp);
    if (!rtp_depayloader_append_start_code(s) ||
        !rtp_depayloader_append_au(s, p, len)) {
        rtp_depayloader_reset_au(s);
        return ZST_ERROR;
    }
    return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
}

static zst_result_t
rtp_depayloader_depay_h265(rtp_depayloader_t* s, const rtp_packet_view_t* rtp)
{
    if (!s || !rtp || !rtp->payload || rtp->payload_len < 2) return ZST_ERROR;

    const uint8_t* p = rtp->payload;
    size_t len = rtp->payload_len;
    uint8_t nal_type = (uint8_t)((p[0] >> 1) & 0x3f);

    if (nal_type != 48 && nal_type != 49) {
        rtp_depayloader_begin_au(s, rtp->timestamp);
        if (!rtp_depayloader_append_start_code(s) ||
            !rtp_depayloader_append_au(s, p, len)) {
            rtp_depayloader_reset_au(s);
            return ZST_ERROR;
        }
        return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
    }

    if (nal_type == 48) { /* Aggregation packet */
        size_t off = 2;
        rtp_depayloader_begin_au(s, rtp->timestamp);
        while (off + 2u <= len) {
            uint16_t nal_len = (uint16_t)(((uint16_t)p[off] << 8) | p[off + 1]);
            off += 2u;
            if (nal_len == 0 || off + nal_len > len) break;
            if (!rtp_depayloader_append_start_code(s) ||
                !rtp_depayloader_append_au(s, p + off, nal_len)) {
                rtp_depayloader_reset_au(s);
                return ZST_ERROR;
            }
            off += nal_len;
        }
        return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
    }

    /* Fragmentation unit */
    if (len < 3) return ZST_ERROR;
    uint8_t fu_header = p[2];
    int start = (fu_header & 0x80) ? 1 : 0;
    uint8_t orig_type = (uint8_t)(fu_header & 0x3f);
    uint8_t nal_header[2];
    nal_header[0] = (uint8_t)((p[0] & 0x81) | (orig_type << 1));
    nal_header[1] = p[1];

    if (start) {
        rtp_depayloader_begin_au(s, rtp->timestamp);
        if (!rtp_depayloader_append_start_code(s) ||
            !rtp_depayloader_append_au(s, nal_header, sizeof(nal_header))) {
            rtp_depayloader_reset_au(s);
            return ZST_ERROR;
        }
    } else if (!s->au_active || s->au_ts != rtp->timestamp) {
        s->dropped_packets++;
        return ZST_OK;
    }

    if (!rtp_depayloader_append_au(s, p + 3, len - 3u)) {
        rtp_depayloader_reset_au(s);
        return ZST_ERROR;
    }
    return rtp->marker ? rtp_depayloader_push_au(s) : ZST_OK;
}

static zst_result_t
rtp_depayloader_depay_aac(rtp_depayloader_t* s, const rtp_packet_view_t* rtp)
{
    if (!s || !rtp || !rtp->payload || rtp->payload_len < 4) return ZST_ERROR;

    const uint8_t* p = rtp->payload;
    size_t len = rtp->payload_len;
    uint16_t au_bits = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    size_t au_header_bytes = ((size_t)au_bits + 7u) / 8u;
    if (au_bits == 0 || (au_bits % 16u) != 0 || 2u + au_header_bytes > len) return ZST_ERROR;

    size_t au_count = (size_t)au_bits / 16u;
    size_t data_off = 2u + au_header_bytes;
    size_t cursor = data_off;
    uint64_t base_pts = rtp_depayloader_ts_to_pts(rtp->timestamp, s->clock_rate);
    zst_result_t ret = ZST_OK;

    for (size_t i = 0; i < au_count; i++) {
        size_t h = 2u + i * 2u;
        uint16_t au_hdr = (uint16_t)(((uint16_t)p[h] << 8) | p[h + 1]);
        size_t au_size = (size_t)(au_hdr >> 3);
        if (au_size == 0 || cursor + au_size > len) {
            s->dropped_packets++;
            return ret == ZST_OK ? ZST_ERROR : ret;
        }

        uint8_t* out = malloc(au_size);
        if (!out) return ZST_ERROR;
        memcpy(out, p + cursor, au_size);

        uint64_t pts = base_pts + ((uint64_t)i * 1024ULL * 1000000000ULL) / (uint64_t)s->clock_rate;
        uint64_t duration = (1024ULL * 1000000000ULL) / (uint64_t)s->clock_rate;
        zst_result_t one = rtp_depayloader_push_bytes(s, out, au_size, pts, duration);
        if (one != ZST_OK) ret = one;
        cursor += au_size;
    }

    return ret;
}

static zst_result_t
rtp_depayloader_depay_pcm(rtp_depayloader_t* s, const rtp_packet_view_t* rtp)
{
    if (!s || !rtp || !rtp->payload || rtp->payload_len == 0) return ZST_ERROR;

    rtp_depayloader_begin_au(s, rtp->timestamp);
    if (!rtp_depayloader_append_au(s, rtp->payload, rtp->payload_len)) {
        rtp_depayloader_reset_au(s);
        return ZST_ERROR;
    }

    if (rtp->marker) {
        int bytes_per_frame = s->channels * s->sample_size;
        uint64_t duration = 0;
        if (bytes_per_frame > 0 && s->clock_rate > 0) {
            duration = ((uint64_t)(s->au_len / (size_t)bytes_per_frame) * 1000000000ULL) /
                       (uint64_t)s->clock_rate;
        }
        uint8_t* data = s->au_data;
        size_t len = s->au_len;
        uint64_t pts = s->au_pts;
        s->au_data = NULL;
        s->au_len = 0;
        s->au_cap = 0;
        s->au_active = 0;
        return rtp_depayloader_push_bytes(s, data, len, pts, duration);
    }

    return ZST_OK;
}

static void
rtp_depayloader_pad_destroy(zst_pad_t* pad)
{
    if (!pad || !pad->priv) return;
    rtp_depayloader_reset_au((rtp_depayloader_t*)pad->priv);
}

static zst_result_t
rtp_depayloader_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    rtp_depayloader_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        return rtp_depayloader_push_au(s);
    }

    rtp_packet_view_t rtp;
    if (!rtp_depayloader_parse_rtp(buf, &rtp)) {
        s->dropped_packets++;
        return ZST_ERROR;
    }

    s->packets++;
    s->bytes += buf->memory.size;

    if (rtp.payload_type != s->payload_type) {
        s->dropped_packets++;
        return ZST_OK;
    }
    if (s->filter_ssrc && rtp.ssrc != s->expected_ssrc) {
        s->dropped_packets++;
        return ZST_OK;
    }

    if (s->have_seq && rtp.seq != s->next_seq) {
        s->dropped_packets++;
        rtp_depayloader_reset_au(s);
    }
    s->have_seq = 1;
    s->next_seq = (uint16_t)(rtp.seq + 1u);

    switch (s->codec) {
        case RTP_DEPAYLOADER_CODEC_H264:
            return rtp_depayloader_depay_h264(s, &rtp);
        case RTP_DEPAYLOADER_CODEC_H265:
            return rtp_depayloader_depay_h265(s, &rtp);
        case RTP_DEPAYLOADER_CODEC_AAC:
            return rtp_depayloader_depay_aac(s, &rtp);
        case RTP_DEPAYLOADER_CODEC_PCM:
            return rtp_depayloader_depay_pcm(s, &rtp);
    }
    return ZST_ERROR;
}

static zst_result_t
rtp_depayloader_open(zst_element_t* el)
{
    rtp_depayloader_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    rtp_depayloader_reset_au(s);
    s->have_seq = 0;
    s->next_seq = 0;
    s->packets = 0;
    s->bytes = 0;
    s->out_buffers = 0;
    s->out_bytes = 0;
    s->dropped_packets = 0;
    return ZST_OK;
}

static zst_result_t
rtp_depayloader_close(zst_element_t* el)
{
    rtp_depayloader_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    rtp_depayloader_reset_au(s);
    s->have_seq = 0;
    return ZST_OK;
}

static zst_caps_t*
rtp_depayloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    rtp_depayloader_t* s = el ? el->priv : NULL;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad && pad->direction == ZST_PAD_SINK) {
        zst_caps_struct_t* c = calloc(1, sizeof(*c));
        if (!c) {
            zst_caps_destroy(caps);
            return NULL;
        }
        strncpy(c->media_type, "application/x-rtp", sizeof(c->media_type) - 1);
        c->type = ZST_CAPS_ANY;
        zst_caps_append(caps, c);
        return caps;
    }

    rtp_depayloader_codec_t codec = s ? s->codec : RTP_DEPAYLOADER_CODEC_H264;
    switch (codec) {
        case RTP_DEPAYLOADER_CODEC_H264:
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
            break;
        case RTP_DEPAYLOADER_CODEC_H265:
            zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
            break;
        case RTP_DEPAYLOADER_CODEC_AAC:
            zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-aac", 0, 0, ""));
            zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
            break;
        case RTP_DEPAYLOADER_CODEC_PCM:
            zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
            break;
    }
    return caps;
}

static zst_result_t
rtp_depayloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    rtp_depayloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "codec") == 0 || strcmp(name, "media") == 0 || strcmp(name, "encoding") == 0) {
        rtp_depayloader_codec_t codec;
        if (!rtp_depayloader_parse_codec(value, &codec)) return ZST_ERROR;
        s->codec = codec;
        if (codec == RTP_DEPAYLOADER_CODEC_H264 || codec == RTP_DEPAYLOADER_CODEC_H265) {
            s->clock_rate = RTP_DEPAYLOADER_DEFAULT_RATE_VIDEO;
        } else if (s->clock_rate == RTP_DEPAYLOADER_DEFAULT_RATE_VIDEO) {
            s->clock_rate = RTP_DEPAYLOADER_DEFAULT_RATE_AUDIO;
        }
        rtp_depayloader_reset_au(s);
        return ZST_OK;
    }
    if (strcmp(name, "payload-type") == 0 || strcmp(name, "pt") == 0 ||
        strcmp(name, "video-payload-type") == 0 || strcmp(name, "audio-payload-type") == 0) {
        int pt = atoi(value);
        if (pt < 0 || pt > 127) return ZST_ERROR;
        s->payload_type = (uint8_t)pt;
        return ZST_OK;
    }
    if (strcmp(name, "ssrc") == 0 || strcmp(name, "video-ssrc") == 0 || strcmp(name, "audio-ssrc") == 0) {
        s->expected_ssrc = (uint32_t)strtoul(value, NULL, 0);
        s->filter_ssrc = s->expected_ssrc != 0;
        return ZST_OK;
    }
    if (strcmp(name, "clock-rate") == 0 || strcmp(name, "sample-rate") == 0 ||
        strcmp(name, "audio-clock-rate") == 0) {
        int rate = atoi(value);
        if (rate <= 0) return ZST_ERROR;
        s->clock_rate = (uint32_t)rate;
        return ZST_OK;
    }
    if (strcmp(name, "channels") == 0) {
        int channels = atoi(value);
        if (channels <= 0) return ZST_ERROR;
        s->channels = channels;
        return ZST_OK;
    }
    if (strcmp(name, "sample-size") == 0 || strcmp(name, "bytes-per-sample") == 0) {
        int sample_size = atoi(value);
        if (sample_size <= 0) return ZST_ERROR;
        s->sample_size = sample_size;
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
rtp_depayloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    rtp_depayloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0 || strcmp(name, "media") == 0 || strcmp(name, "encoding") == 0) {
        snprintf(value_out, max_len, "%s", rtp_depayloader_codec_to_string(s->codec));
    } else if (strcmp(name, "payload-type") == 0 || strcmp(name, "pt") == 0 ||
               strcmp(name, "video-payload-type") == 0 || strcmp(name, "audio-payload-type") == 0) {
        snprintf(value_out, max_len, "%u", s->payload_type);
    } else if (strcmp(name, "ssrc") == 0 || strcmp(name, "video-ssrc") == 0 || strcmp(name, "audio-ssrc") == 0) {
        snprintf(value_out, max_len, "%u", s->expected_ssrc);
    } else if (strcmp(name, "clock-rate") == 0 || strcmp(name, "sample-rate") == 0 ||
               strcmp(name, "audio-clock-rate") == 0) {
        snprintf(value_out, max_len, "%u", s->clock_rate);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->channels);
    } else if (strcmp(name, "sample-size") == 0 || strcmp(name, "bytes-per-sample") == 0) {
        snprintf(value_out, max_len, "%d", s->sample_size);
    } else if (strcmp(name, "packets") == 0 || strcmp(name, "total-packets") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->packets);
    } else if (strcmp(name, "bytes") == 0 || strcmp(name, "total-bytes") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->bytes);
    } else if (strcmp(name, "out-buffers") == 0 || strcmp(name, "buffers") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->out_buffers);
    } else if (strcmp(name, "out-bytes") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->out_bytes);
    } else if (strcmp(name, "dropped-packets") == 0 || strcmp(name, "drops") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->dropped_packets);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name = "rtpdepay",
    .open = rtp_depayloader_open,
    .close = rtp_depayloader_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = rtp_depayloader_get_caps,
    .set_property = rtp_depayloader_set_property,
    .get_property = rtp_depayloader_get_property,
};

zst_element_t*
zst_rtp_depayloader_create(void)
{
    rtp_depayloader_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->codec = RTP_DEPAYLOADER_CODEC_H264;
    s->payload_type = RTP_DEPAYLOADER_DEFAULT_PT;
    s->clock_rate = RTP_DEPAYLOADER_DEFAULT_RATE_VIDEO;
    s->channels = 2;
    s->sample_size = 2;

    zst_element_t* el = zst_element_create(&g_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }

    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!s->sink_pad || !s->src_pad) {
        if (s->sink_pad) zst_pad_destroy(s->sink_pad);
        if (s->src_pad) zst_pad_destroy(s->src_pad);
        zst_element_destroy(el);
        return NULL;
    }

    s->sink_pad->push = rtp_depayloader_pad_push;
    s->sink_pad->priv = s;
    s->sink_pad->destroy_priv = rtp_depayloader_pad_destroy;
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    zst_caps_t* sink_caps = zst_caps_create();
    if (sink_caps) {
        zst_caps_struct_t* c = calloc(1, sizeof(*c));
        if (c) {
            strncpy(c->media_type, "application/x-rtp", sizeof(c->media_type) - 1);
            c->type = ZST_CAPS_ANY;
            zst_caps_append(sink_caps, c);
        }
        zst_pad_set_template_caps(s->sink_pad, sink_caps);
        zst_caps_destroy(sink_caps);
    }

    zst_caps_t* src_caps = zst_caps_create();
    if (src_caps) {
        zst_caps_append(src_caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(src_caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        zst_caps_append(src_caps, zst_caps_struct_create_audio("audio/x-aac", 0, 0, ""));
        zst_caps_append(src_caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        zst_caps_append(src_caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
        zst_pad_set_template_caps(s->src_pad, src_caps);
        zst_caps_destroy(src_caps);
    }

    ZST_LOG_INFO("rtpdepay", "created generic RTP depayloader element");
    return el;
}

#ifdef BUILDING_PLUGIN

#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtpdepay") == 0 || strcmp(name, "rtp_depayloader") == 0 ||
        strcmp(name, "rtpdepayload") == 0) {
        return zst_rtp_depayloader_create();
    }
    return NULL;
}

static const zst_pad_template_t g_rtpdepay_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "application/x-rtp" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;audio/x-aac;audio/aac;audio/x-raw" }
};

static const zst_property_spec_t g_rtpdepay_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "RTP payload codec: h264, h265, aac, pcm" },
    { "payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "RTP payload type to accept" },
    { "ssrc", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Expected RTP SSRC; 0 accepts any SSRC" },
    { "clock-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "RTP clock rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "Alias for clock-rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM channel count" },
    { "sample-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM bytes per sample" },
    { "packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP packets consumed" },
    { "bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP bytes consumed" },
    { "out-buffers", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Depayloaded buffers produced" },
    { "out-bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Depayloaded bytes produced" },
    { "dropped-packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Malformed, mismatched, or discontinuous RTP packets" }
};

static const zst_element_desc_t g_rtpdepay_elements[] = {
    {
        .name = "rtpdepay",
        .long_name = "RTP Depayloader",
        .category = "RTP",
        .description = "Depayloads RTP packet buffers into H.264/H.265/AAC/PCM access units",
        .author = "zstreamer",
        .properties = g_rtpdepay_properties,
        .nb_properties = sizeof(g_rtpdepay_properties) / sizeof(g_rtpdepay_properties[0]),
        .pads = g_rtpdepay_pads,
        .nb_pads = sizeof(g_rtpdepay_pads) / sizeof(g_rtpdepay_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtpdepay_plugin",
        .author = "zstreamer",
        .version = "0.1.0",
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
        *nb_elements_out = sizeof(g_rtpdepay_elements) / sizeof(g_rtpdepay_elements[0]);
    }
    return g_rtpdepay_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}

#endif /* BUILDING_PLUGIN */
