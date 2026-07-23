/*=============================================================================
    sdp_muxer.c — SDP generator/muxer

    Generates an SDP description for RTP sessions from configured properties and
    optionally observed encoded packet headers.  It is intentionally lightweight:
    it does not RTP-packetize media; use RTSP/RTMP/SRT/TS elements for transport.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_sdp_muxer.h"
#include "zst_media_utils.h"

#define SDP_MUXER_TEXT_MAX       4096
#define SDP_MUXER_EXTRA_MAX      512
#define SDP_MUXER_DEFAULT_ADDR   "127.0.0.1"
#define SDP_MUXER_DEFAULT_NAME   "zstreamer"
#define SDP_MUXER_DEFAULT_VIDEO_PORT 5004
#define SDP_MUXER_DEFAULT_AUDIO_PORT 5006
#define SDP_MUXER_DEFAULT_VIDEO_PT   96
#define SDP_MUXER_DEFAULT_AUDIO_PT   97
#define SDP_MUXER_DEFAULT_AUDIO_RATE 48000
#define SDP_MUXER_DEFAULT_AUDIO_CH   2

typedef struct {
    char address[64];
    char session_name[128];
    char sdp_file[256];
    char video_codec[16];
    char audio_codec[16];
    int  video_enabled;
    int  audio_enabled;
    int  video_port;
    int  audio_port;
    int  video_pt;
    int  audio_pt;
    int  audio_sample_rate;
    int  audio_channels;
    int  emit_once;

    char media_mode[32];
    int ptp_version;
    char ptp_address[64];
    int ptp_domain;

    int video_width;
    int video_height;
    char video_format[32];

    uint8_t h264_sps[SDP_MUXER_EXTRA_MAX];
    int     h264_sps_len;
    uint8_t h264_pps[SDP_MUXER_EXTRA_MAX];
    int     h264_pps_len;
    char    h264_sprop[1024];
    char    h264_profile_level_id[32];
    uint8_t h265_vps[SDP_MUXER_EXTRA_MAX];
    int     h265_vps_len;
    uint8_t h265_sps[SDP_MUXER_EXTRA_MAX];
    int     h265_sps_len;
    uint8_t h265_pps[SDP_MUXER_EXTRA_MAX];
    int     h265_pps_len;
    char    h265_sprop_vps[768];
    char    h265_sprop_sps[768];
    char    h265_sprop_pps[768];
    uint8_t aac_config[8];
    int     aac_config_len;

    char sdp_text[SDP_MUXER_TEXT_MAX];
    int  sdp_valid;
    int  emitted;

    zst_pad_t* video_pad;
    zst_pad_t* audio_pad;
    zst_pad_t* src_pad;
} sdp_muxer_t;

static int sdp_muxer_is_h265(const sdp_muxer_t* s) {
    return s && (strcasecmp(s->video_codec, "h265") == 0 ||
                 strcasecmp(s->video_codec, "hevc") == 0 ||
                 strcasecmp(s->video_codec, "hvc1") == 0);
}

static int sdp_muxer_is_h264(const sdp_muxer_t* s) {
    return s && (strcasecmp(s->video_codec, "h264") == 0 ||
                 strcasecmp(s->video_codec, "avc") == 0);
}

static int sdp_muxer_is_video_codec(const sdp_muxer_t* s, const char* codec) {
    return s && codec && strcasecmp(s->video_codec, codec) == 0;
}

static int sdp_muxer_is_audio_codec(const sdp_muxer_t* s, const char* codec) {
    return s && codec && strcasecmp(s->audio_codec, codec) == 0;
}

static void sdp_muxer_copy_limited(uint8_t* dst, int* dst_len, const uint8_t* src, int len) {
    if (!dst || !dst_len || !src || len <= 0) return;
    if (len > SDP_MUXER_EXTRA_MAX) len = SDP_MUXER_EXTRA_MAX;
    memcpy(dst, src, (size_t)len);
    *dst_len = len;
}

static void sdp_muxer_parse_h264_annexb(sdp_muxer_t* s, const uint8_t* data, int size) {
    int code_size = 0;
    int pos = zst_find_start_code(data, size, 0, &code_size);
    while (pos >= 0 && pos < size) {
        int nal_start = pos + code_size;
        int next_code = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        if (nal_len > 0) {
            int nal_type = data[nal_start] & 0x1f;
            if (nal_type == 7 && s->h264_sps_len == 0) {
                sdp_muxer_copy_limited(s->h264_sps, &s->h264_sps_len, data + nal_start, nal_len);
            } else if (nal_type == 8 && s->h264_pps_len == 0) {
                sdp_muxer_copy_limited(s->h264_pps, &s->h264_pps_len, data + nal_start, nal_len);
            }
        }
        if (next < 0) break;
        code_size = next_code;
        pos = next;
    }
}

static void sdp_muxer_parse_h265_annexb(sdp_muxer_t* s, const uint8_t* data, int size) {
    int code_size = 0;
    int pos = zst_find_start_code(data, size, 0, &code_size);
    while (pos >= 0 && pos < size) {
        int nal_start = pos + code_size;
        int next_code = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_len = nal_end - nal_start;
        if (nal_len >= 2) {
            int nal_type = (data[nal_start] >> 1) & 0x3f;
            if (nal_type == 32 && s->h265_vps_len == 0) {
                sdp_muxer_copy_limited(s->h265_vps, &s->h265_vps_len, data + nal_start, nal_len);
            } else if (nal_type == 33 && s->h265_sps_len == 0) {
                sdp_muxer_copy_limited(s->h265_sps, &s->h265_sps_len, data + nal_start, nal_len);
            } else if (nal_type == 34 && s->h265_pps_len == 0) {
                sdp_muxer_copy_limited(s->h265_pps, &s->h265_pps_len, data + nal_start, nal_len);
            }
        }
        if (next < 0) break;
        code_size = next_code;
        pos = next;
    }
}

static const int* sdp_muxer_aac_rates(size_t* count_out) {
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                 24000, 22050, 16000, 12000, 11025, 8000,
                                 7350 };
    if (count_out) *count_out = sizeof(rates) / sizeof(rates[0]);
    return rates;
}

static int sdp_muxer_aac_freq_index(int sample_rate) {
    size_t count = 0;
    const int* rates = sdp_muxer_aac_rates(&count);
    for (int i = 0; i < (int)count; i++) {
        if (rates[i] == sample_rate) return i;
    }
    return 3; /* 48000 */
}

