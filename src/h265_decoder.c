/*=============================================================================
    h265_decoder.c — FFmpeg libavcodec H.265 video decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_h265_decoder.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    int             format;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;
} h265_decoder_t;

static const char*
h265_pix_fmt_to_string(int fmt)
{
    switch ((enum AVPixelFormat)fmt) {
    case AV_PIX_FMT_YUV420P: return "YUV420P";
    case AV_PIX_FMT_NV12:    return "NV12";
    case AV_PIX_FMT_NV21:    return "NV21";
    case AV_PIX_FMT_YUYV422: return "YUYV422";
    case AV_PIX_FMT_UYVY422: return "UYVY";
    case AV_PIX_FMT_RGB24:   return "RGB24";
    case AV_PIX_FMT_BGR24:   return "BGR24";
    case AV_PIX_FMT_RGBA:    return "RGBA";
    case AV_PIX_FMT_BGRA:    return "BGRA";
    default:                 return "";
    }
}

static void
h265_dec_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static int
h265_is_annexb(const uint8_t* data, size_t size)
{
    if (!data || size < 4) return 0;
    return (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) ||
           (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01);
}

static int
h265_hvcc_config_record(const uint8_t* data, size_t size)
{
    return data && size > 23 && data[0] == 0x01;
}

static int
h265_hvcc_4byte_lengths_valid(const uint8_t* data, size_t size)
{
    size_t pos = 0;
    int nals = 0;
    while (pos + 4 <= size) {
        uint32_t n = ((uint32_t)data[pos] << 24) |
                     ((uint32_t)data[pos + 1] << 16) |
                     ((uint32_t)data[pos + 2] << 8) |
                     (uint32_t)data[pos + 3];
        pos += 4;
        if (n == 0 || n > size - pos) return 0;
        pos += n;
        nals++;
    }
    return pos == size && nals > 0;
}

static zst_result_t
h265_packet_from_hvcc_config(const uint8_t* data, size_t size, AVPacket* pkt)
{
    if (!data || !pkt || size <= 23 || data[0] != 0x01) return ZST_ERROR;

    const uint8_t* p = data + 22;
    const uint8_t* end = data + size;
    uint8_t length_size = (uint8_t)((data[21] & 0x03u) + 1u);
    (void)length_size;
    uint8_t num_arrays = *p++;
    size_t out_size = 0;

    const uint8_t* q = p;
    for (uint8_t ai = 0; ai < num_arrays; ai++) {
        if (q + 3 > end) return ZST_ERROR;
        q++; /* array_completeness + NAL_unit_type */
        uint16_t num_nalus = ((uint16_t)q[0] << 8) | q[1];
        q += 2;
        for (uint16_t ni = 0; ni < num_nalus; ni++) {
            if (q + 2 > end) return ZST_ERROR;
            uint16_t n = ((uint16_t)q[0] << 8) | q[1];
            q += 2;
            if (q + n > end) return ZST_ERROR;
            out_size += 4u + n;
            q += n;
        }
    }
    if (out_size == 0 || out_size > INT_MAX) return ZST_ERROR;

    if (av_new_packet(pkt, (int)out_size) < 0) return ZST_ERROR;
    uint8_t* out = pkt->data;
    for (uint8_t ai = 0; ai < num_arrays; ai++) {
        p++; /* array header */
        uint16_t num_nalus = ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        for (uint16_t ni = 0; ni < num_nalus; ni++) {
            uint16_t n = ((uint16_t)p[0] << 8) | p[1];
            p += 2;
            out[0] = 0x00;
            out[1] = 0x00;
            out[2] = 0x00;
            out[3] = 0x01;
            out += 4;
            memcpy(out, p, n);
            out += n;
            p += n;
        }
    }
    return ZST_OK;
}

