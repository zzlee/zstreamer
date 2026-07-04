/*=============================================================================
    rtp_payloader.c — Generic RTP packetizer/payloader element

    Converts one encoded/raw media access unit on its sink pad into one or more
    RTP packet buffers on its src pad.  Network transport is intentionally kept
    out of this element so the RTP packets can be reused with UDP multicast,
    RTSP interleaving, file capture, tests, etc.
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
#include "zstreamer/elements/zst_rtp_payloader.h"
#include "zst_media_utils.h"
#include "zst_buffer_pool.h"

#define RTP_PAYLOADER_DEFAULT_PT         96
#define RTP_PAYLOADER_DEFAULT_SSRC       0x53545250u /* STRP */
#define RTP_PAYLOADER_DEFAULT_SEQ        0x7000u
#define RTP_PAYLOADER_DEFAULT_MTU        1200       /* RTP payload bytes */
#define RTP_PAYLOADER_DEFAULT_RATE_VIDEO 90000
#define RTP_PAYLOADER_DEFAULT_RATE_AUDIO 48000
#define RTP_PAYLOADER_MAX_PAYLOAD        65507

typedef enum {
    RTP_PAYLOADER_CODEC_H264,
    RTP_PAYLOADER_CODEC_H265,
    RTP_PAYLOADER_CODEC_AAC,
    RTP_PAYLOADER_CODEC_PCM
} rtp_payloader_codec_t;

typedef struct {
    rtp_payloader_codec_t codec;
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t clock_rate;
    int mtu;
    int channels;
    int sample_size;
    uint64_t packets;
    uint64_t bytes;

    zst_pad_t* sink_pad;
    zst_pad_t* src_pad;
    zst_buffer_pool_t* pool;
} rtp_payloader_t;

static const char*
rtp_payloader_codec_to_string(rtp_payloader_codec_t codec)
{
    switch (codec) {
        case RTP_PAYLOADER_CODEC_H264: return "h264";
        case RTP_PAYLOADER_CODEC_H265: return "h265";
        case RTP_PAYLOADER_CODEC_AAC:  return "aac";
        case RTP_PAYLOADER_CODEC_PCM:  return "pcm";
    }
    return "h264";
}

static int
rtp_payloader_parse_codec(const char* value, rtp_payloader_codec_t* codec_out)
{
    if (!value || !codec_out) return 0;
    if (strcasecmp(value, "h264") == 0 || strcasecmp(value, "avc") == 0) {
        *codec_out = RTP_PAYLOADER_CODEC_H264;
    } else if (strcasecmp(value, "h265") == 0 || strcasecmp(value, "hevc") == 0 ||
               strcasecmp(value, "hvc1") == 0) {
        *codec_out = RTP_PAYLOADER_CODEC_H265;
    } else if (strcasecmp(value, "aac") == 0 || strcasecmp(value, "mpeg4-generic") == 0) {
        *codec_out = RTP_PAYLOADER_CODEC_AAC;
    } else if (strcasecmp(value, "pcm") == 0 || strcasecmp(value, "l16") == 0 ||
               strcasecmp(value, "raw-audio") == 0) {
        *codec_out = RTP_PAYLOADER_CODEC_PCM;
    } else {
        return 0;
    }
    return 1;
}

static uint32_t
rtp_payloader_pts_to_rtp_ts(uint64_t pts_ns, uint32_t clock_rate)
{
    return (uint32_t)(pts_ns * (uint64_t)clock_rate / 1000000000ULL);
}

