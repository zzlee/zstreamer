/*=============================================================================
    nv_video_scaler.c — NVIDIA V4L2 video scaler / converter implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <linux/videodev2.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"

#ifndef V4L2_PIX_FMT_YUV420M
#define V4L2_PIX_FMT_YUV420M v4l2_fourcc('Y', 'M', '1', '2') /* YUV420 planar */
#endif

typedef struct {
    int             fd;
    int             initialized;
    zst_buffer_pool_t* pool;
    uint32_t        current_in_width;
    uint32_t        current_in_height;

    uint32_t        target_width;
    uint32_t        target_height;
    char            target_pixel_format[32];

    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    struct v4l2_buffer* capture_buffers;
    struct v4l2_buffer* output_buffers;
    uint32_t        nb_capture_buffers;
    uint32_t        nb_output_buffers;
    uint32_t        out_idx;
} nv_video_scaler_t;

static zst_result_t
nv_video_scaler_open(zst_element_t* el)
{
    nv_video_scaler_t* s = el->priv;
    s->fd = open("/dev/nvhost-vic", O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_ERROR("nvvideoscaler", "Failed to open /dev/nvhost-vic (ensure you are on Jetson)");
        return ZST_ERROR;
    }
    s->initialized = 0;
    s->pool = NULL;
    return ZST_OK;
}

static zst_result_t
nv_video_scaler_close(zst_element_t* el)
{
    nv_video_scaler_t* s = el->priv;
    if (s->fd >= 0) {
        if (s->capture_buffers) {
            for (uint32_t i = 0; i < s->nb_capture_buffers; i++) {
                for (int j = 0; j < 3; j++) {
                    if (s->capture_buffers[i].m.planes[j].m.userptr && s->capture_buffers[i].m.planes[j].m.userptr != (unsigned long)MAP_FAILED) {
                        munmap((void*)s->capture_buffers[i].m.planes[j].m.userptr, s->capture_buffers[i].m.planes[j].length);
                    }
                }
            }
            free(s->capture_buffers);
            s->capture_buffers = NULL;
        }
        if (s->output_buffers) {
            for (uint32_t i = 0; i < s->nb_output_buffers; i++) {
                for(int j = 0; j < 3; j++) {
                    if (s->output_buffers[i].m.planes[j].m.userptr && s->output_buffers[i].m.planes[j].m.userptr != (unsigned long)MAP_FAILED) {
                        munmap((void*)s->output_buffers[i].m.planes[j].m.userptr, s->output_buffers[i].m.planes[j].length);
                    }
                }
            }
            free(s->output_buffers);
            s->output_buffers = NULL;
        }
        close(s->fd);
        s->fd = -1;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_result_t
nv_video_scaler_start(zst_element_t* el)
{
    nv_video_scaler_t* s = el->priv;
    if (s->fd < 0) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t
nv_video_scaler_stop(zst_element_t* el)
{
    nv_video_scaler_t* s = el->priv;
    if (s->fd >= 0 && s->initialized) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    return ZST_OK;
}

static zst_result_t
nv_video_scaler_init_v4l2(nv_video_scaler_t* s, uint32_t in_width, uint32_t in_height, uint32_t out_width, uint32_t out_height)
{
    s->current_in_width = in_width;
    s->current_in_height = in_height;

    struct v4l2_format fmt = {0};

    // OUTPUT queue (input to the converter)
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = in_width;
    fmt.fmt.pix_mp.height = in_height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.num_planes = 3;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) return ZST_ERROR;

    // CAPTURE queue (output from the converter)
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = out_width;
    fmt.fmt.pix_mp.height = out_height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.num_planes = 3;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) return ZST_ERROR;

    // Request OUTPUT buffers
    struct v4l2_requestbuffers req = {0};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) return ZST_ERROR;
    s->nb_output_buffers = req.count;
    s->output_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[3] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 3;
        buf.m.planes = planes;
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) return ZST_ERROR;

        for (int j = 0; j < 3; j++) {
            s->output_buffers[i].m.planes[j].m.userptr = (unsigned long)mmap(NULL, planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[j].m.mem_offset);
            s->output_buffers[i].m.planes[j].length = planes[j].length;
        }
    }

    // Request CAPTURE buffers
    memset(&req, 0, sizeof(req));
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) return ZST_ERROR;
    s->nb_capture_buffers = req.count;
    s->capture_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[3] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 3;
        buf.m.planes = planes;
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) return ZST_ERROR;

        for (int j = 0; j < 3; j++) {
            s->capture_buffers[i].m.planes[j].m.userptr = (unsigned long)mmap(NULL, planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, planes[j].m.mem_offset);
            s->capture_buffers[i].m.planes[j].length = planes[j].length;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) return ZST_ERROR;

    // Queue all capture buffers to be ready
    for (uint32_t i = 0; i < s->nb_capture_buffers; i++) {
        struct v4l2_buffer vbuf_cap = {0};
        struct v4l2_plane planes_cap[3] = {0};
        vbuf_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        vbuf_cap.memory = V4L2_MEMORY_MMAP;
        vbuf_cap.index = i;
        vbuf_cap.length = 3;
        vbuf_cap.m.planes = planes_cap;
        ioctl(s->fd, VIDIOC_QBUF, &vbuf_cap);
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 16,
        .buffer_size = out_width * out_height * 3 / 2, // YUV420 size
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    s->initialized = 1;
    return ZST_OK;
}