static int sdp_muxer_aac_rate_from_index(int idx) {
    size_t count = 0;
    const int* rates = sdp_muxer_aac_rates(&count);
    return (idx >= 0 && idx < (int)count) ? rates[idx] : SDP_MUXER_DEFAULT_AUDIO_RATE;
}

static void sdp_muxer_make_aac_config(sdp_muxer_t* s) {
    if (!s || s->aac_config_len > 0) return;
    int profile = 2; /* AAC LC */
    int freq_idx = sdp_muxer_aac_freq_index(s->audio_sample_rate);
    int channels = s->audio_channels > 0 ? s->audio_channels : 2;
    s->aac_config[0] = (uint8_t)((profile << 3) | (freq_idx >> 1));
    s->aac_config[1] = (uint8_t)(((freq_idx & 1) << 7) | (channels << 3));
    s->aac_config_len = 2;
}

static void sdp_muxer_parse_aac_config(sdp_muxer_t* s, const uint8_t* data, int size) {
    if (!s || !data || size < 2) return;
    int audio_object_type = (data[0] >> 3) & 0x1f;
    int freq_idx = ((data[0] & 0x07) << 1) | ((data[1] >> 7) & 0x01);
    int channels = (data[1] >> 3) & 0x0f;
    if (audio_object_type <= 0 || freq_idx == 15) return;
    int copy = size > (int)sizeof(s->aac_config) ? (int)sizeof(s->aac_config) : size;
    memcpy(s->aac_config, data, (size_t)copy);
    s->aac_config_len = copy;
    s->audio_sample_rate = sdp_muxer_aac_rate_from_index(freq_idx);
    if (channels > 0) s->audio_channels = channels;
}

static void sdp_muxer_parse_aac_adts(sdp_muxer_t* s, const uint8_t* data, int size) {
    if (!s || !data || size < 7) return;
    if (data[0] != 0xff || (data[1] & 0xf0) != 0xf0) return;
    int profile_minus1 = (data[2] >> 6) & 0x03;
    int freq_idx = (data[2] >> 2) & 0x0f;
    int channels = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
    int profile = profile_minus1 + 1;
    s->aac_config[0] = (uint8_t)((profile << 3) | (freq_idx >> 1));
    s->aac_config[1] = (uint8_t)(((freq_idx & 1) << 7) | (channels << 3));
    s->aac_config_len = 2;
    s->audio_sample_rate = sdp_muxer_aac_rate_from_index(freq_idx);
    if (channels > 0) s->audio_channels = channels;
}

static int sdp_muxer_b64_len(int len) { return ((len + 2) / 3) * 4; }

static int sdp_muxer_base64(const uint8_t* in, int len, char* out, int out_cap) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int needed = sdp_muxer_b64_len(len) + 1;
    if (!in || len < 0 || !out || out_cap < needed) return -1;
    int p = 0;
    for (int i = 0; i < len; i += 3) {
        int rem = len - i;
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)(rem > 1 ? in[i + 1] : 0) << 8) |
                     (uint32_t)(rem > 2 ? in[i + 2] : 0);
        out[p++] = table[(v >> 18) & 0x3f];
        out[p++] = table[(v >> 12) & 0x3f];
        out[p++] = rem > 1 ? table[(v >> 6) & 0x3f] : '=';
        out[p++] = rem > 2 ? table[v & 0x3f] : '=';
    }
    out[p] = '\0';
    return p;
}

static void sdp_muxer_hex(const uint8_t* in, int len, char* out, int out_cap) {
    static const char hex[] = "0123456789ABCDEF";
    int p = 0;
    if (!out || out_cap <= 0) return;
    for (int i = 0; in && i < len && p + 2 < out_cap; i++) {
        out[p++] = hex[(in[i] >> 4) & 0x0f];
        out[p++] = hex[in[i] & 0x0f];
    }
    out[p] = '\0';
}

