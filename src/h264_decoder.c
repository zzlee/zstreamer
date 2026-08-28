/*=============================================================================
    h264_decoder.c — FFmpeg libavcodec H.264 video decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

typedef struct {
    AVCodecContext* codec_ctx;
    AVCodecParserContext* parser;
    AVFrame*        frame;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        width;
    uint32_t        height;
    int             format;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    int             threads;
    int             low_latency;
} h264_decoder_t;

static const char*
h264_pix_fmt_to_string(int fmt)
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
h264_dec_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static int
h264_is_annexb(const uint8_t* data, size_t size)
{
    if (!data || size < 4) return 0;
    return (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) ||
           (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01);
}

static int
h264_avcc_4byte_lengths_valid(const uint8_t* data, size_t size)
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
h264_packet_from_buffer(zst_buffer_t* in, AVPacket* pkt)
{
    if (!pkt || !in) return ZST_ERROR;

    pkt->pts = in->pts;
    pkt->dts = in->dts;
    pkt->duration = in->duration;

    const uint8_t* data = (const uint8_t*)in->memory.data;
    size_t size = in->memory.size;
    if (!data || size == 0) return ZST_ERROR;

    if (!h264_is_annexb(data, size) && h264_avcc_4byte_lengths_valid(data, size)) {
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
h264_open(zst_element_t* el)
{
    h264_decoder_t* s = el->priv;
    s->codec_ctx = NULL;
    s->parser = NULL;
    s->frame = NULL;
    s->initialized = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->format = AV_PIX_FMT_NONE;
    s->threads = 0; /* auto */
    s->low_latency = 0;
    return ZST_OK;
}

static zst_result_t
h264_close(zst_element_t* el)
{
    h264_decoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->parser) {
        av_parser_close(s->parser);
        s->parser = NULL;
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
h264_init_decoder(h264_decoder_t* s)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return ZST_ERROR;

    s->parser = av_parser_init(codec->id);
    if (!s->parser) return ZST_ERROR;

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) {
        av_parser_close(s->parser);
        s->parser = NULL;
        return ZST_ERROR;
    }

    if (s->threads > 0) {
        s->codec_ctx->thread_count = s->threads;
    } else {
        s->codec_ctx->thread_count = 0; /* auto-detect */
    }

    if (s->low_latency) {
        s->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        s->codec_ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        s->codec_ctx->thread_type = FF_THREAD_SLICE;
    }

    if (avcodec_open2(s->codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        av_parser_close(s->parser);
        s->parser = NULL;
        return ZST_ERROR;
    }

    s->frame = av_frame_alloc();
    if (!s->frame) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
        av_parser_close(s->parser);
        s->parser = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
h264_update_pool(h264_decoder_t* s, int width, int height, int format)
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
h264_emit_buffer(zst_element_t* el, zst_buffer_t* buf, zst_buffer_t** out)
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
h264_emit_frame(zst_element_t* el, h264_decoder_t* s, zst_buffer_t** out)
{
    if (h264_update_pool(s, s->frame->width, s->frame->height, s->frame->format) != ZST_OK) {
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
    int64_t frame_duration = 0;
#if LIBAVUTIL_VERSION_MAJOR >= 58
    frame_duration = s->frame->duration;
#else
    frame_duration = s->frame->pkt_duration;
#endif
    if (frame_duration > 0) {
        vbuf->duration = (zst_time_t)frame_duration;
    }

    zst_video_frame_t* v_frame = vbuf->payload;
    if (!v_frame) {
        v_frame = calloc(1, sizeof(*v_frame));
        if (!v_frame) {
            zst_buffer_unref(vbuf);
            return ZST_ERROR;
        }
        vbuf->payload = v_frame;
        vbuf->destroy = h264_dec_buf_free;
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

    return h264_emit_buffer(el, vbuf, out);
}

static zst_result_t
h264_decode_packet(zst_element_t* el, h264_decoder_t* s, AVPacket* av_pkt, zst_buffer_t** out)
{
    int ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        fprintf(stderr, "=== DIAGNOSTIC: avcodec_send_packet returned %d ===\n", ret);
    }
    if (ret == AVERROR(EAGAIN)) {
        while (1) {
            int recv_ret = avcodec_receive_frame(s->codec_ctx, s->frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            } else if (recv_ret < 0) {
                avcodec_flush_buffers(s->codec_ctx);
                return recv_ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
            }
            zst_result_t emit_ret = h264_emit_frame(el, s, out);
            av_frame_unref(s->frame);
            if (emit_ret != ZST_OK) return emit_ret;
        }
        ret = avcodec_send_packet(s->codec_ctx, av_pkt);
    }
    
    if (ret < 0 && ret != AVERROR_EOF) {
        avcodec_flush_buffers(s->codec_ctx);
        return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
    }

    while (1) {
        ret = avcodec_receive_frame(s->codec_ctx, s->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            avcodec_flush_buffers(s->codec_ctx);
            return ret == AVERROR_INVALIDDATA ? ZST_AGAIN : ZST_ERROR;
        }
        zst_result_t emit_ret = h264_emit_frame(el, s, out);
        av_frame_unref(s->frame);
        if (emit_ret != ZST_OK) return emit_ret;
    }
    return ZST_OK;
}

static zst_result_t
h264_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h264_decoder_t* s = el->priv;
    if (out) *out = NULL;
    if (!in) return ZST_ERROR;

    if (!s->initialized) {
        if (h264_init_decoder(s) != ZST_OK) return ZST_ERROR;
    }

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        uint8_t* p_out = NULL;
        int p_out_size = 0;
        av_parser_parse2(s->parser, s->codec_ctx, &p_out, &p_out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (p_out_size > 0) {
            AVPacket pkt = {0};
            pkt.data = p_out;
            pkt.size = p_out_size;
            h264_decode_packet(el, s, &pkt, out);
        }
        h264_decode_packet(el, s, NULL, out);

        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
        return h264_emit_buffer(el, eos_buf, out);
    }

    AVPacket stack_pkt = {0};
    if (h264_packet_from_buffer(in, &stack_pkt) != ZST_OK) {
        return ZST_ERROR;
    }

    zst_result_t res = ZST_OK;

    if (s->low_latency) {
        res = h264_decode_packet(el, s, &stack_pkt, out);
    } else {
        uint8_t* in_data = stack_pkt.data;
        int in_size = stack_pkt.size;
        
        while (in_size > 0) {
            uint8_t* p_out = NULL;
            int p_out_size = 0;
            int len = av_parser_parse2(s->parser, s->codec_ctx,
                                       &p_out, &p_out_size,
                                       in_data, in_size,
                                       stack_pkt.pts, stack_pkt.dts, -1);
            if (len < 0) {
                res = ZST_ERROR;
                break;
            }
            in_data += len;
            in_size -= len;

            if (p_out_size > 0) {
                AVPacket p_pkt = {0};
                p_pkt.data = p_out;
                p_pkt.size = p_out_size;
                p_pkt.pts = s->parser->pts;
                p_pkt.dts = s->parser->dts;
                res = h264_decode_packet(el, s, &p_pkt, out);
                if (res != ZST_OK && res != ZST_AGAIN) {
                    break;
                }
            }
        }
    }

    if (stack_pkt.buf) av_packet_unref(&stack_pkt);
    return res;
}

static zst_result_t
h264_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = h264_process(pad->parent, buf, &out);
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
h264_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    h264_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           0.0,
                                                           h264_pix_fmt_to_string(s->format)));
    }

    return caps;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    h264_decoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
