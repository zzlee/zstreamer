/*=============================================================================
    video_scaler.c — Video scaling and color space conversion element
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_video_scaler.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    int target_width;
    int target_height;
    char target_pixel_format[32];

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;

    struct SwsContext* sws_ctx;
    int current_in_width;
    int current_in_height;
    enum AVPixelFormat current_in_format;

    int current_out_width;
    int current_out_height;
    enum AVPixelFormat current_out_format;

    zst_buffer_pool_t* pool;
} video_scaler_t;

static enum AVPixelFormat
pixel_format_from_str(const char* name)
{
    if (!name || name[0] == '\0') return AV_PIX_FMT_NONE;
    if (strcmp(name, "YUV420P") == 0 || strcmp(name, "I420") == 0) return AV_PIX_FMT_YUV420P;
    if (strcmp(name, "NV12") == 0) return AV_PIX_FMT_NV12;
    if (strcmp(name, "YUYV") == 0 || strcmp(name, "YUY2") == 0) return AV_PIX_FMT_YUYV422;
    if (strcmp(name, "RGB24") == 0) return AV_PIX_FMT_RGB24;
    if (strcmp(name, "BGR24") == 0) return AV_PIX_FMT_BGR24;
    return AV_PIX_FMT_NONE;
}

static zst_result_t
scaler_open(zst_element_t* el)
{
    video_scaler_t* s = el->priv;
    s->sws_ctx = NULL;
    s->current_in_width = 0;
    s->current_in_height = 0;
    s->current_in_format = AV_PIX_FMT_NONE;
    s->current_out_width = 0;
    s->current_out_height = 0;
    s->current_out_format = AV_PIX_FMT_NONE;
    return ZST_OK;
}

static zst_result_t
scaler_close(zst_element_t* el)
{
    video_scaler_t* s = el->priv;
    if (s->sws_ctx) {
        sws_freeContext(s->sws_ctx);
        s->sws_ctx = NULL;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_caps_t*
scaler_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    video_scaler_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        /* Sink pad accepts any raw video */
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    } else if (pad == s->srcpad) {
        /* Src pad offers the target configuration, or propagates downstream */
        int w = s->target_width;
        int h = s->target_height;
        const char* fmt = s->target_pixel_format;

        if (w <= 0 && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_VIDEO) {
                w = st->video.width;
            }
        }
        if (h <= 0 && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_VIDEO) {
                h = st->video.height;
            }
        }
        if ((!fmt || fmt[0] == '\0') && s->sinkpad->peer && s->sinkpad->peer->caps) {
            zst_caps_struct_t* st = s->sinkpad->peer->caps->structs;
            if (st && st->type == ZST_CAPS_VIDEO) {
                fmt = st->video.pixel_format;
            }
        }

        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", w, h, 0.0, fmt ? fmt : ""));
    }
    return caps;
}

static void
nearest_neighbor_scale_yuv420p(
    uint32_t in_w, uint32_t in_h,
    const uint8_t* in_y, uint32_t in_stride_y,
    const uint8_t* in_u, uint32_t in_stride_u,
    const uint8_t* in_v, uint32_t in_stride_v,
    uint32_t out_w, uint32_t out_h,
    uint8_t* out_y, uint32_t out_stride_y,
    uint8_t* out_u, uint32_t out_stride_u,
    uint8_t* out_v, uint32_t out_stride_v)
{
    for (uint32_t r = 0; r < out_h; r++) {
        uint32_t in_r = (r * in_h) / out_h;
        for (uint32_t c = 0; c < out_w; c++) {
            uint32_t in_c = (c * in_w) / out_w;
            out_y[r * out_stride_y + c] = in_y[in_r * in_stride_y + in_c];
        }
    }

    uint32_t in_w_uv = in_w / 2;
    uint32_t in_h_uv = in_h / 2;
    uint32_t out_w_uv = out_w / 2;
    uint32_t out_h_uv = out_h / 2;

    for (uint32_t r = 0; r < out_h_uv; r++) {
        uint32_t in_r = (r * in_h_uv) / out_h_uv;
        for (uint32_t c = 0; c < out_w_uv; c++) {
            uint32_t in_c = (c * in_w_uv) / out_w_uv;
            out_u[r * out_stride_u + c] = in_u[in_r * in_stride_u + in_c];
            out_v[r * out_stride_v + c] = in_v[in_r * in_stride_v + in_c];
        }
    }
}

