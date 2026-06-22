/*=============================================================================
    v4l2_source.c — V4L2 camera capture with mock synthetic fallback
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_v4l2_source.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

static void v4l2_buf_free(zst_buffer_t* buf);

typedef struct {
    int fd;
    int is_mock;
    uint32_t width;
    uint32_t height;
    uint64_t frame_count;
    
    struct {
        void* start;
        size_t length;
    } *buffers;
    uint32_t nb_buffers;

    zst_buffer_pool_t* pool;
} v4l2_source_t;

static void
yuyv_to_yuv420p(uint32_t width, uint32_t height, const uint8_t* yuyv, uint8_t* yuv420p)
{
    uint8_t* y = yuv420p;
    uint8_t* u = yuv420p + width * height;
    uint8_t* v = yuv420p + width * height + (width * height) / 4;

    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j += 2) {
            uint32_t yuyv_idx = (i * width + j) * 2;
            y[i * width + j] = yuyv[yuyv_idx];
            y[i * width + j + 1] = yuyv[yuyv_idx + 2];
            
            if (i % 2 == 0) {
                u[(i / 2) * (width / 2) + j / 2] = yuyv[yuyv_idx + 1];
                v[(i / 2) * (width / 2) + j / 2] = yuyv[yuyv_idx + 3];
            }
        }
    }
}

static zst_result_t
v4l2_open(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    s->width = 640;
    s->height = 480;
    s->frame_count = 0;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = s->width * s->height * 3 / 2, // YUV420P
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    s->fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        ZST_LOG_WARN("v4l2src", "Failed to open /dev/video0. Falling back to synthetic source.");
        s->is_mock = 1;
        return ZST_OK;
    }

    /* Configure format YUYV */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = s->width;
    fmt.fmt.pix.height = s->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) {
        ZST_LOG_WARN("v4l2src", "VIDIOC_S_FMT failed. Falling back to synthetic source.");
        close(s->fd);
        s->fd = -1;
        s->is_mock = 1;
        return ZST_OK;
    }

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) {
        ZST_LOG_WARN("v4l2src", "VIDIOC_REQBUFS failed. Falling back to synthetic source.");
        close(s->fd);
        s->fd = -1;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->buffers = calloc(req.count, sizeof(*s->buffers));
    s->nb_buffers = req.count;

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ZST_LOG_ERROR("v4l2src", "VIDIOC_QUERYBUF failed.");
            goto error;
        }
        s->buffers[i].length = buf.length;
        s->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, buf.m.offset);
        if (s->buffers[i].start == MAP_FAILED) {
            ZST_LOG_ERROR("v4l2src", "mmap failed.");
            goto error;
        }
    }

    /* Queue all buffers */
    for (uint32_t i = 0; i < s->nb_buffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(s->fd, VIDIOC_QBUF, &buf) < 0) {
            ZST_LOG_ERROR("v4l2src", "VIDIOC_QBUF failed.");
            goto error;
        }
    }

    s->is_mock = 0;
    return ZST_OK;

error:
    if (s->buffers) {
        for (uint32_t i = 0; i < s->nb_buffers; i++) {
            if (s->buffers[i].start && s->buffers[i].start != MAP_FAILED) {
                munmap(s->buffers[i].start, s->buffers[i].length);
            }
        }
        free(s->buffers);
        s->buffers = NULL;
    }
    close(s->fd);
    s->fd = -1;
    s->is_mock = 1;
    return ZST_OK;
}

static zst_result_t
v4l2_close(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        if (s->buffers) {
            for (uint32_t i = 0; i < s->nb_buffers; i++) {
                munmap(s->buffers[i].start, s->buffers[i].length);
            }
            free(s->buffers);
            s->buffers = NULL;
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
v4l2_start(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s->fd, VIDIOC_STREAMON, &type) < 0) {
            ZST_LOG_ERROR("v4l2src", "VIDIOC_STREAMON failed.");
            return ZST_ERROR;
        }
    }
    return ZST_OK;
}