h264_dec_set_property(zst_element_t* el, const char* name, const char* value)
{
    h264_decoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "threads") == 0) {
        s->threads = atoi(value);
        if (s->threads < 0) s->threads = 0;
        return ZST_OK;
    } else if (strcmp(name, "low-latency") == 0) {
        if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
            s->low_latency = 1;
        } else {
            s->low_latency = 0;
        }
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
h264_dec_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    h264_decoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "threads") == 0) {
        snprintf(value_out, max_len, "%d", s->threads);
    } else if (strcmp(name, "low-latency") == 0) {
        snprintf(value_out, max_len, "%s", s->low_latency ? "true" : "false");
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name     = "h264dec",
    .open     = h264_open,
    .close    = h264_close,
    .process  = h264_process,
    .get_caps = h264_get_caps,
    .set_property = h264_dec_set_property,
    .get_property = h264_dec_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_h264_decoder_create(void)
{
    zst_element_t* el;
    h264_decoder_t* priv;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    el = zst_element_create(&g_ops, priv);
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
    priv->sinkpad->push = h264_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "h264dec") == 0) {
        return zst_h264_decoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_h264dec_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_h264dec_elements[] = {
    {
        .name = "h264dec",
        .long_name = "H.264 Decoder",
        .category = "Codec/Decoder",
        .description = "Decodes H.264 video frames",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_h264dec_pads,
        .nb_pads = sizeof(g_h264dec_pads) / sizeof(g_h264dec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h264decoder_plugin",
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
        *nb_elements_out = sizeof(g_h264dec_elements) / sizeof(g_h264dec_elements[0]);
    }
    return g_h264dec_elements;
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