static zst_buffer_t*
rtp_payloader_make_packet(rtp_payloader_t* s, const uint8_t* header, int header_len,
                          const uint8_t* payload, int payload_len,
                          uint32_t rtp_ts, int marker, uint64_t pts_ns)
{
    if (!s || payload_len < 0 || header_len < 0 || (payload_len + header_len) > RTP_PAYLOADER_MAX_PAYLOAD) return NULL;

    size_t packet_len = (size_t)payload_len + (size_t)header_len + 12u;

    zst_buffer_t* out = NULL;
    if (s->pool) {
        zst_buffer_pool_acquire(s->pool, &out, 0, 0);
    }

    uint8_t* pkt = NULL;
    if (!out) {
        out = zst_buffer_create(ZST_BUFFER_USER);
        if (!out) return NULL;

        pkt = malloc(packet_len);
        if (!pkt) {
            zst_buffer_unref(out);
            return NULL;
        }

        out->memory.type = ZST_MEMORY_CPU;
        out->memory.data = pkt;
        out->memory.priv = pkt;
        out->memory.release = free;
    } else {
        pkt = (uint8_t*)out->memory.data;
    }

    out->memory.size = packet_len;

    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((marker ? 0x80 : 0x00) | (s->payload_type & 0x7f));
    pkt[2] = (uint8_t)(s->seq >> 8);
    pkt[3] = (uint8_t)(s->seq & 0xff);
    pkt[4] = (uint8_t)(rtp_ts >> 24);
    pkt[5] = (uint8_t)(rtp_ts >> 16);
    pkt[6] = (uint8_t)(rtp_ts >> 8);
    pkt[7] = (uint8_t)(rtp_ts);
    pkt[8] = (uint8_t)(s->ssrc >> 24);
    pkt[9] = (uint8_t)(s->ssrc >> 16);
    pkt[10] = (uint8_t)(s->ssrc >> 8);
    pkt[11] = (uint8_t)(s->ssrc);
    if (header && header_len > 0) {
        memcpy(pkt + 12, header, (size_t)header_len);
    }
    if (payload && payload_len > 0) {
        memcpy(pkt + 12 + header_len, payload, (size_t)payload_len);
    }

    out->pts = pts_ns;
    out->dts = pts_ns;

    s->seq++;
    s->packets++;
    s->bytes += packet_len;
    return out;
}

static zst_result_t
rtp_payloader_push_packet(rtp_payloader_t* s, const uint8_t* header, int header_len,
                          const uint8_t* payload, int payload_len,
                          uint32_t rtp_ts, int marker, uint64_t pts_ns)
{
    zst_buffer_t* out = rtp_payloader_make_packet(s, header, header_len, payload, payload_len, rtp_ts, marker, pts_ns);
    if (!out) return ZST_ERROR;

    zst_result_t ret = ZST_OK;
    if (s->src_pad && s->src_pad->peer) {
        ret = zst_pad_push(s->src_pad, out);
    }
    zst_buffer_unref(out);
    return ret;
}

static zst_result_t
rtp_payloader_send_h264_nal(rtp_payloader_t* s, const uint8_t* nal, int nal_len,
                            uint32_t ts, int marker, uint64_t pts_ns)
{
    if (!s || !nal || nal_len <= 0) return ZST_ERROR;

    int mtu = s->mtu;
    if (mtu < 3 || mtu > RTP_PAYLOADER_MAX_PAYLOAD) return ZST_ERROR;

    if (nal_len <= mtu) {
        return rtp_payloader_push_packet(s, NULL, 0, nal, nal_len, ts, marker, pts_ns);
    }

    uint8_t fu_ind = (uint8_t)((nal[0] & 0xe0) | 28);
    uint8_t nal_type = nal[0] & 0x1f;
    int off = 1;
    int first = 1;
    zst_result_t ret = ZST_OK;

    while (off < nal_len) {
        int chunk = nal_len - off;
        if (chunk > mtu - 2) chunk = mtu - 2;

        uint8_t fu[2];
        fu[0] = fu_ind;
        fu[1] = nal_type;
        if (first) fu[1] |= 0x80;
        if (off + chunk >= nal_len) fu[1] |= 0x40;

        zst_result_t one = rtp_payloader_push_packet(s, fu, 2, nal + off, chunk, ts,
                                                     marker && (off + chunk >= nal_len), pts_ns);
        if (one != ZST_OK) ret = one;

        off += chunk;
        first = 0;
    }

    return ret;
}

static zst_result_t
rtp_payloader_packetize_h264(rtp_payloader_t* s, const uint8_t* data, int size, uint64_t pts_ns)
{
    if (!s || !data || size <= 0) return ZST_ERROR;

    uint32_t ts = rtp_payloader_pts_to_rtp_ts(pts_ns, 90000);
    int code = 0;
    int pos = zst_find_start_code(data, size, 0, &code);
    if (pos < 0) {
        return rtp_payloader_send_h264_nal(s, data, size, ts, 1, pts_ns);
    }

    zst_result_t ret = ZST_OK;
    while (pos >= 0 && pos < size) {
        int nal_start = pos + code;
        int next_code = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        int is_last = next < 0;

        if (nal_len > 0) {
            zst_result_t one = rtp_payloader_send_h264_nal(s, data + nal_start,
                                                           nal_len, ts, is_last, pts_ns);
            if (one != ZST_OK) ret = one;
        }

        if (next < 0) break;
        pos = next;
        code = next_code;
    }
    return ret;
}

