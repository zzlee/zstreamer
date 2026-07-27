
/*=============================================================================
    video_test_src.c
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_video_test_src.h"
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
    bool use_clock;
    bool real_time_pacing;

    bool has_base_time;
    zst_time_t base_time;

    uint64_t frame_count;
    zst_buffer_pool_t* pool;

    uint8_t* tmp_yuv420p_buf;
    size_t tmp_yuv420p_size;
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

static bool is_format_420_planar(const char* fmt) {
    return strcmp(fmt, "I420") == 0 || strcmp(fmt, "YUV420P") == 0 ||
           strcmp(fmt, "YV12") == 0;
}

static bool is_format_422_planar(const char* fmt) {
    return strcmp(fmt, "I422") == 0 || strcmp(fmt, "YUV422P") == 0;
}

static bool is_format_422_semi(const char* fmt) {
    return strcmp(fmt, "NV16") == 0;
}

static bool is_format_32bpp(const char* fmt) {
    return strcmp(fmt, "RGB32") == 0 || strcmp(fmt, "BGR32") == 0 ||
           strcmp(fmt, "RGBA") == 0 || strcmp(fmt, "BGRA") == 0 ||
           strcmp(fmt, "ARGB") == 0 || strcmp(fmt, "ABGR") == 0;
}

static bool is_format_422_packed(const char* fmt) {
    return strcmp(fmt, "YUYV") == 0 || strcmp(fmt, "YUY2") == 0 ||
           strcmp(fmt, "UYVY") == 0;
}

static zst_result_t video_test_src_open(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    s->frame_count = 0;

    size_t size;
    if (is_format_422_planar(s->pixel_format) || is_format_422_semi(s->pixel_format)) {
        size = s->width * s->height * 2;
    } else if (is_format_32bpp(s->pixel_format)) {
        size = s->width * s->height * 4;
    } else if (is_format_422_packed(s->pixel_format)) {
        size = s->width * s->height * 2;
    } else if (strcmp(s->pixel_format, "RGB24") == 0 || strcmp(s->pixel_format, "BGR24") == 0) {
        size = s->width * s->height * 3;
    } else {
        size = s->width * s->height * 3 / 2;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 32,
        .buffer_size = size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);

    s->tmp_yuv420p_size = s->width * s->height * 3 / 2;
    s->tmp_yuv420p_buf = malloc(s->tmp_yuv420p_size);
    if (!s->tmp_yuv420p_buf) {
        if (s->pool) {
            zst_buffer_pool_destroy(s->pool);
            s->pool = NULL;
        }
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t video_test_src_start(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    s->has_base_time = false;
    return ZST_OK;
}

static zst_result_t video_test_src_close(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->tmp_yuv420p_buf) {
        free(s->tmp_yuv420p_buf);
        s->tmp_yuv420p_buf = NULL;
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

static void yuv420p_to_nv12(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* y_out, uint8_t* uv_out)
{
    memcpy(y_out, y_in, width * height);
    uint32_t uv_pixels = (width * height) / 4;
    for (uint32_t i = 0; i < uv_pixels; i++) {
        uv_out[2 * i] = u_in[i];
        uv_out[2 * i + 1] = v_in[i];
    }
}

static void yuv420p_to_yuyv(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* yuyv_out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c += 2) {
            uint8_t y0 = y_in[r * width + c];
            uint8_t y1 = y_in[r * width + c + 1];
            uint8_t u  = u_in[(r / 2) * (width / 2) + (c / 2)];
            uint8_t v  = v_in[(r / 2) * (width / 2) + (c / 2)];

            uint32_t idx = 2 * (r * width + c);
            yuyv_out[idx]     = y0;
            yuyv_out[idx + 1] = u;
            yuyv_out[idx + 2] = y1;
            yuyv_out[idx + 3] = v;
        }
    }
}

static void yuv420p_to_rgb24(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* rgb_out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;

            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);

            rgb_out[3 * (r * width + c)]     = rd < 0 ? 0 : (rd > 255 ? 255 : rd);
            rgb_out[3 * (r * width + c) + 1] = gd < 0 ? 0 : (gd > 255 ? 255 : gd);
            rgb_out[3 * (r * width + c) + 2] = bd < 0 ? 0 : (bd > 255 ? 255 : bd);
        }
    }
}

static void yuv420p_to_bgr24(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* bgr_out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;

            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);

            bgr_out[3 * (r * width + c)]     = bd < 0 ? 0 : (bd > 255 ? 255 : bd);
            bgr_out[3 * (r * width + c) + 1] = gd < 0 ? 0 : (gd > 255 ? 255 : gd);
            bgr_out[3 * (r * width + c) + 2] = rd < 0 ? 0 : (rd > 255 ? 255 : rd);
        }
    }
}

static inline uint8_t clamp_u8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* YUV420P → I422 / YUV422P: full-width U/V planes, half height */
static void yuv420p_to_i422(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    uint8_t* y_out  = out;
    uint8_t* u_out  = out + width * height;
    uint8_t* v_out  = out + width * height + (width * height / 2);
    memcpy(y_out, y_in, width * height);
    for (uint32_t r = 0; r < height; r++) {
        uint32_t uv_row = r / 2;
        for (uint32_t c = 0; c < width; c += 2) {
            u_out[r * (width / 2) + c / 2] = u_in[uv_row * (width / 2) + c / 2];
            v_out[r * (width / 2) + c / 2] = v_in[uv_row * (width / 2) + c / 2];
        }
    }
}