static void
nearest_neighbor_scale_nv12(
    uint32_t in_w, uint32_t in_h,
    const uint8_t* in_y, uint32_t in_stride_y,
    const uint8_t* in_uv, uint32_t in_stride_uv,
    uint32_t out_w, uint32_t out_h,
    uint8_t* out_y, uint32_t out_stride_y,
    uint8_t* out_uv, uint32_t out_stride_uv)
{
    for (uint32_t r = 0; r < out_h; r++) {
        uint32_t in_r = (r * in_h) / out_h;
        for (uint32_t c = 0; c < out_w; c++) {
            uint32_t in_c = (c * in_w) / out_w;
            out_y[r * out_stride_y + c] = in_y[in_r * in_stride_y + in_c];
        }
    }

    uint32_t in_w_uv = in_w / 2;
    uint32_t in_h_uv = in_h / 2;
    uint32_t out_w_uv = out_w / 2;
    uint32_t out_h_uv = out_h / 2;

    for (uint32_t r = 0; r < out_h_uv; r++) {
        uint32_t in_r = (r * in_h_uv) / out_h_uv;
        for (uint32_t c = 0; c < out_w_uv; c++) {
            uint32_t in_c = (c * in_w_uv) / out_w_uv;
            out_uv[r * out_stride_uv + 2 * c] = in_uv[in_r * in_stride_uv + 2 * in_c];
            out_uv[r * out_stride_uv + 2 * c + 1] = in_uv[in_r * in_stride_uv + 2 * in_c + 1];
        }
    }
}

static void
scaler_passthrough_destroy(zst_buffer_t* b)
{
    if (b && b->payload) {
        free(b->payload);
        b->payload = NULL;
    }
}

static void
scaler_scaled_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        // memory.data is managed by the allocator
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}