static zst_result_t
rtp_payloader_send_h265_nal(rtp_payloader_t* s, const uint8_t* nal, int nal_len,
                            uint32_t ts, int marker, uint64_t pts_ns)
{
    if (!s || !nal || nal_len < 2) return ZST_ERROR;

    int mtu = s->mtu;
    if (mtu < 4 || mtu > RTP_PAYLOADER_MAX_PAYLOAD) return ZST_ERROR;

    if (nal_len <= mtu) {
        return rtp_payloader_push_packet(s, NULL, 0, nal, nal_len, ts, marker, pts_ns);
    }

    uint8_t nal_type = (uint8_t)((nal[0] >> 1) & 0x3f);
    uint8_t fu_hdr0 = (uint8_t)((nal[0] & 0x81) | (49 << 1)); /* FU payload header */
    uint8_t fu_hdr1 = nal[1];
    int off = 2;
    int first = 1;
    zst_result_t ret = ZST_OK;

    while (off < nal_len) {
        int chunk = nal_len - off;
        if (chunk > mtu - 3) chunk = mtu - 3;

        uint8_t fu[3];
        fu[0] = fu_hdr0;
        fu[1] = fu_hdr1;
        fu[2] = nal_type;
        if (first) fu[2] |= 0x80;
        if (off + chunk >= nal_len) fu[2] |= 0x40;

        zst_result_t one = rtp_payloader_push_packet(s, fu, 3, nal + off, chunk, ts,
                                                     marker && (off + chunk >= nal_len), pts_ns);
        if (one != ZST_OK) ret = one;

        off += chunk;
        first = 0;
    }

    return ret;
}

static zst_result_t
rtp_payloader_packetize_h265(rtp_payloader_t* s, const uint8_t* data, int size, uint64_t pts_ns)
{
    if (!s || !data || size <= 0) return ZST_ERROR;

    uint32_t ts = rtp_payloader_pts_to_rtp_ts(pts_ns, 90000);
    int code = 0;
    int pos = zst_find_start_code(data, size, 0, &code);
    if (pos < 0) {
        return rtp_payloader_send_h265_nal(s, data, size, ts, 1, pts_ns);
    }

    zst_result_t ret = ZST_OK;
    while (pos >= 0 && pos < size) {
        int nal_start = pos + code;
        int next_code = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        int is_last = next < 0;

        if (nal_len > 0) {
            zst_result_t one = rtp_payloader_send_h265_nal(s, data + nal_start,
                                                           nal_len, ts, is_last, pts_ns);
            if (one != ZST_OK) ret = one;
        }

        if (next < 0) break;
        pos = next;
        code = next_code;
    }
    return ret;
}

static zst_result_t
rtp_payloader_packetize_aac(rtp_payloader_t* s, const uint8_t* data, int size, uint64_t pts_ns)
{
    if (!s || !data || size <= 0 || size > 0x1fff) return ZST_ERROR;
    if (size + 4 > s->mtu) return ZST_ERROR;

    uint8_t au[4];
    uint16_t au_header = (uint16_t)(size << 3);
    au[0] = 0;
    au[1] = 16; /* one 16-bit AU-header */
    au[2] = (uint8_t)(au_header >> 8);
    au[3] = (uint8_t)(au_header & 0xff);

    uint32_t ts = rtp_payloader_pts_to_rtp_ts(pts_ns, s->clock_rate);
    zst_result_t ret = rtp_payloader_push_packet(s, au, 4, data, size, ts, 1, pts_ns);
    return ret;
}

static zst_result_t
rtp_payloader_packetize_pcm(rtp_payloader_t* s, const uint8_t* data, int size, uint64_t pts_ns)
{
    if (!s || !data || size <= 0) return ZST_ERROR;

    int bytes_per_sample_frame = s->channels * s->sample_size;
    if (bytes_per_sample_frame <= 0) bytes_per_sample_frame = 1;

    uint32_t base_ts = rtp_payloader_pts_to_rtp_ts(pts_ns, s->clock_rate);
    int off = 0;
    zst_result_t ret = ZST_OK;
    while (off < size) {
        int chunk = size - off;
        if (chunk > s->mtu) chunk = s->mtu;
        uint32_t sample_offset = (uint32_t)(off / bytes_per_sample_frame);
        zst_result_t one = rtp_payloader_push_packet(s, NULL, 0, data + off, chunk,
                                                     base_ts + sample_offset,
                                                     off + chunk >= size, pts_ns);
        if (one != ZST_OK) ret = one;
        off += chunk;
    }
    return ret;
}