/* YUV420P → NV16: Y plane + interleaved UV, both full width */
static void yuv420p_to_nv16(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* y_out, uint8_t* uv_out)
{
    memcpy(y_out, y_in, width * height);
    for (uint32_t r = 0; r < height; r++) {
        uint32_t uv_row = r / 2;
        for (uint32_t c = 0; c < width; c += 2) {
            uint32_t pair = uv_row * (width / 2) + c / 2;
            uv_out[r * width + c]     = u_in[pair];
            uv_out[r * width + c + 1] = v_in[pair];
        }
    }
}

/* YUV420P → BGRA (AV_PIX_FMT_BGRA): B,G,R,A byte order, a.k.a. RGB32 */
static void yuv420p_to_bgra(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);
            uint32_t idx = 4 * (r * width + c);
            out[idx]     = clamp_u8(bd);  /* B */
            out[idx + 1] = clamp_u8(gd);  /* G */
            out[idx + 2] = clamp_u8(rd);  /* R */
            out[idx + 3] = 255;            /* A */
        }
    }
}

/* YUV420P → RGBA (AV_PIX_FMT_RGBA): R,G,B,A byte order, a.k.a. BGR32 */
static void yuv420p_to_rgba(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);
            uint32_t idx = 4 * (r * width + c);
            out[idx]     = clamp_u8(rd);  /* R */
            out[idx + 1] = clamp_u8(gd);  /* G */
            out[idx + 2] = clamp_u8(bd);  /* B */
            out[idx + 3] = 255;            /* A */
        }
    }
}

/* YUV420P → ARGB (AV_PIX_FMT_ARGB): A,R,G,B byte order */
static void yuv420p_to_argb(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);
            uint32_t idx = 4 * (r * width + c);
            out[idx]     = 255;            /* A */
            out[idx + 1] = clamp_u8(rd);  /* R */
            out[idx + 2] = clamp_u8(gd);  /* G */
            out[idx + 3] = clamp_u8(bd);  /* B */
        }
    }
}

/* YUV420P → ABGR (AV_PIX_FMT_ABGR): A,B,G,R byte order */
static void yuv420p_to_abgr(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c++) {
            int y = y_in[r * width + c];
            int u = u_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int v = v_in[(r / 2) * (width / 2) + (c / 2)] - 128;
            int rd = y + (int)(1.402 * v);
            int gd = y - (int)(0.344136 * u + 0.714136 * v);
            int bd = y + (int)(1.772 * u);
            uint32_t idx = 4 * (r * width + c);
            out[idx]     = 255;            /* A */
            out[idx + 1] = clamp_u8(bd);  /* B */
            out[idx + 2] = clamp_u8(gd);  /* G */
            out[idx + 3] = clamp_u8(rd);  /* R */
        }
    }
}