static int sdp_muxer_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int sdp_muxer_parse_hex(const char* in, uint8_t* out, int out_cap) {
    if (!in || !out || out_cap <= 0) return 0;
    int n = 0;
    while (*in && n < out_cap) {
        while (*in && isspace((unsigned char)*in)) in++;
        int hi = sdp_muxer_hex_value(*in++);
        if (hi < 0 || !*in) break;
        int lo = sdp_muxer_hex_value(*in++);
        if (lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void sdp_muxer_append(char** dst, size_t* rem, const char* fmt, ...) {
    if (!dst || !*dst || !rem || *rem == 0 || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*dst, *rem, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= *rem) {
        *dst += *rem - 1;
        *rem = 1;
    } else {
        *dst += n;
        *rem -= (size_t)n;
    }
}

static zst_result_t sdp_muxer_write_file(sdp_muxer_t* s) {
    if (!s || !s->sdp_file[0]) return ZST_OK;
    FILE* fp = fopen(s->sdp_file, "wb");
    if (!fp) {
        ZST_LOG_ERROR("sdpmuxer", "failed to open SDP file '%s'", s->sdp_file);
        return ZST_ERROR;
    }
    size_t len = strlen(s->sdp_text);
    size_t wrote = fwrite(s->sdp_text, 1, len, fp);
    int close_ret = fclose(fp);
    if (wrote != len || close_ret != 0) {
        ZST_LOG_ERROR("sdpmuxer", "failed to write SDP file '%s'", s->sdp_file);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static void sdp_muxer_copy_string(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void sdp_muxer_apply_caps(sdp_muxer_t* s, zst_pad_t* pad) {
    if (!s || !pad || !pad->caps || !pad->caps->structs) return;
    const zst_caps_t* caps = pad->caps;
    const zst_caps_struct_t* st = caps->structs;
    const char* mt = st->media_type;

    if (pad == s->video_pad) {
        if (strcmp(mt, "video/x-h265") == 0 || strcmp(mt, "video/h265") == 0 || strcmp(mt, "video/hevc") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "h265");
        } else if (strcmp(mt, "video/x-h264") == 0 || strcmp(mt, "video/h264") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "h264");
        } else if (strcmp(mt, "video/x-vp8") == 0 || strcmp(mt, "video/vp8") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "vp8");
        } else if (strcmp(mt, "video/x-vp9") == 0 || strcmp(mt, "video/vp9") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "vp9");
        } else if (strcmp(mt, "video/x-av1") == 0 || strcmp(mt, "video/av1") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "av1");
        } else if (strcmp(mt, "video/x-raw") == 0 || strcmp(mt, "video/raw") == 0) {
            sdp_muxer_copy_string(s->video_codec, sizeof(s->video_codec), "raw");
            int v = 0;
            if (zst_caps_get_int(caps, "width", &v) == ZST_OK) s->video_width = v;
            if (zst_caps_get_int(caps, "height", &v) == ZST_OK) s->video_height = v;
            const char* fmt = NULL;
            if (zst_caps_get_string(caps, "format", &fmt) == ZST_OK) {
                sdp_muxer_copy_string(s->video_format, sizeof(s->video_format), fmt);
            }
        }
        const char* str = NULL;
        if (zst_caps_get_string(caps, "profile-level-id", &str) == ZST_OK) {
            sdp_muxer_copy_string(s->h264_profile_level_id, sizeof(s->h264_profile_level_id), str);
        }
        if (zst_caps_get_string(caps, "sprop-parameter-sets", &str) == ZST_OK) {
            sdp_muxer_copy_string(s->h264_sprop, sizeof(s->h264_sprop), str);
        }
        if (zst_caps_get_string(caps, "sprop-vps", &str) == ZST_OK) {
            sdp_muxer_copy_string(s->h265_sprop_vps, sizeof(s->h265_sprop_vps), str);
        }
        if (zst_caps_get_string(caps, "sprop-sps", &str) == ZST_OK) {
            sdp_muxer_copy_string(s->h265_sprop_sps, sizeof(s->h265_sprop_sps), str);
        }
        if (zst_caps_get_string(caps, "sprop-pps", &str) == ZST_OK) {
            sdp_muxer_copy_string(s->h265_sprop_pps, sizeof(s->h265_sprop_pps), str);
        }
    } else if (pad == s->audio_pad) {
        if (strcmp(mt, "audio/aac") == 0 || strcmp(mt, "audio/x-aac") == 0) {
            sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "aac");
        } else if (strcmp(mt, "audio/opus") == 0 || strcmp(mt, "audio/x-opus") == 0) {
            sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "opus");
        } else if (strcmp(mt, "audio/PCMU") == 0 || strcmp(mt, "audio/x-mulaw") == 0) {
            sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "pcmu");
        } else if (strcmp(mt, "audio/PCMA") == 0 || strcmp(mt, "audio/x-alaw") == 0) {
            sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "pcma");
        } else if (strcmp(mt, "audio/L16") == 0) {
            sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "l16");
        } else if (strcmp(mt, "audio/x-raw") == 0) {
            const char* fmt = NULL;
            if (zst_caps_get_string(caps, "format", &fmt) == ZST_OK) {
                if (strstr(fmt, "24")) sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "l24");
                else if (strstr(fmt, "32")) sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "l32");
                else sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "l16");
            } else {
                sdp_muxer_copy_string(s->audio_codec, sizeof(s->audio_codec), "l16");
            }
        }
        int v = 0;
        if (zst_caps_get_int(caps, "sample-rate", &v) == ZST_OK && v > 0) s->audio_sample_rate = v;
        if (zst_caps_get_int(caps, "channels", &v) == ZST_OK && v > 0) s->audio_channels = v;
        const void* data = NULL;
        size_t size = 0;
        if (zst_caps_get_buffer(caps, "codec-data", &data, &size) != ZST_OK) {
            zst_caps_get_buffer(caps, "codec_data", &data, &size);
        }
        if (data && size >= 2) {
            sdp_muxer_parse_aac_config(s, (const uint8_t*)data, (int)size);
        } else {
            const char* hex = NULL;
            if (zst_caps_get_string(caps, "config", &hex) == ZST_OK ||
                zst_caps_get_string(caps, "codec-data", &hex) == ZST_OK) {
                uint8_t asc[8];
                int n = sdp_muxer_parse_hex(hex, asc, (int)sizeof(asc));
                if (n >= 2) sdp_muxer_parse_aac_config(s, asc, n);
            }
        }
    }
}