static zst_result_t
h265_packet_from_buffer(zst_buffer_t* in, AVPacket* pkt)
{
    if (!pkt || !in) return ZST_ERROR;

    pkt->pts = in->pts;
    pkt->dts = in->dts;
    pkt->duration = in->duration;

    const uint8_t* data = (const uint8_t*)in->memory.data;
    size_t size = in->memory.size;
    if (!data || size == 0) return ZST_ERROR;

    if (h265_hvcc_config_record(data, size)) {
        if (h265_packet_from_hvcc_config(data, size, pkt) != ZST_OK) return ZST_ERROR;
    } else if (!h265_is_annexb(data, size) && h265_hvcc_4byte_lengths_valid(data, size)) {
        if (av_new_packet(pkt, (int)size) < 0) return ZST_ERROR;
        size_t pos = 0;
        uint8_t* out = pkt->data;
        while (pos + 4 <= size) {
            uint32_t n = ((uint32_t)data[pos] << 24) |
                         ((uint32_t)data[pos + 1] << 16) |
                         ((uint32_t)data[pos + 2] << 8) |
                         (uint32_t)data[pos + 3];
            pos += 4;
            *out++ = 0x00;
            *out++ = 0x00;
            *out++ = 0x00;
            *out++ = 0x01;
            memcpy(out, data + pos, n);
            out += n;
            pos += n;
        }
    } else {
        pkt->data = (uint8_t*)data;
        pkt->size = (int)size;
    }

    return ZST_OK;
}

static zst_result_t
h265_open(zst_element_t* el)
{
    h265_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->format = AV_PIX_FMT_NONE;
    return ZST_OK;
}

