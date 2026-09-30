/*=============================================================================
    h26x_parser.c - Bounded portable AVC/HEVC elementary stream parser
 =============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pad_event.h"
#include "zstreamer/elements/zst_h26x_parser.h"

#define H26X_DEFAULT_MAX_NAL (16u * 1024u * 1024u)
#define H26X_DEFAULT_MAX_BUFFERED (64u * 1024u * 1024u)
#define H264_MAX_SPS 32u
#define H264_MAX_PPS 256u

typedef enum { H26X_CODEC_AUTO, H26X_CODEC_H264, H26X_CODEC_H265 } h26x_codec_t;
typedef enum { H26X_FMT_AUTO, H26X_FMT_ANNEXB, H26X_FMT_LENGTH } h26x_format_t;
typedef enum { H26X_ERROR, H26X_DROP, H26X_WARN_DROP } h26x_policy_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bit;
} h26x_bits_t;

typedef struct {
    uint8_t *nal;
    size_t len;
    uint32_t width, height, coded_width, coded_height, profile, level, bit_depth;
    double framerate;
    uint64_t version;
    int valid;
} h264_sps_t;

typedef struct {
    uint8_t *nal;
    size_t len;
    uint32_t sps_id;
    uint64_t version;
    int valid;
} h264_pps_t;

typedef struct {
    h26x_codec_t codec;
    h26x_format_t input_format;
    uint32_t nal_length_size;
    int aggregate_au;
    h26x_policy_t policy;
    uint32_t max_nal_size, max_au_size, max_buffered_bytes;
    uint8_t *adapter;
    size_t adapter_len, adapter_cap;
    uint64_t adapter_pts, adapter_dts, adapter_duration;
    int have_timestamp;
    uint8_t *ps[3]; /* H.264 SPS/PPS; H.265 VPS/SPS/PPS. */
    size_t ps_len[3];
    h264_sps_t h264_sps[H264_MAX_SPS];
    h264_pps_t h264_pps[H264_MAX_PPS];
    uint32_t h264_active_sps, h264_active_pps;
    uint64_t h264_active_sps_version, h264_active_pps_version;
    int h264_active;
    uint32_t width, height, coded_width, coded_height, profile, level, bit_depth;
    double framerate;
    int format_valid, format_dirty;
    uint64_t generation, parsed_nals, output_buffers, parse_errors, dropped_nals;
    uint8_t *au;
    size_t au_len, au_cap;
    uint64_t au_pts, au_dts, au_duration;
    uint32_t au_flags;
    int au_has_vcl;
    zst_pad_t *sink_pad, *src_pad;
} h26x_parser_t;

static int bits_read(h26x_bits_t *b, unsigned n, uint32_t *out)
{
    uint32_t v = 0;
    if (!b || !out || n > 32 || b->bit > b->len * 8u || n > b->len * 8u - b->bit) return 0;
    while (n--) { v = (v << 1) | ((b->data[b->bit >> 3] >> (7u - (b->bit & 7u))) & 1u); b->bit++; }
    *out = v;
    return 1;
}

static int bits_ue(h26x_bits_t *b, uint32_t *out)
{
    unsigned zeros = 0;
    uint32_t bit, suffix;
    while (zeros < 31) { if (!bits_read(b, 1, &bit)) return 0; if (bit) break; zeros++; }
    if (zeros == 31 || !bits_read(b, zeros, &suffix)) return 0;
    *out = ((1u << zeros) - 1u) + suffix;
    return 1;
}