static zst_result_t sdp_muxer_generate(sdp_muxer_t* s) {
    if (!s) return ZST_ERROR;
    char* out = s->sdp_text;
    size_t rem = sizeof(s->sdp_text);
    uint64_t sid = (uint64_t)time(NULL);

    sdp_muxer_make_aac_config(s);

    sdp_muxer_append(&out, &rem, "v=0\r\n");
    if (strcmp(s->media_mode, "st2110") == 0) {
        sdp_muxer_append(&out, &rem, "o=%s %llu 1 IN IP4 %s\r\n", s->session_name, (unsigned long long)sid, s->address);
        sdp_muxer_append(&out, &rem, "s=%s\r\n", s->session_name);
        sdp_muxer_append(&out, &rem, "c=IN IP4 %s\r\n", s->address);
        sdp_muxer_append(&out, &rem, "t=0 0\r\n");
        sdp_muxer_append(&out, &rem, "a=tool:zstreamer\r\n");
        sdp_muxer_append(&out, &rem, "a=keywait:70\r\n");
        sdp_muxer_append(&out, &rem, "a=ts-refclk:ptp=IEEE1588-2019:%s:%d\r\n", s->ptp_address, s->ptp_domain);
    } else {
        sdp_muxer_append(&out, &rem, "o=- %llu 1 IN IP4 %s\r\n", (unsigned long long)sid, s->address);
        sdp_muxer_append(&out, &rem, "s=%s\r\n", s->session_name);
        sdp_muxer_append(&out, &rem, "c=IN IP4 %s\r\n", s->address);
        sdp_muxer_append(&out, &rem, "t=0 0\r\n");
        sdp_muxer_append(&out, &rem, "a=tool:zstreamer\r\n");
    }

    if (s->video_enabled) {
        if (sdp_muxer_is_h265(s)) {
            char vps[768] = "";
            char sps[768] = "";
            char pps[768] = "";
            if (s->h265_sprop_vps[0]) sdp_muxer_copy_string(vps, sizeof(vps), s->h265_sprop_vps);
            else if (s->h265_vps_len > 0) sdp_muxer_base64(s->h265_vps, s->h265_vps_len, vps, sizeof(vps));
            if (s->h265_sprop_sps[0]) sdp_muxer_copy_string(sps, sizeof(sps), s->h265_sprop_sps);
            else if (s->h265_sps_len > 0) sdp_muxer_base64(s->h265_sps, s->h265_sps_len, sps, sizeof(sps));
            if (s->h265_sprop_pps[0]) sdp_muxer_copy_string(pps, sizeof(pps), s->h265_sprop_pps);
            else if (s->h265_pps_len > 0) sdp_muxer_base64(s->h265_pps, s->h265_pps_len, pps, sizeof(pps));
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d H265/90000\r\n", s->video_pt);
            if (vps[0] || sps[0] || pps[0]) {
                sdp_muxer_append(&out, &rem, "a=fmtp:%d", s->video_pt);
                if (vps[0]) sdp_muxer_append(&out, &rem, " sprop-vps=%s", vps);
                if (sps[0]) sdp_muxer_append(&out, &rem, "%ssprop-sps=%s", vps[0] ? ";" : " ", sps);
                if (pps[0]) sdp_muxer_append(&out, &rem, "%ssprop-pps=%s", (vps[0] || sps[0]) ? ";" : " ", pps);
                sdp_muxer_append(&out, &rem, "\r\n");
            }
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        } else if (sdp_muxer_is_h264(s)) {
            char sps[768] = "";
            char pps[768] = "";
            if (s->h264_sprop[0]) {
                const char* comma = strchr(s->h264_sprop, ',');
                if (comma) {
                    size_t n = (size_t)(comma - s->h264_sprop);
                    if (n >= sizeof(sps)) n = sizeof(sps) - 1;
                    memcpy(sps, s->h264_sprop, n);
                    sps[n] = '\0';
                    sdp_muxer_copy_string(pps, sizeof(pps), comma + 1);
                }
            }
            if (!sps[0] && s->h264_sps_len > 0) sdp_muxer_base64(s->h264_sps, s->h264_sps_len, sps, sizeof(sps));
            if (!pps[0] && s->h264_pps_len > 0) sdp_muxer_base64(s->h264_pps, s->h264_pps_len, pps, sizeof(pps));
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d H264/90000\r\n", s->video_pt);
            sdp_muxer_append(&out, &rem, "a=fmtp:%d packetization-mode=1", s->video_pt);
            if (s->h264_profile_level_id[0]) {
                sdp_muxer_append(&out, &rem, ";profile-level-id=%s", s->h264_profile_level_id);
            }
            if (sps[0] && pps[0]) {
                sdp_muxer_append(&out, &rem, ";sprop-parameter-sets=%s,%s", sps, pps);
            }
            sdp_muxer_append(&out, &rem, "\r\n");
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        } else if (sdp_muxer_is_video_codec(s, "vp8")) {
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d VP8/90000\r\n", s->video_pt);
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        } else if (sdp_muxer_is_video_codec(s, "vp9")) {
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d VP9/90000\r\n", s->video_pt);
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        } else if (sdp_muxer_is_video_codec(s, "av1")) {
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d AV1/90000\r\n", s->video_pt);
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        } else if (sdp_muxer_is_video_codec(s, "raw")) {
            sdp_muxer_append(&out, &rem, "m=video %d RTP/AVP %d\r\n", s->video_port, s->video_pt);
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d raw/90000\r\n", s->video_pt);
            if (strcmp(s->media_mode, "st2110") == 0) {
                const char* sampling = "YCbCr-4:2:2";
                int depth = 10;
                if (s->video_format[0]) {
                    if (strstr(s->video_format, "444")) sampling = "YCbCr-4:4:4";
                    if (strstr(s->video_format, "RGB")) sampling = "RGB";
                    if (strstr(s->video_format, "8")) depth = 8;
                    else if (strstr(s->video_format, "12")) depth = 12;
                    else depth = 10;
                }
                int w = s->video_width > 0 ? s->video_width : 1920;
                int h = s->video_height > 0 ? s->video_height : 1080;
                sdp_muxer_append(&out, &rem, "a=fmtp:%d sampling=%s;width=%d;height=%d;depth=%d;interlace\r\n", 
                                 s->video_pt, sampling, w, h, depth);
            }
            sdp_muxer_append(&out, &rem, "a=control:trackID=0\r\n");
        }
    }

    if (s->audio_enabled) {
        sdp_muxer_append(&out, &rem, "m=audio %d RTP/AVP %d\r\n", s->audio_port, s->audio_pt);
        if (sdp_muxer_is_audio_codec(s, "opus")) {
            int rate = s->audio_sample_rate > 0 ? s->audio_sample_rate : 48000;
            int ch = s->audio_channels > 0 ? s->audio_channels : 2;
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d OPUS/%d/%d\r\n", s->audio_pt, rate, ch);
        } else if (sdp_muxer_is_audio_codec(s, "pcmu")) {
            int rate = s->audio_sample_rate > 0 ? s->audio_sample_rate : 8000;
            int ch = s->audio_channels > 0 ? s->audio_channels : 1;
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d PCMU/%d/%d\r\n", s->audio_pt, rate, ch);
        } else if (sdp_muxer_is_audio_codec(s, "pcma")) {
            int rate = s->audio_sample_rate > 0 ? s->audio_sample_rate : 8000;
            int ch = s->audio_channels > 0 ? s->audio_channels : 1;
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d PCMA/%d/%d\r\n", s->audio_pt, rate, ch);
        } else if (sdp_muxer_is_audio_codec(s, "l16") || sdp_muxer_is_audio_codec(s, "l24") || sdp_muxer_is_audio_codec(s, "l32") || sdp_muxer_is_audio_codec(s, "pcm")) {
            int rate = s->audio_sample_rate > 0 ? s->audio_sample_rate : 48000;
            int ch = s->audio_channels > 0 ? s->audio_channels : 2;
            const char* c = "L16";
            if (sdp_muxer_is_audio_codec(s, "l24")) c = "L24";
            else if (sdp_muxer_is_audio_codec(s, "l32")) c = "L32";
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d %s/%d/%d\r\n", s->audio_pt, c, rate, ch);
            if (strcmp(s->media_mode, "st2110") == 0) {
                sdp_muxer_append(&out, &rem, "a=mediaclk:direct=0\r\n");
            }
        } else {
            char config[32];
            sdp_muxer_make_aac_config(s);
            sdp_muxer_hex(s->aac_config, s->aac_config_len, config, sizeof(config));
            sdp_muxer_append(&out, &rem, "a=rtpmap:%d MPEG4-GENERIC/%d/%d\r\n",
                             s->audio_pt, s->audio_sample_rate, s->audio_channels);
            sdp_muxer_append(&out, &rem,
                             "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;config=%s;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n",
                             s->audio_pt, config);
        }
        sdp_muxer_append(&out, &rem, "a=control:trackID=1\r\n");
    }

    s->sdp_valid = 1;
    return sdp_muxer_write_file(s);
}