static zst_result_t
rtp_payloader_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    rtp_payloader_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    if ((buf->flags & ZST_BUFFER_FLAG_EOS) || !buf->memory.data || buf->memory.size == 0) {
        return ZST_OK;
    }
    if (buf->memory.size > (size_t)INT32_MAX) return ZST_ERROR;

    const uint8_t* data = (const uint8_t*)buf->memory.data;
    int size = (int)buf->memory.size;
    switch (s->codec) {
        case RTP_PAYLOADER_CODEC_H264:
            return rtp_payloader_packetize_h264(s, data, size, buf->pts);
        case RTP_PAYLOADER_CODEC_H265:
            return rtp_payloader_packetize_h265(s, data, size, buf->pts);
        case RTP_PAYLOADER_CODEC_AAC:
            return rtp_payloader_packetize_aac(s, data, size, buf->pts);
        case RTP_PAYLOADER_CODEC_PCM:
            return rtp_payloader_packetize_pcm(s, data, size, buf->pts);
    }
    return ZST_ERROR;
}

static zst_result_t
rtp_payloader_open(zst_element_t* el)
{
    rtp_payloader_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    s->seq = RTP_PAYLOADER_DEFAULT_SEQ;
    s->packets = 0;
    s->bytes = 0;

    if (!s->pool) {
        zst_buffer_pool_config_t cfg = {0};
        cfg.min_buffers = 16;
        cfg.max_buffers = 256;
        cfg.buffer_size = RTP_PAYLOADER_MAX_PAYLOAD + 128;
        cfg.buffer_type = ZST_BUFFER_USER;
        s->pool = zst_buffer_pool_create(NULL, &cfg);
        if (!s->pool) {
            ZST_LOG_WARN("rtppay", "Failed to create buffer pool, will fallback to malloc");
        }
    }

    return ZST_OK;
}

static zst_result_t
rtp_payloader_close(zst_element_t* el)
{
    rtp_payloader_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;

    if (s->pool) { printf("Using pool!\n");
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    return ZST_OK;
}

static zst_caps_t*
rtp_payloader_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad && pad->direction == ZST_PAD_SRC) {
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

    zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
    zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-aac", 0, 0, ""));
    zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
    zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
    return caps;
}

static zst_result_t
rtp_payloader_set_property(zst_element_t* el, const char* name, const char* value)
{
    rtp_payloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "codec") == 0 || strcmp(name, "media") == 0 || strcmp(name, "encoding") == 0) {
        rtp_payloader_codec_t codec;
        if (!rtp_payloader_parse_codec(value, &codec)) return ZST_ERROR;
        s->codec = codec;
        if (codec == RTP_PAYLOADER_CODEC_H264 || codec == RTP_PAYLOADER_CODEC_H265) {
            s->clock_rate = RTP_PAYLOADER_DEFAULT_RATE_VIDEO;
        } else if (s->clock_rate == RTP_PAYLOADER_DEFAULT_RATE_VIDEO) {
            s->clock_rate = RTP_PAYLOADER_DEFAULT_RATE_AUDIO;
        }
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
        s->ssrc = (uint32_t)strtoul(value, NULL, 0);
        return ZST_OK;
    }
    if (strcmp(name, "clock-rate") == 0 || strcmp(name, "sample-rate") == 0 ||
        strcmp(name, "audio-clock-rate") == 0) {
        int rate = atoi(value);
        if (rate <= 0) return ZST_ERROR;
        s->clock_rate = (uint32_t)rate;
        return ZST_OK;
    }
    if (strcmp(name, "mtu") == 0) {
        int mtu = atoi(value);
        if (mtu < 4 || mtu > RTP_PAYLOADER_MAX_PAYLOAD) return ZST_ERROR;
        s->mtu = mtu;
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
    if (strcmp(name, "seq") == 0 || strcmp(name, "sequence") == 0 || strcmp(name, "sequence-number") == 0) {
        s->seq = (uint16_t)(strtoul(value, NULL, 0) & 0xffffu);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
rtp_payloader_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    rtp_payloader_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0 || strcmp(name, "media") == 0 || strcmp(name, "encoding") == 0) {
        snprintf(value_out, max_len, "%s", rtp_payloader_codec_to_string(s->codec));
    } else if (strcmp(name, "payload-type") == 0 || strcmp(name, "pt") == 0 ||
               strcmp(name, "video-payload-type") == 0 || strcmp(name, "audio-payload-type") == 0) {
        snprintf(value_out, max_len, "%u", s->payload_type);
    } else if (strcmp(name, "ssrc") == 0 || strcmp(name, "video-ssrc") == 0 || strcmp(name, "audio-ssrc") == 0) {
        snprintf(value_out, max_len, "%u", s->ssrc);
    } else if (strcmp(name, "clock-rate") == 0 || strcmp(name, "sample-rate") == 0 ||
               strcmp(name, "audio-clock-rate") == 0) {
        snprintf(value_out, max_len, "%u", s->clock_rate);
    } else if (strcmp(name, "mtu") == 0) {
        snprintf(value_out, max_len, "%d", s->mtu);
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->channels);
    } else if (strcmp(name, "sample-size") == 0 || strcmp(name, "bytes-per-sample") == 0) {
        snprintf(value_out, max_len, "%d", s->sample_size);
    } else if (strcmp(name, "seq") == 0 || strcmp(name, "sequence") == 0 || strcmp(name, "sequence-number") == 0) {
        snprintf(value_out, max_len, "%u", s->seq);
    } else if (strcmp(name, "packets") == 0 || strcmp(name, "total-packets") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->packets);
    } else if (strcmp(name, "bytes") == 0 || strcmp(name, "total-bytes") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->bytes);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name = "rtppay",
    .open = rtp_payloader_open,
    .close = rtp_payloader_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = rtp_payloader_get_caps,
    .set_property = rtp_payloader_set_property,
    .get_property = rtp_payloader_get_property,
};