static void nv_video_scaler_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static zst_result_t
nv_video_scaler_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    nv_video_scaler_t* s = el->priv;
    if (out) *out = NULL;
    if (!in || s->fd < 0) return ZST_ERROR;

    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;

    uint32_t out_width = s->target_width ? s->target_width : frame->width;
    uint32_t out_height = s->target_height ? s->target_height : frame->height;

    if (!s->initialized) {
        if (nv_video_scaler_init_v4l2(s, frame->width, frame->height, out_width, out_height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    struct v4l2_buffer vbuf_out = {0};
    struct v4l2_plane planes_out[3] = {0};
    vbuf_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    vbuf_out.memory = V4L2_MEMORY_MMAP;
    vbuf_out.length = 3;
    vbuf_out.m.planes = planes_out;
    vbuf_out.timestamp.tv_sec = in->pts / 1000000000;
    vbuf_out.timestamp.tv_usec = (in->pts % 1000000000) / 1000;

    int dq_out = ioctl(s->fd, VIDIOC_DQBUF, &vbuf_out);
    if (dq_out < 0 && errno != EAGAIN) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLOUT };
        poll(&pfd, 1, 100);
        dq_out = ioctl(s->fd, VIDIOC_DQBUF, &vbuf_out);
    }

    if (dq_out >= 0 || errno == EAGAIN) {
        if (dq_out < 0) {
            vbuf_out.index = s->out_idx;
            s->out_idx = (s->out_idx + 1) % s->nb_output_buffers;
        }

        for (int j = 0; j < 3; j++) {
            size_t bytes = (j == 0) ? frame->stride[0] * s->current_in_height : frame->stride[j] * (s->current_in_height / 2);
            memcpy((void*)s->output_buffers[vbuf_out.index].m.planes[j].m.userptr, frame->plane[j], bytes);
            planes_out[j].bytesused = bytes;
            planes_out[j].length = s->output_buffers[vbuf_out.index].m.planes[j].length;
        }

        ioctl(s->fd, VIDIOC_QBUF, &vbuf_out);
    }

    struct v4l2_buffer vbuf_cap = {0};
    struct v4l2_plane planes_cap[3] = {0};
    vbuf_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    vbuf_cap.memory = V4L2_MEMORY_MMAP;
    vbuf_cap.length = 3;
    vbuf_cap.m.planes = planes_cap;

    struct pollfd pfd_cap = { .fd = s->fd, .events = POLLIN };
    if (poll(&pfd_cap, 1, 100) > 0) {
        if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf_cap) == 0) {
            zst_buffer_t* pkt = NULL;
            if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) == ZST_OK) {
                size_t total_size = planes_cap[0].bytesused + planes_cap[1].bytesused + planes_cap[2].bytesused;
                pkt->memory.size = total_size;

                uint8_t* dst = pkt->memory.data;
                for (int j = 0; j < 3; j++) {
                    memcpy(dst, (void*)s->capture_buffers[vbuf_cap.index].m.planes[j].m.userptr, planes_cap[j].bytesused);
                    dst += planes_cap[j].bytesused;
                }

                zst_video_frame_t* out_frame = pkt->payload;
                if (!out_frame) {
                    out_frame = calloc(1, sizeof(*out_frame));
                    pkt->payload = out_frame;
                    // DO NOT OVERWRITE pkt->destroy to allow buffer pool reuse.
                }

                out_frame->width = out_width;
                out_frame->height = out_height;
                out_frame->format = 0; // YUV420P
                out_frame->plane[0] = pkt->memory.data;
                out_frame->plane[1] = pkt->memory.data + out_width * out_height;
                out_frame->plane[2] = pkt->memory.data + out_width * out_height + (out_width * out_height) / 4;
                out_frame->stride[0] = out_width;
                out_frame->stride[1] = out_width / 2;
                out_frame->stride[2] = out_width / 2;

                pkt->pts = (zst_time_t)vbuf_cap.timestamp.tv_sec * 1000000000ULL + vbuf_cap.timestamp.tv_usec * 1000ULL;
                pkt->dts = pkt->pts;
                pkt->duration = in->duration;
                *out = pkt;
            }
            ioctl(s->fd, VIDIOC_QBUF, &vbuf_cap);
        }
    }

    return ZST_OK;
}