static void sdp_muxer_observe_video(sdp_muxer_t* s, zst_buffer_t* buf) {
    if (!s || !buf || !buf->memory.data || buf->memory.size == 0) return;
    const uint8_t* data = (const uint8_t*)buf->memory.data;
    int size = (int)buf->memory.size;
    if (sdp_muxer_is_h265(s)) {
        sdp_muxer_parse_h265_annexb(s, data, size);
    } else {
        sdp_muxer_parse_h264_annexb(s, data, size);
    }
}

static void sdp_muxer_observe_audio(sdp_muxer_t* s, zst_buffer_t* buf) {
    if (!s || !buf || !buf->memory.data || buf->memory.size == 0) return;
    sdp_muxer_parse_aac_adts(s, (const uint8_t*)buf->memory.data, (int)buf->memory.size);
}

static zst_result_t sdp_muxer_emit(zst_element_t* el) {
    sdp_muxer_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    if (s->emit_once && s->emitted) return ZST_OK;
    if (!s->sdp_valid && sdp_muxer_generate(s) != ZST_OK) return ZST_ERROR;

    if (s->src_pad && s->src_pad->peer) {
        size_t len = strlen(s->sdp_text);
        zst_buffer_t* out = zst_buffer_create(ZST_BUFFER_USER);
        if (!out) return ZST_ERROR;
        char* data = malloc(len + 1);
        if (!data) {
            zst_buffer_unref(out);
            return ZST_ERROR;
        }
        memcpy(data, s->sdp_text, len + 1);
        out->memory.data = data;
        out->memory.size = len;
        out->memory.priv = data;
        out->memory.release = free;
        zst_pad_push(s->src_pad, out);
        zst_buffer_unref(out);
    }
    s->emitted = 1;
    return ZST_OK;
}