static uint8_t *rbsp_from_nal(const uint8_t *nal, size_t len, size_t header, size_t *out_len)
{
    uint8_t *out;
    size_t i, n = 0, zeros = 0;
    if (!nal || len <= header) return NULL;
    out = malloc(len - header);
    if (!out) return NULL;
    for (i = header; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3 && i + 1 < len && nal[i + 1] <= 3) { zeros = 0; continue; }
        out[n++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    *out_len = n;
    return out;
}

static double parse_h264_vui_framerate(h26x_bits_t *b)
{
    uint32_t present, value, aspect, overscan, video_signal, colour, chroma_loc, timing;
    uint32_t num_units, time_scale;
    if (!bits_read(b, 1, &present) || !present) return 0.0;
    if (!bits_read(b, 1, &aspect)) return 0.0;
    if (aspect) {
        if (!bits_read(b, 8, &value)) return 0.0;
        if (value == 255 && (!bits_read(b, 16, &value) || !bits_read(b, 16, &value))) return 0.0;
    }
    if (!bits_read(b, 1, &overscan)) return 0.0;
    if (overscan && !bits_read(b, 1, &value)) return 0.0;
    if (!bits_read(b, 1, &video_signal)) return 0.0;
    if (video_signal) {
        if (!bits_read(b, 3, &value) || !bits_read(b, 1, &value) ||
            !bits_read(b, 1, &colour)) return 0.0;
        if (colour && (!bits_read(b, 8, &value) || !bits_read(b, 8, &value) ||
                       !bits_read(b, 8, &value))) return 0.0;
    }
    if (!bits_read(b, 1, &chroma_loc)) return 0.0;
    if (chroma_loc && (!bits_ue(b, &value) || !bits_ue(b, &value))) return 0.0;
    if (!bits_read(b, 1, &timing) || !timing ||
        !bits_read(b, 32, &num_units) || !bits_read(b, 32, &time_scale) ||
        !bits_read(b, 1, &value) || num_units == 0)
        return 0.0;
    return (double)time_scale / (2.0 * (double)num_units);
}

static int parse_h264_sps(h264_sps_t *sps, uint32_t *sps_id, const uint8_t *nal, size_t len)
{
    uint8_t *rbsp; size_t n; h26x_bits_t b; uint32_t profile, level, id, t, chroma = 1;
    uint32_t width_mbs, height_map, frame_mbs, crop = 0, left = 0, right = 0, top = 0, bottom = 0;
    rbsp = rbsp_from_nal(nal, len, 1, &n); if (!rbsp) return 0;
    b.data = rbsp; b.len = n; b.bit = 0;
    if (!bits_read(&b, 8, &profile) || !bits_read(&b, 8, &t) || !bits_read(&b, 8, &level) || !bits_ue(&b, &id)) goto fail;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        if (!bits_ue(&b, &chroma)) goto fail;
        if (chroma == 3 && !bits_read(&b, 1, &t)) goto fail;
        if (!bits_ue(&b, &t) || !bits_ue(&b, &t) || !bits_read(&b, 1, &t) || !bits_read(&b, 1, &t)) goto fail;
        if (t) { /* Scaling matrices are irrelevant to format; reject only malformed short data. */
            unsigned count = chroma == 3 ? 12 : 8;
            for (unsigned i = 0; i < count; i++) { if (!bits_read(&b, 1, &t)) goto fail; if (t) goto fail; }
        }
    }
    if (!bits_ue(&b, &t) || !bits_ue(&b, &t) || !bits_ue(&b, &t) || !bits_ue(&b, &t) || !bits_read(&b, 1, &t) || !bits_ue(&b, &width_mbs) || !bits_ue(&b, &height_map) || !bits_read(&b, 1, &frame_mbs)) goto fail;
    if (!frame_mbs && !bits_read(&b, 1, &t)) goto fail;
    if (!bits_read(&b, 1, &t) || !bits_read(&b, 1, &crop)) goto fail;
    if (crop && (!bits_ue(&b, &left) || !bits_ue(&b, &right) || !bits_ue(&b, &top) || !bits_ue(&b, &bottom))) goto fail;
    sps->coded_width = (width_mbs + 1u) * 16u;
    sps->coded_height = (height_map + 1u) * 16u * (2u - frame_mbs);
    { uint32_t sub_w = chroma == 3 ? 1 : 2, sub_h = chroma == 1 ? 2 : 1;
       uint32_t crop_x = (left + right) * sub_w, crop_y = (top + bottom) * sub_h * (2u - frame_mbs);
       if (crop_x >= sps->coded_width || crop_y >= sps->coded_height) goto fail;
       sps->width = sps->coded_width - crop_x; sps->height = sps->coded_height - crop_y; }
    sps->profile = profile; sps->level = level; sps->bit_depth = 8;
    sps->framerate = parse_h264_vui_framerate(&b);
    *sps_id = id;
    free(rbsp); return 1;
fail: free(rbsp); return 0;
}

static int skip_h265_ptl(h26x_bits_t *b, unsigned max_sub_layers)
{
    uint32_t v, p[7] = {0};
    if (!bits_read(b, 2, &v) || !bits_read(b, 1, &v) || !bits_read(b, 5, &v) || !bits_read(b, 32, &v) || !bits_read(b, 48, &v) || !bits_read(b, 8, &v)) return 0;
    for (unsigned i = 0; i < max_sub_layers; i++) if (!bits_read(b, 1, &p[i]) || !bits_read(b, 1, &p[i + 1])) return 0;
    for (unsigned i = max_sub_layers; i < 8; i++) if (!bits_read(b, 2, &v)) return 0;
    for (unsigned i = 0; i < max_sub_layers; i++) {
        if (p[i]) {
            for (unsigned j = 0; j < 88; j++) if (!bits_read(b, 1, &v)) return 0;
        }
        if (p[i + 1] && !bits_read(b, 8, &v)) return 0;
    }
    return 1;
}