static zst_caps_t*
nv_video_scaler_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));

    return caps;
}

static zst_result_t
nv_video_scaler_set_property(zst_element_t* el, const char* name, const char* value)
{
    nv_video_scaler_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "width") == 0) {
        s->target_width = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        s->target_height = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0) {
        snprintf(s->target_pixel_format, sizeof(s->target_pixel_format), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
nv_video_scaler_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    nv_video_scaler_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->target_width);
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->target_height);
    } else if (strcmp(name, "pixel-format") == 0) {
        snprintf(value_out, max_len, "%s", s->target_pixel_format);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
nv_video_scaler_get_pool(zst_element_t* el)
{
    nv_video_scaler_t* s = el->priv;
    return s->pool;
}

static zst_result_t
nv_video_scaler_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t ret = nv_video_scaler_process(pad->parent, buf, &out);

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

static zst_element_ops_t g_nv_video_scaler_ops = {
    .name    = "nvvideoscaler",
    .open    = nv_video_scaler_open,
    .close   = nv_video_scaler_close,
    .start   = nv_video_scaler_start,
    .stop    = nv_video_scaler_stop,
    .process = nv_video_scaler_process,
    .get_caps = nv_video_scaler_get_caps,
    .set_property = nv_video_scaler_set_property,
    .get_property = nv_video_scaler_get_property,
    .get_pool = nv_video_scaler_get_pool
};

zst_element_t*
zst_nv_video_scaler_create(void)
{
    nv_video_scaler_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    zst_element_t* el = zst_element_create(&g_nv_video_scaler_ops, priv);
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
    priv->sinkpad->push = nv_video_scaler_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "nvvideoscaler") == 0) {
        return zst_nv_video_scaler_create();
    }
    return NULL;
}

static const zst_pad_template_t g_nvvideoscaler_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_nvvideoscaler_elements[] = {
    {
        .name = "nvvideoscaler",
        .long_name = "NVIDIA V4L2 Video Scaler",
        .category = "Filter/Video",
        .description = "Hardware video scaler and format converter using NV V4L2 extensions",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_nvvideoscaler_pads,
        .nb_pads = sizeof(g_nvvideoscaler_pads) / sizeof(g_nvvideoscaler_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "nvvideoscaler_plugin",
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
        *nb_elements_out = sizeof(g_nvvideoscaler_elements) / sizeof(g_nvvideoscaler_elements[0]);
    }
    return g_nvvideoscaler_elements;
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