static zst_result_t sdp_muxer_sink_push(zst_pad_t* pad, zst_buffer_t* buf) {
    if (!pad || !pad->parent) return ZST_ERROR;
    zst_element_t* el = pad->parent;
    sdp_muxer_t* s = el->priv;
    if (!s || !buf) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->src_pad && s->src_pad->peer) {
            zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_USER);
            if (eos) {
                eos->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(s->src_pad, eos);
                zst_buffer_unref(eos);
            }
        }
        return ZST_OK;
    }

    sdp_muxer_apply_caps(s, pad);

    if (pad == s->video_pad) {
        s->video_enabled = 1;
        sdp_muxer_observe_video(s, buf);
    } else if (pad == s->audio_pad) {
        s->audio_enabled = 1;
        sdp_muxer_observe_audio(s, buf);
    }

    s->sdp_valid = 0;
    return sdp_muxer_emit(el);
}

static zst_caps_t* sdp_muxer_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter) {
    (void)filter;
    sdp_muxer_t* s = el ? el->priv : NULL;
    if (!s || !pad) return NULL;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->video_pad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-vp8", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-vp9", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-av1", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
        return caps;
    }
    if (pad == s->audio_pad) {
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/opus", 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/PCMU", 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/PCMA", 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/L16", 0, 0, ""));
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
        return caps;
    }
    if (pad == s->src_pad) {
        zst_caps_append(caps, zst_caps_struct_create_video("application/sdp", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("text/plain", 0, 0, 0.0, ""));
        return caps;
    }

    zst_caps_destroy(caps);
    return NULL;
}

static zst_result_t sdp_muxer_open(zst_element_t* el) {
    sdp_muxer_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    s->emitted = 0;
    s->sdp_valid = 0;
    return sdp_muxer_generate(s);
}

static zst_result_t sdp_muxer_start(zst_element_t* el) {
    sdp_muxer_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;
    if (!s->sdp_valid && sdp_muxer_generate(s) != ZST_OK) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t sdp_muxer_stop(zst_element_t* el) {
    sdp_muxer_t* s = el ? el->priv : NULL;
    if (s) s->emitted = 0;
    return ZST_OK;
}

static zst_result_t sdp_muxer_close(zst_element_t* el) {
    return sdp_muxer_stop(el);
}

static zst_result_t sdp_muxer_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el;
    (void)in;
    if (out) *out = NULL;
    return ZST_OK;
}

static int sdp_muxer_parse_bool(const char* v) {
    return v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
                 strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0);
}

static zst_result_t sdp_muxer_set_property(zst_element_t* el, const char* name, const char* value) {
    if (!el || !name || !value) return ZST_ERROR;
    sdp_muxer_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "address") == 0 || strcmp(name, "connection-address") == 0) {
        strncpy(s->address, value, sizeof(s->address) - 1);
        s->address[sizeof(s->address) - 1] = '\0';
    } else if (strcmp(name, "session-name") == 0 || strcmp(name, "name") == 0) {
        strncpy(s->session_name, value, sizeof(s->session_name) - 1);
        s->session_name[sizeof(s->session_name) - 1] = '\0';
    } else if (strcmp(name, "sdp-file") == 0 || strcmp(name, "location") == 0) {
        strncpy(s->sdp_file, value, sizeof(s->sdp_file) - 1);
        s->sdp_file[sizeof(s->sdp_file) - 1] = '\0';
    } else if (strcmp(name, "video-codec") == 0 || strcmp(name, "codec") == 0) {
        if (strcasecmp(value, "h264") != 0 && strcasecmp(value, "avc") != 0 &&
            strcasecmp(value, "h265") != 0 && strcasecmp(value, "hevc") != 0 &&
            strcasecmp(value, "hvc1") != 0 && strcasecmp(value, "vp8") != 0 &&
            strcasecmp(value, "vp9") != 0 && strcasecmp(value, "av1") != 0 &&
            strcasecmp(value, "raw") != 0) return ZST_ERROR;
        strncpy(s->video_codec, value, sizeof(s->video_codec) - 1);
        s->video_codec[sizeof(s->video_codec) - 1] = '\0';
    } else if (strcmp(name, "audio-codec") == 0) {
        if (strcasecmp(value, "aac") != 0 && strcasecmp(value, "mpeg4-generic") != 0 &&
            strcasecmp(value, "opus") != 0 && strcasecmp(value, "pcmu") != 0 &&
            strcasecmp(value, "pcma") != 0 && strcasecmp(value, "l16") != 0 &&
            strcasecmp(value, "l24") != 0 && strcasecmp(value, "l32") != 0 &&
            strcasecmp(value, "pcm") != 0) return ZST_ERROR;
        if (strcasecmp(value, "mpeg4-generic") == 0) value = "aac";
        strncpy(s->audio_codec, value, sizeof(s->audio_codec) - 1);
        s->audio_codec[sizeof(s->audio_codec) - 1] = '\0';
        if ((strcasecmp(s->audio_codec, "pcmu") == 0 || strcasecmp(s->audio_codec, "pcma") == 0) &&
            s->audio_sample_rate == SDP_MUXER_DEFAULT_AUDIO_RATE) {
            s->audio_sample_rate = 8000;
            if (s->audio_channels == SDP_MUXER_DEFAULT_AUDIO_CH) s->audio_channels = 1;
        }
    } else if (strcmp(name, "video-port") == 0) {
        s->video_port = atoi(value);
    } else if (strcmp(name, "audio-port") == 0) {
        s->audio_port = atoi(value);
    } else if (strcmp(name, "video-payload-type") == 0 || strcmp(name, "video-pt") == 0) {
        s->video_pt = atoi(value);
    } else if (strcmp(name, "audio-payload-type") == 0 || strcmp(name, "audio-pt") == 0) {
        s->audio_pt = atoi(value);
    } else if (strcmp(name, "sample-rate") == 0 || strcmp(name, "audio-sample-rate") == 0) {
        s->audio_sample_rate = atoi(value);
        s->aac_config_len = 0;
    } else if (strcmp(name, "channels") == 0 || strcmp(name, "audio-channels") == 0) {
        s->audio_channels = atoi(value);
        s->aac_config_len = 0;
    } else if (strcmp(name, "enable-video") == 0 || strcmp(name, "video-enabled") == 0) {
        s->video_enabled = sdp_muxer_parse_bool(value);
    } else if (strcmp(name, "enable-audio") == 0 || strcmp(name, "audio-enabled") == 0) {
        s->audio_enabled = sdp_muxer_parse_bool(value);
    } else if (strcmp(name, "emit-once") == 0) {
        s->emit_once = sdp_muxer_parse_bool(value);
    } else if (strcmp(name, "media-mode") == 0) {
        sdp_muxer_copy_string(s->media_mode, sizeof(s->media_mode), value);
    } else if (strcmp(name, "ptp-version") == 0) {
        s->ptp_version = atoi(value);
    } else if (strcmp(name, "ptp-address") == 0) {
        sdp_muxer_copy_string(s->ptp_address, sizeof(s->ptp_address), value);
    } else if (strcmp(name, "ptp-domain") == 0) {
        s->ptp_domain = atoi(value);
    } else {
        return ZST_ERROR;
    }

    s->sdp_valid = 0;
    return sdp_muxer_generate(s);
}