static zst_result_t
h265_close(zst_element_t* el)
{
    h265_decoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->frame) {
        av_frame_free(&s->frame);
        s->frame = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
h265_init_decoder(h265_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->frame = av_frame_alloc();
    if (!s->frame) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
h265_update_pool(h265_decoder_t* s, int width, int height, int format)
{
    if (s->pool && s->width == (uint32_t)width && s->height == (uint32_t)height && s->format == format) {
        return ZST_OK;
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    int size = av_image_get_buffer_size((enum AVPixelFormat)format, width, height, 1);
    if (size < 0) return ZST_ERROR;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = (size_t)size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) return ZST_ERROR;

    s->width = (uint32_t)width;
    s->height = (uint32_t)height;
    s->format = format;
    return ZST_OK;
}

static zst_result_t
h265_emit_buffer(zst_element_t* el, zst_buffer_t* buf, zst_buffer_t** out)
{
    if (!buf) return ZST_ERROR;

    if (el->nb_src_pads > 0 && el->src_pads[0]->peer) {
        zst_result_t ret = zst_pad_push(el->src_pads[0], buf);
        zst_buffer_unref(buf);
        return ret;
    }

    if (out && *out == NULL) {
        *out = buf;
        return ZST_OK;
    }

    zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t
h265_emit_frame(zst_element_t* el, h265_decoder_t* s, zst_buffer_t** out)
{
    if (h265_update_pool(s, s->frame->width, s->frame->height, s->frame->format) != ZST_OK) {
        return ZST_ERROR;
    }

    zst_buffer_t* vbuf = NULL;
    if (zst_buffer_pool_acquire(s->pool, &vbuf, 0, 0) != ZST_OK) {
        return ZST_ERROR;
    }

    int64_t pts = s->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = s->frame->pts;
    if (pts != AV_NOPTS_VALUE) {
        vbuf->pts = (zst_time_t)pts;
        vbuf->dts = (zst_time_t)pts;
    }
    if (s->frame->duration > 0) {
        vbuf->duration = (zst_time_t)s->frame->duration;
    }

    zst_video_frame_t* v_frame = vbuf->payload;
    if (!v_frame) {
        v_frame = calloc(1, sizeof(*v_frame));
        if (!v_frame) {
            zst_buffer_unref(vbuf);
            return ZST_ERROR;
        }
        vbuf->payload = v_frame;
        vbuf->destroy = h265_dec_buf_free;
    }

    v_frame->width = (uint32_t)s->frame->width;
    v_frame->height = (uint32_t)s->frame->height;
    v_frame->format = (uint32_t)s->frame->format;

    uint8_t* dst_data[4] = {0};
    int dst_linesize[4] = {0};
    int fill_ret = av_image_fill_arrays(dst_data, dst_linesize, vbuf->memory.data,
                                        (enum AVPixelFormat)s->frame->format,
                                        s->frame->width, s->frame->height, 1);
    if (fill_ret < 0) {
        zst_buffer_unref(vbuf);
        return ZST_ERROR;
    }
    vbuf->memory.size = (size_t)fill_ret;

    av_image_copy(dst_data, dst_linesize,
                  (const uint8_t**)s->frame->data, s->frame->linesize,
                  (enum AVPixelFormat)s->frame->format,
                  s->frame->width, s->frame->height);

    for (int i = 0; i < 4; i++) {
        v_frame->plane[i] = dst_data[i];
        v_frame->stride[i] = (uint32_t)dst_linesize[i];
    }

    return h265_emit_buffer(el, vbuf, out);
}

static zst_result_t
h265_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h265_decoder_t* s = el->priv;
    if (out) *out = NULL;

    if (!in) {
        return ZST_ERROR;
    }

    if (!s->initialized) {
        if (h265_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    AVPacket* av_pkt = NULL;
    AVPacket stack_pkt = {0};
    if (!(in->flags & ZST_BUFFER_FLAG_EOS)) {
        if (h265_packet_from_buffer(in, &stack_pkt) != ZST_OK) {
            return ZST_ERROR;
        }
        av_pkt = &stack_pkt;
    }

    int ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    if (av_pkt && av_pkt->buf) {
        av_packet_unref(av_pkt);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
#ifdef AVERROR_INPUT_CHANGED
        if (ret == AVERROR_INPUT_CHANGED) {
            avcodec_flush_buffers(s->codec_ctx);
            return ZST_AGAIN;
        }
#endif
        avcodec_flush_buffers(s->codec_ctx);
        return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
    }

    while (1) {
        ret = avcodec_receive_frame(s->codec_ctx, s->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
#ifdef AVERROR_INPUT_CHANGED
            if (ret == AVERROR_INPUT_CHANGED) {
                avcodec_flush_buffers(s->codec_ctx);
                return ZST_AGAIN;
            }
#endif
            avcodec_flush_buffers(s->codec_ctx);
            return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
        }

        zst_result_t emit_ret = h265_emit_frame(el, s, out);
        av_frame_unref(s->frame);
        if (emit_ret != ZST_OK) {
            return emit_ret;
        }
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
        return h265_emit_buffer(el, eos_buf, out);
    }

    return ZST_OK;
}

static zst_result_t
h265_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = h265_process(pad->parent, buf, &out);
    if (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
        } else {
            zst_buffer_unref(out);
        }
    }
    return ret;
}

static zst_caps_t*
h265_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    h265_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           0.0,
                                                           h265_pix_fmt_to_string(s->format)));
    }

    return caps;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    h265_decoder_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name     = "h265dec",
    .open     = h265_open,
    .close    = h265_close,
    .process  = h265_process,
    .get_caps = h265_get_caps,
    .get_pool = element_get_pool
};

zst_element_t*
zst_h265_decoder_create(void)
{
    h265_decoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sinkpad->push = h265_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}



zst_element_t*
zst_h265_decoder_create_with_config(const zst_h265_decoder_config_t* config)
{
    (void)config;
    return zst_element_factory_make("h265dec");
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "h265dec") == 0) {
        return zst_h265_decoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_h265dec_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-h265" },
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_element_desc_t g_h265dec_elements[] = {
    {
        .name = "h265dec",
        .long_name = "H.265 Decoder",
        .category = "Codec/Decoder",
        .description = "Decodes H.265 video frames",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_h265dec_pads,
        .nb_pads = sizeof(g_h265dec_pads) / sizeof(g_h265dec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h265decoder_plugin",
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
        *nb_elements_out = sizeof(g_h265dec_elements) / sizeof(g_h265dec_elements[0]);
    }
    return g_h265dec_elements;
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