static int parse_h265_sps(h26x_parser_t *s, const uint8_t *nal, size_t len)
{
    uint8_t *rbsp; size_t n; h26x_bits_t b; uint32_t v, sublayers, chroma, width, height, conf, l, r, t, bot, bitdepth;
    rbsp = rbsp_from_nal(nal, len, 2, &n); if (!rbsp) return 0;
    b.data = rbsp; b.len = n; b.bit = 0;
    if (!bits_read(&b, 4, &v) || !bits_read(&b, 3, &sublayers) || !bits_read(&b, 1, &v) || !skip_h265_ptl(&b, sublayers) || !bits_ue(&b, &v) || !bits_ue(&b, &chroma)) goto fail;
    if (chroma == 3 && !bits_read(&b, 1, &v)) goto fail;
    if (!bits_ue(&b, &width) || !bits_ue(&b, &height) || !bits_read(&b, 1, &conf)) goto fail;
    l = r = t = bot = 0;
    if (conf && (!bits_ue(&b, &l) || !bits_ue(&b, &r) || !bits_ue(&b, &t) || !bits_ue(&b, &bot))) goto fail;
    if (!bits_ue(&b, &bitdepth) || !bits_ue(&b, &v)) goto fail;
    { uint32_t sub_w = chroma == 3 ? 1 : 2, sub_h = chroma == 1 ? 2 : 1;
      uint32_t crop_x = (l + r) * sub_w, crop_y = (t + bot) * sub_h;
      if (crop_x >= width || crop_y >= height) goto fail;
      s->coded_width = width; s->coded_height = height; s->width = width - crop_x; s->height = height - crop_y; }
    s->bit_depth = bitdepth + 8u; s->format_valid = 1; s->format_dirty = 1;
    free(rbsp); return 1;
fail: free(rbsp); return 0;
}

static int reserve(uint8_t **data, size_t *cap, size_t have, size_t add, size_t limit)
{
    size_t need, next; uint8_t *p;
    if (add > limit - have) return 0; need = have + add; if (need <= *cap) return 1;
    next = *cap ? *cap : 256; while (next < need && next <= limit / 2) next *= 2; if (next < need) next = need;
    p = realloc(*data, next); if (!p) return 0; *data = p; *cap = next; return 1;
}

static void clear_au(h26x_parser_t *s) { free(s->au); s->au = NULL; s->au_len = s->au_cap = 0; s->au_has_vcl = 0; s->au_flags = 0; }
static void clear_adapter(h26x_parser_t *s) { s->adapter_len = 0; s->have_timestamp = 0; }
static void clear_ps(h26x_parser_t *s)
{
    for (unsigned i = 0; i < 3; i++) { free(s->ps[i]); s->ps[i] = NULL; s->ps_len[i] = 0; }
    for (unsigned i = 0; i < H264_MAX_SPS; i++) { free(s->h264_sps[i].nal); memset(&s->h264_sps[i], 0, sizeof(s->h264_sps[i])); }
    for (unsigned i = 0; i < H264_MAX_PPS; i++) { free(s->h264_pps[i].nal); memset(&s->h264_pps[i], 0, sizeof(s->h264_pps[i])); }
    s->h264_active = 0;
    s->framerate = 0.0;
}
static void parser_pad_destroy(zst_pad_t *pad)
{
    h26x_parser_t *s = pad ? pad->priv : NULL;
    if (!s) return;
    free(s->adapter); s->adapter = NULL; s->adapter_cap = s->adapter_len = 0;
    clear_au(s); clear_ps(s);
}