static zst_result_t sdp_muxer_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    if (!el || !name || !value_out || max_len == 0) return ZST_ERROR;
    sdp_muxer_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (!s->sdp_valid) sdp_muxer_generate(s);

    if (strcmp(name, "sdp") == 0) {
        strncpy(value_out, s->sdp_text, max_len - 1);
    } else if (strcmp(name, "address") == 0 || strcmp(name, "connection-address") == 0) {
        strncpy(value_out, s->address, max_len - 1);
    } else if (strcmp(name, "session-name") == 0 || strcmp(name, "name") == 0) {
        strncpy(value_out, s->session_name, max_len - 1);
    } else if (strcmp(name, "sdp-file") == 0 || strcmp(name, "location") == 0) {
        strncpy(value_out, s->sdp_file, max_len - 1);
    } else if (strcmp(name, "video-codec") == 0 || strcmp(name, "codec") == 0) {
        strncpy(value_out, s->video_codec, max_len - 1);
    } else if (strcmp(name, "audio-codec") == 0) {
        strncpy(value_out, s->audio_codec, max_len - 1);
    } else if (strcmp(name, "video-port") == 0) {
        snprintf(value_out, max_len, "%d", s->video_port);
    } else if (strcmp(name, "audio-port") == 0) {
        snprintf(value_out, max_len, "%d", s->audio_port);
    } else if (strcmp(name, "video-payload-type") == 0 || strcmp(name, "video-pt") == 0) {
        snprintf(value_out, max_len, "%d", s->video_pt);
    } else if (strcmp(name, "audio-payload-type") == 0 || strcmp(name, "audio-pt") == 0) {
        snprintf(value_out, max_len, "%d", s->audio_pt);
    } else if (strcmp(name, "sample-rate") == 0 || strcmp(name, "audio-sample-rate") == 0) {
        snprintf(value_out, max_len, "%d", s->audio_sample_rate);
    } else if (strcmp(name, "channels") == 0 || strcmp(name, "audio-channels") == 0) {
        snprintf(value_out, max_len, "%d", s->audio_channels);
    } else if (strcmp(name, "enable-video") == 0 || strcmp(name, "video-enabled") == 0) {
        snprintf(value_out, max_len, "%s", s->video_enabled ? "true" : "false");
    } else if (strcmp(name, "enable-audio") == 0 || strcmp(name, "audio-enabled") == 0) {
        snprintf(value_out, max_len, "%s", s->audio_enabled ? "true" : "false");
    } else if (strcmp(name, "emit-once") == 0) {
        snprintf(value_out, max_len, "%s", s->emit_once ? "true" : "false");
    } else if (strcmp(name, "media-mode") == 0) {
        strncpy(value_out, s->media_mode, max_len - 1);
    } else if (strcmp(name, "ptp-version") == 0) {
        snprintf(value_out, max_len, "%d", s->ptp_version);
    } else if (strcmp(name, "ptp-address") == 0) {
        strncpy(value_out, s->ptp_address, max_len - 1);
    } else if (strcmp(name, "ptp-domain") == 0) {
        snprintf(value_out, max_len, "%d", s->ptp_domain);
    } else {
        return ZST_ERROR;
    }
    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name = "sdpmuxer",
    .open = sdp_muxer_open,
    .close = sdp_muxer_close,
    .start = sdp_muxer_start,
    .stop = sdp_muxer_stop,
    .process = sdp_muxer_process,
    .get_caps = sdp_muxer_get_caps,
    .set_property = sdp_muxer_set_property,
    .get_property = sdp_muxer_get_property,
};

