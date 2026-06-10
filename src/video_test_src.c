
/*=============================================================================
    video_test_src.c
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

typedef enum {
    PATTERN_BARS,
    PATTERN_GRADIENT,
    PATTERN_CHECKERBOARD,
    PATTERN_NOISE,
    PATTERN_BLACK
} pattern_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    pattern_t pattern;
    char pixel_format[32];
    int num_buffers;
    bool loop;

    uint64_t frame_count;
    zst_time_t anchor_time;
    zst_buffer_pool_t* pool;
} video_test_src_t;

static void video_test_src_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}

static zst_result_t video_test_src_open(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    s->frame_count = 0;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = s->width * s->height * 3 / 2, // YUV420P
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    return ZST_OK;
}

static zst_result_t video_test_src_close(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static void get_bar_color(int bar, uint8_t *y, uint8_t *u, uint8_t *v) {
    int r=0, g=0, b=0;
    switch(bar) {
        case 0: r=255; g=255; b=255; break; // White
        case 1: r=255; g=255; b=0;   break; // Yellow
        case 2: r=0;   g=255; b=255; break; // Cyan
        case 3: r=0;   g=255; b=0;   break; // Green
        case 4: r=255; g=0;   b=255; break; // Magenta
        case 5: r=255; g=0;   b=0;   break; // Red
        case 6: r=0;   g=0;   b=255; break; // Blue
        case 7: r=0;   g=0;   b=0;   break; // Black
    }
    *y = (uint8_t)( 0.299*r + 0.587*g + 0.114*b);
    *u = (uint8_t)(-0.169*r - 0.331*g + 0.500*b + 128);
    *v = (uint8_t)( 0.500*r - 0.419*g - 0.081*b + 128);
}

static void render_bars(video_test_src_t* s, uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) {
    int bar_width = s->width / 8;
    for (uint32_t c = 0; c < s->width; c++) {
        int bar = (c / bar_width);
        if (bar > 7) bar = 7;
        uint8_t cy, cu, cv;
        get_bar_color(bar, &cy, &cu, &cv);
        for (uint32_t r = 0; r < s->height; r++) {
            y_plane[r * s->width + c] = cy;
            if (r % 2 == 0 && c % 2 == 0) {
                u_plane[(r / 2) * (s->width / 2) + (c / 2)] = cu;
                v_plane[(r / 2) * (s->width / 2) + (c / 2)] = cv;
            }
        }
    }
}

static void render_gradient(video_test_src_t* s, uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) {
    int shift = s->frame_count % s->width;
    for (uint32_t c = 0; c < s->width; c++) {
        uint8_t cy = (uint8_t)(((c + shift) % s->width) * 255 / s->width);
        for (uint32_t r = 0; r < s->height; r++) {
            y_plane[r * s->width + c] = cy;
            if (r % 2 == 0 && c % 2 == 0) {
                u_plane[(r / 2) * (s->width / 2) + (c / 2)] = 128;
                v_plane[(r / 2) * (s->width / 2) + (c / 2)] = 128;
            }
        }
    }
}

static void render_checkerboard(video_test_src_t* s, uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) {
    for (uint32_t r = 0; r < s->height; r++) {
        for (uint32_t c = 0; c < s->width; c++) {
            int block_r = r / 8;
            int block_c = c / 8;
            uint8_t cy = ((block_r + block_c) % 2 == 0) ? 255 : 0;
            y_plane[r * s->width + c] = cy;
            if (r % 2 == 0 && c % 2 == 0) {
                u_plane[(r / 2) * (s->width / 2) + (c / 2)] = 128;
                v_plane[(r / 2) * (s->width / 2) + (c / 2)] = 128;
            }
        }
    }
}

static void render_noise(video_test_src_t* s, uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) {
    for (uint32_t i = 0; i < s->width * s->height; i++) {
        y_plane[i] = rand() % 256;
    }
    for (uint32_t i = 0; i < (s->width * s->height) / 4; i++) {
        u_plane[i] = rand() % 256;
        v_plane[i] = rand() % 256;
    }
}

static void render_black(video_test_src_t* s, uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane) {
    memset(y_plane, 0, s->width * s->height);
    memset(u_plane, 128, (s->width * s->height) / 4);
    memset(v_plane, 128, (s->width * s->height) / 4);
}

static zst_result_t video_test_src_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    video_test_src_t* s = el->priv;

    if (s->num_buffers != -1 && s->frame_count >= (uint64_t)s->num_buffers) {
        if (s->loop) {
            s->frame_count = 0;
        } else {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
            if (eos_buf) eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
    }

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        return ZST_ERROR;
    }

    uint8_t* raw_data = buf->memory.data;

    zst_video_frame_t* frame = buf->payload;
    if (!frame) {
        frame = calloc(1, sizeof(*frame));
        if (!frame) {
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
        buf->payload = frame;
        buf->destroy = video_test_src_buf_free;
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

    uint8_t* y_plane = frame->plane[0];
    uint8_t* u_plane = frame->plane[1];
    uint8_t* v_plane = frame->plane[2];

    switch (s->pattern) {
        case PATTERN_BARS: render_bars(s, y_plane, u_plane, v_plane); break;
        case PATTERN_GRADIENT: render_gradient(s, y_plane, u_plane, v_plane); break;
        case PATTERN_CHECKERBOARD: render_checkerboard(s, y_plane, u_plane, v_plane); break;
        case PATTERN_NOISE: render_noise(s, y_plane, u_plane, v_plane); break;
        case PATTERN_BLACK: render_black(s, y_plane, u_plane, v_plane); break;
    }

    uint64_t dur_ns = 1000000000ULL / s->fps; // Nanoseconds as requested

        buf->pts = s->frame_count * dur_ns;
    buf->duration = dur_ns;

    if (el->clock) {
        if (s->frame_count == 0) {
            s->anchor_time = zst_clock_get_time(el->clock);
        }

        zst_time_t expected_time = s->anchor_time + buf->pts;
        zst_time_t current_time = zst_clock_get_time(el->clock);

        if (expected_time > current_time) {
            zst_clock_wait(el->clock, expected_time - current_time);
        }
    }

    s->frame_count++;
    *out = buf;

    /* simulate fps spacing? Not strictly required unless real-time requested, but mock v4l2src did it.
       we can leave it out or add real time wait */
    // struct timespec ts = { .tv_sec = 0, .tv_nsec = dur_ns };
    // nanosleep(&ts, NULL);
    return ZST_OK;
}