static zst_result_t push_caps(h26x_parser_t *s, zst_element_t *el)
{
    zst_caps_t *caps, *old; zst_pad_event_t *event; size_t total = 0, off = 0; uint8_t *codec_data;
    if (!s->format_valid || !s->format_dirty) return ZST_OK;
    if (s->codec == H26X_CODEC_H264 && !s->h264_active) return ZST_OK;
    caps = zst_caps_new_simple(s->codec == H26X_CODEC_H264 ? "video/x-h264" : "video/x-h265"); if (!caps) return ZST_ERROR;
    zst_caps_set_string(caps, "stream-format", "byte-stream"); zst_caps_set_string(caps, "alignment", s->aggregate_au ? "au" : "nal");
    zst_caps_set_uint(caps, "width", s->width); zst_caps_set_uint(caps, "height", s->height); zst_caps_set_uint(caps, "coded-width", s->coded_width); zst_caps_set_uint(caps, "coded-height", s->coded_height);
    zst_caps_set_uint(caps, "profile-idc", s->profile); zst_caps_set_uint(caps, "level-idc", s->level); zst_caps_set_uint(caps, "bit-depth-luma", s->bit_depth); zst_caps_set_uint(caps, "bit-depth-chroma", s->bit_depth);
    if (s->codec == H26X_CODEC_H264) {
        h264_sps_t *sps = &s->h264_sps[s->h264_active_sps];
        h264_pps_t *pps = &s->h264_pps[s->h264_active_pps];
        total = sps->len + pps->len + 8;
        if (s->framerate > 0.0)
            zst_caps_set_double(caps, ZST_H26X_PARSER_CAPS_FRAMERATE, s->framerate);
        if (sps->len >= 4 && sps->len <= UINT16_MAX &&
            pps->len > 0 && pps->len <= UINT16_MAX) {
            size_t avcc_size = 11 + sps->len + pps->len;
            uint8_t *avcc = malloc(avcc_size);
            if (!avcc) { zst_caps_destroy(caps); return ZST_ERROR; }
            avcc[0] = 1;
            avcc[1] = sps->nal[1];
            avcc[2] = sps->nal[2];
            avcc[3] = sps->nal[3];
            avcc[4] = 0xff;
            avcc[5] = 0xe1;
            avcc[6] = (uint8_t)(sps->len >> 8);
            avcc[7] = (uint8_t)sps->len;
            memcpy(avcc + 8, sps->nal, sps->len);
            size_t pps_offset = 8 + sps->len;
            avcc[pps_offset] = 1;
            avcc[pps_offset + 1] = (uint8_t)(pps->len >> 8);
            avcc[pps_offset + 2] = (uint8_t)pps->len;
            memcpy(avcc + pps_offset + 3, pps->nal, pps->len);
            zst_result_t set_ret = zst_caps_set_buffer(caps, ZST_H26X_PARSER_CAPS_AVCC,
                                                       avcc, avcc_size);
            free(avcc);
            if (set_ret != ZST_OK) { zst_caps_destroy(caps); return set_ret; }
        }
    }
    else for (unsigned i = 0; i < 3; i++) total += s->ps_len[i] ? s->ps_len[i] + 4 : 0;
    if (total) { codec_data = malloc(total); if (!codec_data) { zst_caps_destroy(caps); return ZST_ERROR; } if (s->codec == H26X_CODEC_H264) { h264_sps_t *sps = &s->h264_sps[s->h264_active_sps]; h264_pps_t *pps = &s->h264_pps[s->h264_active_pps]; memcpy(codec_data + off, "\0\0\0\1", 4); off += 4; memcpy(codec_data + off, sps->nal, sps->len); off += sps->len; memcpy(codec_data + off, "\0\0\0\1", 4); off += 4; memcpy(codec_data + off, pps->nal, pps->len); } else for (unsigned i = 0; i < 3; i++) if (s->ps_len[i]) { memcpy(codec_data + off, "\0\0\0\1", 4); off += 4; memcpy(codec_data + off, s->ps[i], s->ps_len[i]); off += s->ps_len[i]; } zst_caps_set_buffer(caps, "codec_data", codec_data, total); free(codec_data); }
    s->generation++; zst_caps_set_uint(caps, "format-generation", (uint32_t)s->generation); old = zst_pad_get_caps(s->src_pad);
    event = zst_pad_event_new_caps(caps); if (!event) { zst_caps_destroy(old); zst_caps_destroy(caps); return ZST_ERROR; }
    zst_pad_push_event(s->src_pad, event); zst_pad_event_unref(event);
    if (el->bus) { zst_event_t *bus_event = zst_event_new_caps_changed(el, s->src_pad, old, caps); if (bus_event) zst_bus_post(el->bus, bus_event); }
    zst_caps_destroy(old); zst_caps_destroy(caps); s->format_dirty = 0; return ZST_OK;
}

static zst_result_t emit(h26x_parser_t *s, zst_element_t *el, const uint8_t *nal, size_t len, uint64_t pts, uint64_t dts, uint64_t duration, uint32_t flags)
{
    zst_buffer_t *out; uint8_t *data;
    if (push_caps(s, el) != ZST_OK) return ZST_ERROR;
    data = malloc(len + 4); if (!data) return ZST_ERROR; memcpy(data, "\0\0\0\1", 4); memcpy(data + 4, nal, len);
    out = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET); if (!out) { free(data); return ZST_ERROR; }
    out->pts = pts; out->dts = dts; out->duration = duration; out->flags = flags; out->memory.type = ZST_MEMORY_CPU; out->memory.data = data; out->memory.size = len + 4; out->memory.priv = data; out->memory.release = free;
    s->output_buffers++; if (s->src_pad->peer) { zst_result_t ret = zst_pad_push(s->src_pad, out); zst_buffer_unref(out); return ret; } zst_buffer_unref(out); return ZST_OK;
}

static zst_result_t flush_au(h26x_parser_t *s, zst_element_t *el)
{
    zst_buffer_t *out; uint8_t *data;
    if (!s->au_len) return ZST_OK; if (push_caps(s, el) != ZST_OK) return ZST_ERROR;
    size_t data_len = s->au_len;
    data = s->au; s->au = NULL; s->au_cap = s->au_len = 0; s->au_has_vcl = 0;
    out = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET); if (!out) { free(data); return ZST_ERROR; }
    out->pts = s->au_pts; out->dts = s->au_dts; out->duration = s->au_duration; out->flags = s->au_flags; out->memory.type = ZST_MEMORY_CPU; out->memory.data = data; out->memory.size = data_len; out->memory.priv = data; out->memory.release = free;
    s->output_buffers++; if (s->src_pad->peer) { zst_result_t ret = zst_pad_push(s->src_pad, out); zst_buffer_unref(out); return ret; } zst_buffer_unref(out); return ZST_OK;
}