static zst_result_t
v4l2_stop(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    if (!s->is_mock && s->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    return ZST_OK;
}

static zst_result_t
v4l2_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    v4l2_source_t* s = el->priv;

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        return ZST_ERROR;
    }

    size_t yuv_size = s->width * s->height * 3 / 2;
    uint8_t* raw_data = buf->memory.data;

    zst_video_frame_t* frame = buf->payload;
    if (!frame) {
        frame = calloc(1, sizeof(*frame));
        if (!frame) {
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
        buf->payload = frame;
        buf->destroy = v4l2_buf_free;
    }

    frame->width = s->width;
    frame->height = s->height;
    frame->format = 0; // YUV420P
    frame->plane[0] = raw_data;
    frame->plane[1] = raw_data + s->width * s->height;
    frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
    frame->stride[0] = s->width;
    frame->stride[1] = s->width / 2;
    frame->stride[2] = s->width / 2;

    if (s->is_mock) {
        /* Generate synthetic YUV420P frame (moving vertical bar) */
        uint8_t* y = raw_data;
        uint8_t* u = raw_data + s->width * s->height;
        uint8_t* v = raw_data + s->width * s->height + (s->width * s->height) / 4;

        memset(y, 128, s->width * s->height);
        memset(u, 128, (s->width * s->height) / 4);
        memset(v, 128, (s->width * s->height) / 4);

        int bar_pos = (s->frame_count * 8) % s->width;
        for (uint32_t r = 0; r < s->height; r++) {
            for (uint32_t c = bar_pos; c < bar_pos + 20 && c < s->width; c++) {
                y[r * s->width + c] = 235; // Bright white vertical bar
            }
        }

        /* Simulate 30 fps */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 33333333 };
        nanosleep(&ts, NULL);
    } else {
        /* Real V4L2 capture */
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        int res = poll(&pfd, 1, 200); // 200ms timeout
        if (res <= 0 || !(pfd.revents & POLLIN)) {
            /* Timeout or error, generate a blank/fallback frame */
            memset(raw_data, 16, yuv_size); // Black
        } else {
            struct v4l2_buffer vbuf = {0};
            vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            vbuf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s->fd, VIDIOC_DQBUF, &vbuf) < 0) {
                memset(raw_data, 16, yuv_size);
            } else {
                /* Convert YUYV to YUV420P */
                yuyv_to_yuv420p(s->width, s->height, s->buffers[vbuf.index].start, raw_data);
                ioctl(s->fd, VIDIOC_QBUF, &vbuf);
            }
        }
    }

    if (el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = s->frame_count * 33333333ULL; // 30 fps in nanoseconds
    }
    buf->duration = 33333333ULL;
    s->frame_count++;

    *out = buf;
    return ZST_OK;
}

static void
v4l2_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        // We only free payload since memory is managed by the allocator.
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    v4l2_source_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name    = "v4l2src",
    .open    = v4l2_open,
    .close   = v4l2_close,
    .start   = v4l2_start,
    .stop    = v4l2_stop,
    .process = v4l2_process,
    .get_pool = element_get_pool
};

zst_element_t*
zst_v4l2_source_create(void)
{
    zst_element_t* el;
    v4l2_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->fd = -1;

    el = zst_element_create(&g_ops, priv);
    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}



zst_element_t*
zst_v4l2_source_create_with_config(const zst_v4l2_source_config_t* config)
{
    (void)config;
    return zst_element_factory_make("v4l2src");
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "v4l2src") == 0) {
        return zst_v4l2_source_create();
    }
    return NULL;
}

static const zst_pad_template_t g_v4l2src_pads[] = {
    { "src", ZST_PAD_SRC, "video/x-raw" }
};

static const zst_element_desc_t g_v4l2src_elements[] = {
    {
        .name = "v4l2src",
        .long_name = "V4L2 Source",
        .category = "Source/Video",
        .description = "Captures video from a V4L2 device",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_v4l2src_pads,
        .nb_pads = sizeof(g_v4l2src_pads) / sizeof(g_v4l2src_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "v4l2source_plugin",
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
        *nb_elements_out = sizeof(g_v4l2src_elements) / sizeof(g_v4l2src_elements[0]);
    }
    return g_v4l2src_elements;
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