zst_element_t*
zst_rtp_payloader_create(void)
{
    rtp_payloader_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->codec = RTP_PAYLOADER_CODEC_H264;
    s->payload_type = RTP_PAYLOADER_DEFAULT_PT;
    s->ssrc = RTP_PAYLOADER_DEFAULT_SSRC;
    s->seq = RTP_PAYLOADER_DEFAULT_SEQ;
    s->clock_rate = RTP_PAYLOADER_DEFAULT_RATE_VIDEO;
    s->mtu = RTP_PAYLOADER_DEFAULT_MTU;
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

    s->sink_pad->push = rtp_payloader_pad_push;
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_pad);

    zst_caps_t* sink_caps = zst_caps_create();
    if (sink_caps) {
        zst_caps_append(sink_caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(sink_caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        zst_caps_append(sink_caps, zst_caps_struct_create_audio("audio/x-aac", 0, 0, ""));
        zst_caps_append(sink_caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        zst_caps_append(sink_caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
        zst_pad_set_template_caps(s->sink_pad, sink_caps);
        zst_caps_destroy(sink_caps);
    }

    zst_caps_t* src_caps = zst_caps_create();
    if (src_caps) {
        zst_caps_struct_t* c = calloc(1, sizeof(*c));
        if (c) {
            strncpy(c->media_type, "application/x-rtp", sizeof(c->media_type) - 1);
            c->type = ZST_CAPS_ANY;
            zst_caps_append(src_caps, c);
        }
        zst_pad_set_template_caps(s->src_pad, src_caps);
        zst_caps_destroy(src_caps);
    }

    ZST_LOG_INFO("rtppay", "created generic RTP payloader element");
    return el;
}

#ifdef BUILDING_PLUGIN

#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtppay") == 0 || strcmp(name, "rtp_payloader") == 0) {
        return zst_rtp_payloader_create();
    }
    return NULL;
}

static const zst_pad_template_t g_rtppay_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;audio/x-aac;audio/aac;audio/x-raw" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS, "application/x-rtp" }
};

static const zst_property_spec_t g_rtppay_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "RTP payload codec: h264, h265, aac, pcm" },
    { "payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "RTP payload type" },
    { "ssrc", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1398030928", "RTP SSRC" },
    { "clock-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "RTP clock rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "90000", "Alias for clock-rate" },
    { "mtu", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1200", "Maximum RTP payload bytes" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM channel count" },
    { "sample-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "PCM bytes per sample" },
    { "seq", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "28672", "Next RTP sequence number" },
    { "packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP packets produced" },
    { "bytes", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "RTP bytes produced" }
};

static const zst_element_desc_t g_rtppay_elements[] = {
    {
        .name = "rtppay",
        .long_name = "RTP Payloader",
        .category = "RTP",
        .description = "Packetizes H.264/H.265/AAC/PCM buffers into RTP packet buffers",
        .author = "zstreamer",
        .properties = g_rtppay_properties,
        .nb_properties = sizeof(g_rtppay_properties) / sizeof(g_rtppay_properties[0]),
        .pads = g_rtppay_pads,
        .nb_pads = sizeof(g_rtppay_pads) / sizeof(g_rtppay_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtppay_plugin",
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
        *nb_elements_out = sizeof(g_rtppay_elements) / sizeof(g_rtppay_elements[0]);
    }
    return g_rtppay_elements;
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