/* YUV420P → UYVY: packed YUV 4:2:2, U,Y0,V,Y1 byte order */
static void yuv420p_to_uyvy(uint32_t width, uint32_t height, const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in, uint8_t* out)
{
    for (uint32_t r = 0; r < height; r++) {
        for (uint32_t c = 0; c < width; c += 2) {
            uint8_t y0 = y_in[r * width + c];
            uint8_t y1 = y_in[r * width + c + 1];
            uint8_t u  = u_in[(r / 2) * (width / 2) + (c / 2)];
            uint8_t v  = v_in[(r / 2) * (width / 2) + (c / 2)];
            uint32_t idx = 2 * (r * width + c);
            out[idx]     = u;
            out[idx + 1] = y0;
            out[idx + 2] = v;
            out[idx + 3] = y1;
        }
    }
}

static zst_result_t video_test_src_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    video_test_src_t* s = el->priv;

    if (s->num_buffers != -1 && s->frame_count >= (uint64_t)s->num_buffers) {
        if (s->loop) {
            s->frame_count = 0;
        } else {
            return ZST_EOF;
        }
    }

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        ZST_LOG_ERROR("videotestsrc", "failed to allocate buffer from pool");
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

    uint8_t* y_plane = s->tmp_yuv420p_buf;
    uint8_t* u_plane = s->tmp_yuv420p_buf + s->width * s->height;
    uint8_t* v_plane = s->tmp_yuv420p_buf + s->width * s->height + (s->width * s->height) / 4;

    switch (s->pattern) {
        case PATTERN_BARS: render_bars(s, y_plane, u_plane, v_plane); break;
        case PATTERN_GRADIENT: render_gradient(s, y_plane, u_plane, v_plane); break;
        case PATTERN_CHECKERBOARD: render_checkerboard(s, y_plane, u_plane, v_plane); break;
        case PATTERN_NOISE: render_noise(s, y_plane, u_plane, v_plane); break;
        case PATTERN_BLACK: render_black(s, y_plane, u_plane, v_plane); break;
    }

    frame->width = s->width;
    frame->height = s->height;

    if (strcmp(s->pixel_format, "NV12") == 0) {
        frame->format = 23; // AV_PIX_FMT_NV12
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_nv12(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0], frame->plane[1]);
    } else if (is_format_422_planar(s->pixel_format)) {
        /* I422 / YUV422P */
        frame->format = 32; // AV_PIX_FMT_YUV422P
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + s->width * s->height / 2;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;
        frame->stride[3] = 0;
        yuv420p_to_i422(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (is_format_422_semi(s->pixel_format)) {
        /* NV16 */
        frame->format = 33; // AV_PIX_FMT_NV16
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_nv16(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0], frame->plane[1]);
    } else if (strcmp(s->pixel_format, "RGB24") == 0) {
        frame->format = 2; // AV_PIX_FMT_RGB24
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 3;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_rgb24(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "BGR24") == 0) {
        frame->format = 3; // AV_PIX_FMT_BGR24
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 3;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_bgr24(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "BGRA") == 0 || strcmp(s->pixel_format, "RGB32") == 0) {
        frame->format = 28; // AV_PIX_FMT_BGRA
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 4;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_bgra(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "RGBA") == 0 || strcmp(s->pixel_format, "BGR32") == 0) {
        frame->format = 26; // AV_PIX_FMT_RGBA
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 4;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_rgba(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "ARGB") == 0) {
        frame->format = 25; // AV_PIX_FMT_ARGB
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 4;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_argb(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "ABGR") == 0) {
        frame->format = 27; // AV_PIX_FMT_ABGR
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 4;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_abgr(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "YUYV") == 0 || strcmp(s->pixel_format, "YUY2") == 0) {
        frame->format = 1; // AV_PIX_FMT_YUYV422
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 2;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_yuyv(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (strcmp(s->pixel_format, "UYVY") == 0) {
        frame->format = 17; // AV_PIX_FMT_UYVY422
        frame->plane[0] = raw_data;
        frame->plane[1] = NULL;
        frame->plane[2] = NULL;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width * 2;
        frame->stride[1] = 0;
        frame->stride[2] = 0;
        frame->stride[3] = 0;
        yuv420p_to_uyvy(s->width, s->height, y_plane, u_plane, v_plane, frame->plane[0]);
    } else if (is_format_420_planar(s->pixel_format)) {
        /* I420 / YUV420P / YV12 fallback */
        frame->format = 0; // AV_PIX_FMT_YUV420P
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;
        frame->stride[3] = 0;
        memcpy(raw_data, s->tmp_yuv420p_buf, s->tmp_yuv420p_size);
    } else {
        /* Unknown format fallback — output as YUV420P */
        frame->format = 0; // AV_PIX_FMT_YUV420P
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
        frame->plane[3] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;
        frame->stride[3] = 0;
        memcpy(raw_data, s->tmp_yuv420p_buf, s->tmp_yuv420p_size);
    }

    uint64_t dur_ns = 1000000000ULL / s->fps;
    if (s->use_clock && el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = s->frame_count * dur_ns;
    }
    buf->dts = buf->pts;
    buf->duration = dur_ns;

    if (s->real_time_pacing && el->clock) {
        zst_time_t current_time = zst_clock_get_time(el->clock);
        if (!s->has_base_time) {
            s->base_time = current_time;
            s->has_base_time = true;
        }
        zst_time_t expected_time = s->base_time + s->frame_count * dur_ns;
        if (expected_time > current_time) {
            zst_clock_wait(el->clock, expected_time - current_time);
        }
    }

    s->frame_count++;
    *out = buf;

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
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        s->use_clock = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        s->real_time_pacing = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
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
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        strncpy(value_out, s->use_clock ? "true" : "false", max_len);
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        strncpy(value_out, s->real_time_pacing ? "true" : "false", max_len);
        return ZST_OK;
    }
    return ZST_ERROR;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    video_test_src_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "videotestsrc",
    .open = video_test_src_open,
    .close = video_test_src_close,
    .start = video_test_src_start,
    .process = video_test_src_process,
    .get_caps = video_test_src_get_caps,
    .set_property = video_test_src_set_property,
    .get_property = video_test_src_get_property,
    .get_pool = element_get_pool
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
    priv->use_clock = false;
    priv->real_time_pacing = false;
    priv->has_base_time = false;
    priv->frame_count = 0;

    el = zst_element_create(&g_ops, priv);

    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}

zst_element_t*
zst_video_test_src_create_with_config(const zst_video_test_src_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_video_test_src_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("videotestsrc");
    if (!el) return NULL;

    if (config->width > 0) {
        zst_element_set_property_uint(el, "width", config->width);
    }
    if (config->height > 0) {
        zst_element_set_property_uint(el, "height", config->height);
    }
    if (config->fps > 0) {
        zst_element_set_property_uint(el, "fps", config->fps);
    }
    if (config->pattern) {
        zst_element_set_property_string(el, "pattern", config->pattern);
    }
    if (config->pixel_format) {
        zst_element_set_property_string(el, "pixel-format", config->pixel_format);
    }
    zst_element_set_property_int(el, "num-buffers", config->num_buffers);
    zst_element_set_property_bool(el, "loop", config->loop);
    zst_element_set_property_bool(el, "use-clock", config->use_clock);
    zst_element_set_property_bool(el, "real-time-pacing", config->real_time_pacing);

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

static const zst_pad_template_t g_videotestsrc_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" }
};

static const zst_element_desc_t g_videotestsrc_elements[] = {
    {
        .name = "videotestsrc",
        .long_name = "Video Test Source",
        .category = "Source/Test",
        .description = "Generates synthetic video test patterns",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_videotestsrc_pads,
        .nb_pads = sizeof(g_videotestsrc_pads) / sizeof(g_videotestsrc_pads[0]),
        .create = NULL
    }
};

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
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_videotestsrc_elements) / sizeof(g_videotestsrc_elements[0]);
    }
    return g_videotestsrc_elements;
}

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