static zst_result_t
scaler_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    video_scaler_t* s = el->priv;
    if (!in) return ZST_ERROR;

    /* EOS Passthrough */
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
        return ZST_ERROR;
    }

    zst_video_frame_t* in_frame = in->payload;
    if (!in_frame) return ZST_ERROR;

    /* Resolve output configuration */
    int out_width = 0;
    int out_height = 0;
    const char* out_fmt_str = NULL;

    if (s->srcpad->caps && s->srcpad->caps->structs) {
        zst_caps_struct_t* s_caps = s->srcpad->caps->structs;
        if (s_caps->type == ZST_CAPS_VIDEO) {
            out_width = s_caps->video.width;
            out_height = s_caps->video.height;
            out_fmt_str = s_caps->video.pixel_format;
        }
    }

    if (out_width <= 0) out_width = s->target_width;
    if (out_height <= 0) out_height = s->target_height;
    if (!out_fmt_str || out_fmt_str[0] == '\0') out_fmt_str = s->target_pixel_format;

    if (out_width <= 0) out_width = in_frame->width;
    if (out_height <= 0) out_height = in_frame->height;

    enum AVPixelFormat in_pix_fmt = (enum AVPixelFormat)in_frame->format;
    enum AVPixelFormat out_pix_fmt = AV_PIX_FMT_NONE;
    if (out_fmt_str && out_fmt_str[0] != '\0') {
        out_pix_fmt = pixel_format_from_str(out_fmt_str);
    }
    if (out_pix_fmt == AV_PIX_FMT_NONE) {
        out_pix_fmt = in_pix_fmt;
    }

    /* Passthrough check */
    if (in_frame->width == (uint32_t)out_width &&
        in_frame->height == (uint32_t)out_height &&
        in_pix_fmt == out_pix_fmt) {

        zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!out_buf) return ZST_ERROR;

        out_buf->pts = in->pts;
        out_buf->dts = in->dts;
        out_buf->duration = in->duration;
        out_buf->flags = in->flags;

        out_buf->memory.type = in->memory.type;
        out_buf->memory.data = in->memory.data;
        out_buf->memory.size = in->memory.size;

        out_buf->memory.priv = zst_buffer_ref(in);
        out_buf->memory.release = (void (*)(void*))zst_buffer_unref;

        zst_video_frame_t* out_frame = calloc(1, sizeof(*out_frame));
        if (!out_frame) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
        *out_frame = *in_frame;
        out_buf->payload = out_frame;
        out_buf->destroy = scaler_passthrough_destroy;

        *out = out_buf;
        return ZST_OK;
    }

    /* Reallocate sws context if input/output parameters changed */
    if (s->sws_ctx && (s->current_in_width != (int)in_frame->width ||
                       s->current_in_height != (int)in_frame->height ||
                       s->current_in_format != in_pix_fmt ||
                       s->current_out_width != out_width ||
                       s->current_out_height != out_height ||
                       s->current_out_format != out_pix_fmt)) {
        sws_freeContext(s->sws_ctx);
        s->sws_ctx = NULL;
    }

    if (!s->sws_ctx) {
        s->sws_ctx = sws_getContext(
            in_frame->width, in_frame->height, in_pix_fmt,
            out_width, out_height, out_pix_fmt,
            SWS_BILINEAR, NULL, NULL, NULL
        );
        if (s->sws_ctx) {
            s->current_in_width = in_frame->width;
            s->current_in_height = in_frame->height;
            s->current_in_format = in_pix_fmt;
            s->current_out_width = out_width;
            s->current_out_height = out_height;
            s->current_out_format = out_pix_fmt;
        }
    }

    /* Recreate buffer pool if size changed or it doesn't exist */
    int out_size = av_image_get_buffer_size(out_pix_fmt, out_width, out_height, 1);
    if (out_size < 0) return ZST_ERROR;

    int needs_new_pool = 1;
    if (s->pool) {
        zst_buffer_pool_config_t cfg = zst_buffer_pool_get_config(s->pool);
        if (cfg.buffer_size == (size_t)out_size) {
            needs_new_pool = 0;
        } else {
            zst_buffer_pool_destroy(s->pool);
            s->pool = NULL;
        }
    }

    if (needs_new_pool) {
        zst_buffer_pool_config_t pool_cfg = {
            .min_buffers = 2,
            .max_buffers = 8,
            .buffer_size = out_size,
            .buffer_type = ZST_BUFFER_VIDEO_FRAME
        };
        s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
        if (!s->pool) return ZST_ERROR;
    }

    zst_buffer_t* out_buf = zst_buffer_create_with_pool(s->pool);
    if (!out_buf) {
        return ZST_ERROR;
    }

    uint8_t* out_data = out_buf->memory.data;

    zst_video_frame_t* out_frame = out_buf->payload;
    if (!out_frame) {
        out_frame = calloc(1, sizeof(*out_frame));
        if (!out_frame) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
        out_buf->payload = out_frame;
        out_buf->destroy = scaler_scaled_buf_free;
    }

    out_frame->width = out_width;
    out_frame->height = out_height;
    out_frame->format = out_pix_fmt;

    uint8_t* tmp_data[4] = {0};
    int tmp_linesize[4] = {0};
    int fill_ret = av_image_fill_arrays(tmp_data, tmp_linesize, out_data, out_pix_fmt, out_width, out_height, 1);
    if (fill_ret < 0) {
        zst_buffer_unref(out_buf);
        return ZST_ERROR;
    }
    for (int i = 0; i < 4; i++) {
        out_frame->plane[i] = tmp_data[i];
        out_frame->stride[i] = tmp_linesize[i];
    }

    if (s->sws_ctx) {
        const uint8_t* const src_slices[4] = {
            in_frame->plane[0],
            in_frame->plane[1],
            in_frame->plane[2],
            in_frame->plane[3]
        };
        int src_strides[4] = {
            (int)in_frame->stride[0],
            (int)in_frame->stride[1],
            (int)in_frame->stride[2],
            (int)in_frame->stride[3]
        };

        int scale_ret = sws_scale(
            s->sws_ctx,
            src_slices,
            src_strides,
            0,
            in_frame->height,
            tmp_data,
            tmp_linesize
        );
        if (scale_ret < 0) {
            zst_buffer_unref(out_buf);
            return ZST_ERROR;
        }
    } else {
        /* Synthetic Fallback (Nearest Neighbor) */
        if (in_pix_fmt == AV_PIX_FMT_YUV420P && out_pix_fmt == AV_PIX_FMT_YUV420P) {
            nearest_neighbor_scale_yuv420p(
                in_frame->width, in_frame->height,
                in_frame->plane[0], in_frame->stride[0],
                in_frame->plane[1], in_frame->stride[1],
                in_frame->plane[2], in_frame->stride[2],
                out_width, out_height,
                out_frame->plane[0], out_frame->stride[0],
                out_frame->plane[1], out_frame->stride[1],
                out_frame->plane[2], out_frame->stride[2]
            );
        } else if (in_pix_fmt == AV_PIX_FMT_NV12 && out_pix_fmt == AV_PIX_FMT_NV12) {
            nearest_neighbor_scale_nv12(
                in_frame->width, in_frame->height,
                in_frame->plane[0], in_frame->stride[0],
                in_frame->plane[1], in_frame->stride[1],
                out_width, out_height,
                out_frame->plane[0], out_frame->stride[0],
                out_frame->plane[1], out_frame->stride[1]
            );
        } else {
            /* Unsupported fallback formats: fill with black */
            memset(out_data, 16, out_size);
        }
    }

    out_buf->pts = in->pts;
    out_buf->dts = in->dts;
    out_buf->duration = in->duration;
    out_buf->flags = in->flags;

    *out = out_buf;
    return ZST_OK;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    video_scaler_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name     = "videoscaler",
    .open     = scaler_open,
    .close    = scaler_close,
    .process  = scaler_process,
    .get_caps = scaler_get_caps,
    .get_pool = element_get_pool
};

zst_element_t*
zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format)
{
    video_scaler_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->target_width = target_width;
    priv->target_height = target_height;
    if (target_pixel_format) {
        strncpy(priv->target_pixel_format, target_pixel_format, sizeof(priv->target_pixel_format) - 1);
    }

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}



zst_element_t*
zst_video_scaler_create_with_config(const zst_video_scaler_config_t* config)
{

    return zst_video_scaler_create(config ? config->target_width : 0, config ? config->target_height : 0, config ? config->target_pixel_format : NULL);
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "videoscaler") == 0) {
        return zst_video_scaler_create(0, 0, NULL);
    }
    return NULL;
}

static const zst_pad_template_t g_videoscaler_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-raw" },
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_element_desc_t g_videoscaler_elements[] = {
    {
        .name = "videoscaler",
        .long_name = "Video Scaler",
        .category = "Filter/Video",
        .description = "Converts video resolution or pixel format",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_videoscaler_pads,
        .nb_pads = sizeof(g_videoscaler_pads) / sizeof(g_videoscaler_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "videoscaler_plugin",
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
        *nb_elements_out = sizeof(g_videoscaler_elements) / sizeof(g_videoscaler_elements[0]);
    }
    return g_videoscaler_elements;
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