static int parse_h264_pps(const uint8_t *nal, size_t len, uint32_t *pps_id, uint32_t *sps_id)
{
    uint8_t *r; size_t n; h26x_bits_t b; int ok;
    r = rbsp_from_nal(nal, len, 1, &n); if (!r) return 0;
    b.data = r; b.len = n; b.bit = 0;
    ok = bits_ue(&b, pps_id) && bits_ue(&b, sps_id);
    free(r); return ok && *pps_id < H264_MAX_PPS && *sps_id < H264_MAX_SPS;
}

static int parse_h264_slice(const uint8_t *nal, size_t len, uint32_t *first_mb, uint32_t *pps_id)
{
    uint8_t *r; size_t n; h26x_bits_t b; uint32_t slice_type; int ok;
    r = rbsp_from_nal(nal, len, 1, &n); if (!r) return 0;
    b.data = r; b.len = n; b.bit = 0;
    ok = bits_ue(&b, first_mb) && bits_ue(&b, &slice_type) && bits_ue(&b, pps_id);
    free(r); return ok && *pps_id < H264_MAX_PPS;
}

static int cache_h264_sps(h26x_parser_t *s, const uint8_t *nal, size_t len)
{
    h264_sps_t parsed = {0}, *entry; uint32_t id; uint8_t *copy;
    if (!parse_h264_sps(&parsed, &id, nal, len) || id >= H264_MAX_SPS) return 0;
    entry = &s->h264_sps[id];
    if (entry->valid && entry->len == len && !memcmp(entry->nal, nal, len)) return 1;
    copy = malloc(len); if (!copy) return -1;
    memcpy(copy, nal, len); free(entry->nal); parsed.nal = copy; parsed.len = len;
    parsed.version = entry->version + 1; parsed.valid = 1; *entry = parsed;
    return 1;
}

static int cache_h264_pps(h26x_parser_t *s, const uint8_t *nal, size_t len)
{
    uint32_t pps_id, sps_id; h264_pps_t *entry; uint8_t *copy;
    if (!parse_h264_pps(nal, len, &pps_id, &sps_id)) return 0;
    entry = &s->h264_pps[pps_id];
    if (entry->valid && entry->len == len && !memcmp(entry->nal, nal, len)) return 1;
    copy = malloc(len); if (!copy) return -1;
    memcpy(copy, nal, len); free(entry->nal); entry->nal = copy; entry->len = len;
    entry->sps_id = sps_id; entry->version++; entry->valid = 1;
    return 1;
}

static void activate_h264_config(h26x_parser_t *s, uint32_t pps_id)
{
    h264_pps_t *pps; h264_sps_t *sps;
    if (pps_id >= H264_MAX_PPS || !(pps = &s->h264_pps[pps_id])->valid || !(sps = &s->h264_sps[pps->sps_id])->valid) return;
    if (s->h264_active && s->h264_active_sps == pps->sps_id && s->h264_active_pps == pps_id &&
        s->h264_active_sps_version == sps->version && s->h264_active_pps_version == pps->version) return;
    s->h264_active = 1; s->h264_active_sps = pps->sps_id; s->h264_active_pps = pps_id;
    s->h264_active_sps_version = sps->version; s->h264_active_pps_version = pps->version;
    s->width = sps->width; s->height = sps->height; s->coded_width = sps->coded_width; s->coded_height = sps->coded_height;
    s->profile = sps->profile; s->level = sps->level; s->bit_depth = sps->bit_depth;
    s->framerate = sps->framerate;
    s->format_valid = 1; s->format_dirty = 1;
}