zst_element_t* zst_sdp_muxer_create(void) {
    sdp_muxer_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    strncpy(s->address, SDP_MUXER_DEFAULT_ADDR, sizeof(s->address) - 1);
    strncpy(s->session_name, SDP_MUXER_DEFAULT_NAME, sizeof(s->session_name) - 1);
    strncpy(s->video_codec, "h264", sizeof(s->video_codec) - 1);
    strncpy(s->audio_codec, "aac", sizeof(s->audio_codec) - 1);
    s->video_enabled = 1;
    s->audio_enabled = 0;
    s->video_port = SDP_MUXER_DEFAULT_VIDEO_PORT;
    s->audio_port = SDP_MUXER_DEFAULT_AUDIO_PORT;
    s->video_pt = SDP_MUXER_DEFAULT_VIDEO_PT;
    s->audio_pt = SDP_MUXER_DEFAULT_AUDIO_PT;
    s->audio_sample_rate = SDP_MUXER_DEFAULT_AUDIO_RATE;
    s->audio_channels = SDP_MUXER_DEFAULT_AUDIO_CH;
    s->emit_once = 1;

    strncpy(s->media_mode, "standard", sizeof(s->media_mode) - 1);
    s->ptp_version = 1;
    strncpy(s->ptp_address, "127.0.0.1", sizeof(s->ptp_address) - 1);
    s->ptp_domain = 0;

    zst_element_t* el = zst_element_create(&g_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }

    s->video_pad = zst_pad_create("video", ZST_PAD_SINK);
    s->audio_pad = zst_pad_create("audio", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!s->video_pad || !s->audio_pad || !s->src_pad) {
        if (s->video_pad) zst_pad_destroy(s->video_pad);
        if (s->audio_pad) zst_pad_destroy(s->audio_pad);
        if (s->src_pad) zst_pad_destroy(s->src_pad);
        zst_element_destroy(el);
        return NULL;
    }
    s->video_pad->push = sdp_muxer_sink_push;
    s->audio_pad->push = sdp_muxer_sink_push;

    zst_element_add_pad(el, s->video_pad);
    zst_element_add_pad(el, s->audio_pad);
    zst_element_add_pad(el, s->src_pad);

    zst_caps_t* vcaps = zst_caps_create();
    if (vcaps) {
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-vp8", 0, 0, 0.0, ""));
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-vp9", 0, 0, 0.0, ""));
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-av1", 0, 0, 0.0, ""));
        zst_caps_append(vcaps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
        zst_pad_set_template_caps(s->video_pad, vcaps);
        zst_caps_destroy(vcaps);
    }
    zst_caps_t* acaps = zst_caps_create();
    if (acaps) {
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/opus", 0, 0, ""));
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/PCMU", 0, 0, ""));
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/PCMA", 0, 0, ""));
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/L16", 0, 0, ""));
        zst_caps_append(acaps, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
        zst_pad_set_template_caps(s->audio_pad, acaps);
        zst_caps_destroy(acaps);
    }
    zst_caps_t* scaps = zst_caps_create();
    if (scaps) {
        zst_caps_append(scaps, zst_caps_struct_create_video("application/sdp", 0, 0, 0.0, ""));
        zst_pad_set_template_caps(s->src_pad, scaps);
        zst_caps_destroy(scaps);
    }

    sdp_muxer_generate(s);
    ZST_LOG_INFO("sdpmuxer", "created SDP muxer element");
    return el;
}

#ifdef BUILDING_PLUGIN

#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name) {
    if (strcmp(name, "sdpmuxer") == 0 || strcmp(name, "sdpmux") == 0) {
        return zst_sdp_muxer_create();
    }
    return NULL;
}

static const zst_pad_template_t g_sdpmuxer_pads[] = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265;video/x-vp8;video/x-vp9;video/x-av1;video/x-raw" },
    { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/aac;audio/opus;audio/PCMU;audio/PCMA;audio/L16;audio/x-raw" },
    { "src",   ZST_PAD_SRC,  ZST_PAD_ALWAYS, "application/sdp" }
};

static const zst_property_spec_t g_sdpmuxer_properties[] = {
    { "sdp", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "", "Generated SDP text" },
    { "address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, SDP_MUXER_DEFAULT_ADDR, "Connection address for c=/o= lines" },
    { "session-name", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, SDP_MUXER_DEFAULT_NAME, "SDP session name" },
    { "sdp-file", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Optional path to write generated SDP" },
    { "video-codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "Video codec: h264, h265, vp8, vp9, or av1" },
    { "audio-codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "aac", "Audio codec: aac, opus, pcmu, pcma, or l16" },
    { "enable-video", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Include video media section" },
    { "enable-audio", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Include audio media section" },
    { "video-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5004", "Video RTP port" },
    { "audio-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5006", "Audio RTP port" },
    { "video-payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "96", "Video RTP payload type" },
    { "audio-payload-type", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "97", "Audio RTP payload type" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "48000", "AAC sample rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "AAC channel count" },
    { "emit-once", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Emit SDP only once" },
    { "media-mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "standard", "Media mode: standard or st2110" },
    { "ptp-version", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "PTP version (1 for IEEE1588-2019)" },
    { "ptp-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "PTP address for ts-refclk" },
    { "ptp-domain", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "PTP domain number" }
};

static const zst_element_desc_t g_sdpmuxer_elements[] = {
    {
        .name = "sdpmuxer",
        .long_name = "SDP Muxer",
        .category = "Muxer/RTP",
        .description = "Generates SDP descriptions for H.264/H.265/AAC RTP sessions",
        .author = "zstreamer",
        .properties = g_sdpmuxer_properties,
        .nb_properties = sizeof(g_sdpmuxer_properties) / sizeof(g_sdpmuxer_properties[0]),
        .pads = g_sdpmuxer_pads,
        .nb_pads = sizeof(g_sdpmuxer_pads) / sizeof(g_sdpmuxer_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "sdpmuxer_plugin",
        .author = "zstreamer",
        .version = "0.1.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t* zst_get_plugin_elements(uint32_t* nb_elements_out) {
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_sdpmuxer_elements) / sizeof(g_sdpmuxer_elements[0]);
    }
    return g_sdpmuxer_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t* zst_get_plugin(void) {
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}

#endif /* BUILDING_PLUGIN */