static zst_caps_t* video_test_src_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    (void)pad;
    video_test_src_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", s->width, s->height, (double)s->fps, s->pixel_format));
    return caps;
}

static zst_result_t video_test_src_set_property(zst_element_t* el, const char* name, const char* value)
{
    video_test_src_t* s = el->priv;
    if (strcmp(name, "width") == 0) {
        s->width = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        s->height = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        s->fps = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "pattern") == 0) {
        if (strcmp(value, "bars") == 0) s->pattern = PATTERN_BARS;
        else if (strcmp(value, "gradient") == 0) s->pattern = PATTERN_GRADIENT;
        else if (strcmp(value, "checkerboard") == 0) s->pattern = PATTERN_CHECKERBOARD;
        else if (strcmp(value, "noise") == 0) s->pattern = PATTERN_NOISE;
        else if (strcmp(value, "black") == 0) s->pattern = PATTERN_BLACK;
        else return ZST_ERROR;
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0) {
        strncpy(s->pixel_format, value, sizeof(s->pixel_format) - 1);
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0) {
        s->num_buffers = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        s->loop = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t video_test_src_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    video_test_src_t* s = el->priv;
    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->width);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->height);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        snprintf(value_out, max_len, "%u", s->fps);
        return ZST_OK;
    } else if (strcmp(name, "pattern") == 0) {
        switch (s->pattern) {
            case PATTERN_BARS: strncpy(value_out, "bars", max_len); break;
            case PATTERN_GRADIENT: strncpy(value_out, "gradient", max_len); break;
            case PATTERN_CHECKERBOARD: strncpy(value_out, "checkerboard", max_len); break;
            case PATTERN_NOISE: strncpy(value_out, "noise", max_len); break;
            case PATTERN_BLACK: strncpy(value_out, "black", max_len); break;
        }
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0) {
        strncpy(value_out, s->pixel_format, max_len);
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0) {
        snprintf(value_out, max_len, "%d", s->num_buffers);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        strncpy(value_out, s->loop ? "true" : "false", max_len);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "videotestsrc",
    .open = video_test_src_open,
    .close = video_test_src_close,
    .process = video_test_src_process,
    .get_caps = video_test_src_get_caps,
    .set_property = video_test_src_set_property,
    .get_property = video_test_src_get_property,
};

zst_element_t* zst_video_test_src_create(void)
{
    zst_element_t* el;
    video_test_src_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->width = 640;
    priv->height = 480;
    priv->fps = 30;
    priv->pattern = PATTERN_BARS;
    strcpy(priv->pixel_format, "YUV420P");
    priv->num_buffers = -1;
    priv->loop = false;
    priv->frame_count = 0;

    el = zst_element_create(&g_ops, priv);

    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    if (strcmp(name, "videotestsrc") == 0) {
        return zst_video_test_src_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "videotestsrc_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