static zst_result_t handle_nal(h26x_parser_t *s, zst_element_t *el, const uint8_t *nal, size_t len, uint32_t input_flags)
{
    uint8_t type; int vcl, first, key = 0, ps = -1, slice_valid = 0; uint32_t pps_id = 0, first_mb;
    if (!len) return ZST_OK; type = s->codec == H26X_CODEC_H264 ? nal[0] & 31u : (nal[0] >> 1) & 63u;
    if (s->codec == H26X_CODEC_H264) { vcl = type >= 1 && type <= 5; key = type == 5; if (type == 7) ps = 0; else if (type == 8) ps = 1; slice_valid = vcl && parse_h264_slice(nal, len, &first_mb, &pps_id); first = slice_valid && first_mb == 0; }
    else { vcl = type <= 31; key = type >= 16 && type <= 21; if (type == 32) ps = 0; else if (type == 33) ps = 1; else if (type == 34) ps = 2; first = vcl && len > 2 && (nal[2] & 0x80); }
    if (ps >= 0) {
        if (s->codec == H26X_CODEC_H264) {
            int valid = type == 7 ? cache_h264_sps(s, nal, len) : cache_h264_pps(s, nal, len);
            if (valid == 0) s->parse_errors++;
            else if (valid < 0) return ZST_ERROR;
        } else {
            uint8_t *copy = malloc(len);
            if (!copy) return ZST_ERROR;
            memcpy(copy, nal, len);
            if (s->ps_len[ps] != len || !s->ps[ps] || memcmp(s->ps[ps], nal, len)) {
                free(s->ps[ps]); s->ps[ps] = copy; s->ps_len[ps] = len; s->format_dirty = 1;
            } else free(copy);
            if (s->codec == H26X_CODEC_H265 && type == 33 && !parse_h265_sps(s, nal, len)) s->parse_errors++;
        }
    }
    uint32_t flags = input_flags & ZST_BUFFER_FLAG_EOS;
    if (key) flags |= ZST_BUFFER_FLAG_KEYFRAME;
    s->parsed_nals++;
    if (vcl && first && s->au_has_vcl) { zst_result_t ret = flush_au(s, el); if (ret != ZST_OK) return ret; }
    if (s->codec == H26X_CODEC_H264 && slice_valid && first) activate_h264_config(s, pps_id);
    if (!s->aggregate_au) return emit(s, el, nal, len, s->adapter_pts, s->adapter_dts, s->adapter_duration, flags);
    if (!s->au_len) { s->au_pts = s->adapter_pts; s->au_dts = s->adapter_dts; s->au_duration = s->adapter_duration; }
    if (!reserve(&s->au, &s->au_cap, s->au_len, len + 4, s->max_au_size)) return ZST_ERROR;
    memcpy(s->au + s->au_len, "\0\0\0\1", 4); s->au_len += 4; memcpy(s->au + s->au_len, nal, len); s->au_len += len; s->au_flags |= flags; s->au_has_vcl |= vcl;
    return ZST_OK;
}

static size_t start_code(const uint8_t *p, size_t n, size_t from, size_t *size)
{ for (size_t i = from; i + 3 <= n; i++) if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) { *size = p[i + 2] == 1 ? 3 : 4; return i; } return SIZE_MAX; }

static zst_result_t parse_adapter(h26x_parser_t *s, zst_element_t *el, int eos)
{
    size_t off = 0;
    if (s->input_format == H26X_FMT_AUTO && s->adapter_len >= 4) { size_t sc; s->input_format = start_code(s->adapter, s->adapter_len, 0, &sc) == 0 ? H26X_FMT_ANNEXB : H26X_FMT_LENGTH; }
    if (s->input_format == H26X_FMT_ANNEXB) {
        size_t sc, next, sc_len, next_len; sc = start_code(s->adapter, s->adapter_len, 0, &sc_len); if (sc == SIZE_MAX) { if (s->adapter_len > s->max_buffered_bytes) return ZST_ERROR; return ZST_OK; } off = sc;
        while ((next = start_code(s->adapter, s->adapter_len, off + sc_len, &next_len)) != SIZE_MAX) { if (next > off + sc_len) { zst_result_t ret = handle_nal(s, el, s->adapter + off + sc_len, next - off - sc_len, 0); if (ret != ZST_OK) return ret; } off = next; sc_len = next_len; }
        if (eos && s->adapter_len > off + sc_len) { zst_result_t ret = handle_nal(s, el, s->adapter + off + sc_len, s->adapter_len - off - sc_len, ZST_BUFFER_FLAG_EOS); if (ret != ZST_OK) return ret; off = s->adapter_len; }
    } else if (s->input_format == H26X_FMT_LENGTH) {
        while (s->adapter_len - off >= s->nal_length_size) { uint32_t n = 0; for (uint32_t i = 0; i < s->nal_length_size; i++) n = (n << 8) | s->adapter[off + i]; if (n == 0 || n > s->max_nal_size) return ZST_ERROR; if (n > s->adapter_len - off - s->nal_length_size) break; { uint32_t flags = eos && off + s->nal_length_size + n == s->adapter_len ? ZST_BUFFER_FLAG_EOS : 0; zst_result_t ret = handle_nal(s, el, s->adapter + off + s->nal_length_size, n, flags); if (ret != ZST_OK) return ret; } off += s->nal_length_size + n; }
        if (eos && off != s->adapter_len) { s->parse_errors++; if (s->policy == H26X_ERROR) return ZST_ERROR; s->dropped_nals++; off = s->adapter_len; }
    }
    if (off) { memmove(s->adapter, s->adapter + off, s->adapter_len - off); s->adapter_len -= off; }
    return ZST_OK;
}

static zst_result_t parser_push(zst_pad_t *pad, zst_buffer_t *buf)
{
    h26x_parser_t *s; zst_element_t *el; zst_result_t ret;
    if (!pad || !pad->parent || !buf) return ZST_ERROR; el = pad->parent; s = el->priv;
    if (buf->memory.type != ZST_MEMORY_CPU || (!buf->memory.data && buf->memory.size)) return ZST_ERROR;
    if (buf->flags & ZST_BUFFER_FLAG_DROP) {
        clear_adapter(s);
        clear_au(s);
        return s->src_pad->peer ? zst_pad_push(s->src_pad, buf) : ZST_OK;
    }
    if (!s->have_timestamp) { s->adapter_pts = buf->pts; s->adapter_dts = buf->dts; s->adapter_duration = buf->duration; s->have_timestamp = 1; }
    if (!reserve(&s->adapter, &s->adapter_cap, s->adapter_len, buf->memory.size, s->max_buffered_bytes)) return ZST_ERROR;
    memcpy(s->adapter + s->adapter_len, buf->memory.data, buf->memory.size); s->adapter_len += buf->memory.size;
    ret = parse_adapter(s, el, (buf->flags & ZST_BUFFER_FLAG_EOS) != 0); if (ret != ZST_OK) { s->parse_errors++; return ret; }
    if (buf->flags & ZST_BUFFER_FLAG_EOS) { s->au_flags |= ZST_BUFFER_FLAG_EOS; return flush_au(s, el); } return ZST_OK;
}

static zst_result_t parser_event(zst_element_t *el, zst_pad_t *sink, zst_pad_event_t *event)
{
    h26x_parser_t *s = el ? el->priv : NULL; if (!s || !event) return ZST_ERROR;
    if (event->type == ZST_PAD_EVENT_CAPS) { const char *media, *format; if (!event->as.caps.caps || !event->as.caps.caps->structs) return ZST_ERROR; media = event->as.caps.caps->structs->media_type; if (!strcmp(media, "video/x-h264")) s->codec = H26X_CODEC_H264; else if (!strcmp(media, "video/x-h265")) s->codec = H26X_CODEC_H265; else return ZST_ERROR; if (zst_caps_get_string(event->as.caps.caps, "stream-format", &format) == ZST_OK) s->input_format = !strcmp(format, "byte-stream") ? H26X_FMT_ANNEXB : H26X_FMT_LENGTH; return ZST_OK; }
    if (event->type == ZST_PAD_EVENT_EOS) { zst_result_t ret = parse_adapter(s, el, 1); if (ret == ZST_OK) { s->au_flags |= ZST_BUFFER_FLAG_EOS; ret = flush_au(s, el); } if (ret != ZST_OK) return ret; }
    if (event->type == ZST_PAD_EVENT_STREAM_START || event->type == ZST_PAD_EVENT_FLUSH_START || event->type == ZST_PAD_EVENT_DISCONT) { clear_adapter(s); clear_au(s); if (event->type == ZST_PAD_EVENT_STREAM_START) { clear_ps(s); s->format_valid = s->format_dirty = 0; s->generation = 0; s->codec = H26X_CODEC_AUTO; s->input_format = H26X_FMT_AUTO; } }
    return zst_pad_push_event(s->src_pad, event);
}

static zst_result_t parser_open(zst_element_t *el) { h26x_parser_t *s = el ? el->priv : NULL; if (!s) return ZST_ERROR; clear_adapter(s); clear_au(s); return ZST_OK; }
static zst_result_t parser_close(zst_element_t *el) { h26x_parser_t *s = el ? el->priv : NULL; if (!s) return ZST_ERROR; free(s->adapter); s->adapter = NULL; s->adapter_cap = 0; clear_au(s); clear_ps(s); return ZST_OK; }

static zst_result_t parser_set_property(zst_element_t *el, const char *name, const char *value)
{
    h26x_parser_t *s = el ? el->priv : NULL; unsigned long n; if (!s || !name || !value) return ZST_ERROR;
    if (strcmp(name, "codec") == 0) { if (!strcasecmp(value, "auto")) s->codec = H26X_CODEC_AUTO; else if (!strcasecmp(value, "h264")) s->codec = H26X_CODEC_H264; else if (!strcasecmp(value, "h265")) s->codec = H26X_CODEC_H265; else return ZST_ERROR; return ZST_OK; }
    if (strcmp(name, "input-stream-format") == 0) { if (!strcasecmp(value, "auto")) s->input_format = H26X_FMT_AUTO; else if (!strcasecmp(value, "byte-stream")) s->input_format = H26X_FMT_ANNEXB; else if (!strcasecmp(value, "avc") || !strcasecmp(value, "hvc1") || !strcasecmp(value, "hev1")) s->input_format = H26X_FMT_LENGTH; else return ZST_ERROR; return ZST_OK; }
    if (strcmp(name, "aggregate-au") == 0) { if (!strcasecmp(value, "true") || !strcmp(value, "1")) s->aggregate_au = 1; else if (!strcasecmp(value, "false") || !strcmp(value, "0")) s->aggregate_au = 0; else return ZST_ERROR; return ZST_OK; }
    if (strcmp(name, "malformed-policy") == 0) { if (!strcmp(value, "error")) s->policy = H26X_ERROR; else if (!strcmp(value, "drop")) s->policy = H26X_DROP; else if (!strcmp(value, "warn-drop")) s->policy = H26X_WARN_DROP; else return ZST_ERROR; return ZST_OK; }
    n = strtoul(value, NULL, 10); if (!n) return ZST_ERROR; if (!strcmp(name, "nal-length-size") && n <= 4) s->nal_length_size = n; else if (!strcmp(name, "max-nal-size")) s->max_nal_size = n; else if (!strcmp(name, "max-au-size")) s->max_au_size = n; else if (!strcmp(name, "max-buffered-bytes")) s->max_buffered_bytes = n; else return ZST_ERROR; return ZST_OK;
}

static zst_result_t parser_get_property(zst_element_t *el, const char *name, char *out, size_t size)
{ h26x_parser_t *s = el ? el->priv : NULL; if (!s || !name || !out || !size) return ZST_ERROR; if (!strcmp(name, "codec")) snprintf(out, size, "%s", s->codec == H26X_CODEC_H264 ? "h264" : s->codec == H26X_CODEC_H265 ? "h265" : "auto"); else if (!strcmp(name, "parsed-nals")) snprintf(out, size, "%llu", (unsigned long long)s->parsed_nals); else if (!strcmp(name, "output-buffers")) snprintf(out, size, "%llu", (unsigned long long)s->output_buffers); else if (!strcmp(name, "parse-errors")) snprintf(out, size, "%llu", (unsigned long long)s->parse_errors); else if (!strcmp(name, "dropped-nals")) snprintf(out, size, "%llu", (unsigned long long)s->dropped_nals); else if (!strcmp(name, "format-generation")) snprintf(out, size, "%llu", (unsigned long long)s->generation); else return ZST_ERROR; return ZST_OK; }

static zst_element_ops_t g_ops = { .name = "h26xparse", .open = parser_open, .close = parser_close, .process = NULL, .event = parser_event, .set_property = parser_set_property, .get_property = parser_get_property };

zst_element_t *zst_h26x_parser_create(void)
{
    h26x_parser_t *s = calloc(1, sizeof(*s)); zst_element_t *el; zst_caps_t *caps;
    if (!s) return NULL; s->codec = H26X_CODEC_AUTO; s->input_format = H26X_FMT_AUTO; s->nal_length_size = 4; s->policy = H26X_ERROR; s->max_nal_size = H26X_DEFAULT_MAX_NAL; s->max_au_size = H26X_DEFAULT_MAX_BUFFERED; s->max_buffered_bytes = H26X_DEFAULT_MAX_BUFFERED;
    el = zst_element_create(&g_ops, s); if (!el) { free(s); return NULL; } s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK); s->src_pad = zst_pad_create("src", ZST_PAD_SRC); if (!s->sink_pad || !s->src_pad) { zst_element_destroy(el); return NULL; } s->sink_pad->push = parser_push; s->sink_pad->priv = s; s->sink_pad->destroy_priv = parser_pad_destroy; zst_element_add_pad(el, s->sink_pad); zst_element_add_pad(el, s->src_pad);
    caps = zst_caps_create(); if (caps) { zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0, "")); zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0, "")); zst_pad_set_template_caps(s->sink_pad, caps); zst_pad_set_template_caps(s->src_pad, caps); zst_caps_destroy(caps); }
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
static zst_element_t *plugin_create(const char *name) { return name && !strcmp(name, "h26xparse") ? zst_h26x_parser_create() : NULL; }
static const zst_pad_template_t g_pads[] = { { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265" }, { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264;video/x-h265" } };
static const zst_property_spec_t g_props[] = { { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "auto", "Input codec: auto, h264, h265" }, { "input-stream-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "auto", "Input format: auto, byte-stream, avc, hvc1, hev1" }, { "nal-length-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4", "Length-prefix field size" }, { "aggregate-au", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Aggregate NALs into access units" }, { "malformed-policy", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "error", "Malformed input policy" } };
static const zst_element_desc_t g_elements[] = { { .name = "h26xparse", .long_name = "H.264/H.265 Parser", .category = "Codec/Parser", .description = "Normalizes AVC and HEVC elementary streams to Annex-B", .author = "zstreamer", .properties = g_props, .nb_properties = sizeof(g_props)/sizeof(g_props[0]), .pads = g_pads, .nb_pads = 2, .create = NULL } };
static zst_plugin_t g_plugin = { .desc = { .name = "h26xparse_plugin", .author = "zstreamer", .version = "0.1.0" }, .create_element = plugin_create };
ZST_PLUGIN_EXPORT const zst_element_desc_t *zst_get_plugin_elements(uint32_t *n) { if (n) *n = 1; return g_elements; }
ZST_PLUGIN_EXPORT zst_plugin_t *zst_get_plugin(void) { zst_plugin_t *p = malloc(sizeof(*p)); if (p) *p = g_plugin; return p; }
#endif
