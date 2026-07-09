/*=============================================================================
    ipp_comp_sink.c — Intel IPP software compositor sink element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>

#include <ipp.h>
#include <ippi.h>
#include <ippcc.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_ipp_comp_sink.h"
#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_bus.h"
#include "zst_log.h"
#include "zst_pipeline.h"
#include "zst_queue.h"

#define IPPCOMP_MAX_NAME 32

typedef enum {
    IPPCOMP_FMT_YUV420P,
    IPPCOMP_FMT_NV12,
    IPPCOMP_FMT_RGB,
    IPPCOMP_FMT_UNKNOWN
} ippcomp_fmt_t;

typedef struct ipp_comp_sink ipp_comp_sink_t;

typedef struct {
    ipp_comp_sink_t* parent;
    zst_pad_t* pad;
    char name[IPPCOMP_MAX_NAME];

    int x;
    int y;
    uint32_t width;
    uint32_t height;
    int z_order;
    double alpha;
    int visible;
    char scaling[16];
    uint32_t border_width;
    float border_color[4];

    zst_queue_t* queue;
    zst_buffer_t* latest;
    uint64_t frame_count;
    int eos;
} ipp_comp_input_t;

typedef enum {
    CAPTURE_IDLE,
    CAPTURE_REQUESTED,
    CAPTURE_DONE
} capture_state_t;

struct ipp_comp_sink {
    char window_title[256];
    uint32_t canvas_width;
    uint32_t canvas_height;
    float bg[4];
    int fullscreen;
    int vsync;
    int null_mode;
    int window_open;

    int64_t max_lateness;
    double display_rate;

    uint8_t* canvas_rgba;
    size_t canvas_size;

    ipp_comp_input_t** inputs;
    uint32_t nb_inputs;
    uint32_t next_pad_index;
    uint64_t composite_count;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t worker_thread;
    bool running;

    /* Capture request */
    uint8_t* capture_buf;
    uint32_t capture_w;
    uint32_t capture_h;
    volatile capture_state_t capture_state;
    pthread_cond_t capture_cond;
};

static ippcomp_fmt_t
comp_detect_format(const zst_video_frame_t* frame)
{
    if (!frame || !frame->plane[0]) return IPPCOMP_FMT_UNKNOWN;
    if (frame->format == 0 && frame->plane[1] && frame->plane[2] && !frame->plane[3]) return IPPCOMP_FMT_YUV420P;
    if ((frame->format == 1 || frame->format == 12) && frame->plane[1] && !frame->plane[2]) return IPPCOMP_FMT_NV12;
    if ((frame->format == 2 || frame->format == 24) && !frame->plane[1]) return IPPCOMP_FMT_RGB;
    if (frame->plane[1] && frame->plane[2] && !frame->plane[3]) return IPPCOMP_FMT_YUV420P;
    if (frame->plane[1] && !frame->plane[2]) return IPPCOMP_FMT_NV12;
    if (!frame->plane[1]) return IPPCOMP_FMT_RGB;
    return IPPCOMP_FMT_UNKNOWN;
}

static void
comp_pixel_rect(uint32_t canvas_w, uint32_t canvas_h,
                const ipp_comp_input_t* in, uint32_t img_w, uint32_t img_h,
                int* dx, int* dy, int* dw, int* dh,
                int* sx, int* sy, int* sw, int* sh)
{
    float x = (float)in->x;
    float y = (float)in->y;
    float w = (float)(in->width ? in->width : canvas_w);
    float h = (float)(in->height ? in->height : canvas_h);
    float tx0 = 0.0f, ty0 = 0.0f, tx1 = 1.0f, ty1 = 1.0f;

    if (img_w > 0 && img_h > 0 && w > 0 && h > 0) {
        float img_aspect = (float)img_w / (float)img_h;
        float dst_aspect = w / h;
        if (strcmp(in->scaling, "fit") == 0) {
            if (img_aspect > dst_aspect) {
                float nh = w / img_aspect;
                y += (h - nh) * 0.5f;
                h = nh;
            } else {
                float nw = h * img_aspect;
                x += (w - nw) * 0.5f;
                w = nw;
            }
        } else if (strcmp(in->scaling, "crop") == 0) {
            if (img_aspect > dst_aspect) {
                float vis = dst_aspect / img_aspect;
                tx0 = (1.0f - vis) * 0.5f;
                tx1 = 1.0f - tx0;
            } else {
                float vis = img_aspect / dst_aspect;
                ty0 = (1.0f - vis) * 0.5f;
                ty1 = 1.0f - ty0;
            }
        }
    }

    *dx = (int)(x + 0.5f);
    *dy = (int)(y + 0.5f);
    *dw = (int)(w + 0.5f);
    *dh = (int)(h + 0.5f);
    *sx = (int)(tx0 * img_w + 0.5f);
    *sy = (int)(ty0 * img_h + 0.5f);
    *sw = (int)((tx1 - tx0) * img_w + 0.5f);
    *sh = (int)((ty1 - ty0) * img_h + 0.5f);
}

static void
comp_draw_rect(ipp_comp_sink_t* s, int x, int y, int w, int h, float color[4])
{
    if (x >= (int)s->canvas_width || y >= (int)s->canvas_height || x + w <= 0 || y + h <= 0) return;
    int cx = x < 0 ? 0 : x;
    int cy = y < 0 ? 0 : y;
    int cw = x + w > (int)s->canvas_width ? (int)s->canvas_width - cx : (x + w) - cx;
    int ch = y + h > (int)s->canvas_height ? (int)s->canvas_height - cy : (y + h) - cy;

    uint8_t c[4] = {
        (uint8_t)(color[0] * 255.0f),
        (uint8_t)(color[1] * 255.0f),
        (uint8_t)(color[2] * 255.0f),
        (uint8_t)(color[3] * 255.0f)
    };

    IppiSize roi = { cw, ch };
    uint8_t* ptr = s->canvas_rgba + cy * s->canvas_width * 4 + cx * 4;
    ippiSet_8u_C4R(c, ptr, s->canvas_width * 4, roi);
}

static void
comp_upload_and_draw(ipp_comp_sink_t* s, const ipp_comp_input_t* in)
{
    if (!in->latest || !in->visible) return;
    zst_video_frame_t* frame = (zst_video_frame_t*)in->latest->payload;
    if (!frame || frame->width == 0 || frame->height == 0) return;

    ippcomp_fmt_t fmt = comp_detect_format(frame);

    int dx, dy, dw, dh, sx, sy, sw, sh;
    comp_pixel_rect(s->canvas_width, s->canvas_height, in, frame->width, frame->height,
                    &dx, &dy, &dw, &dh, &sx, &sy, &sw, &sh);

    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    /* Draw border */
    if (in->border_width > 0) {
        int bw = (int)in->border_width;
        float* bc = (float*)in->border_color;
        comp_draw_rect(s, dx - bw, dy - bw, dw + 2*bw, bw, bc); /* Top */
        comp_draw_rect(s, dx - bw, dy + dh, dw + 2*bw, bw, bc); /* Bottom */
        comp_draw_rect(s, dx - bw, dy, bw, dh, bc); /* Left */
        comp_draw_rect(s, dx + dw, dy, bw, dh, bc); /* Right */
    }

    if (dx >= (int)s->canvas_width || dy >= (int)s->canvas_height || dx + dw <= 0 || dy + dh <= 0) return;

    /* Create intermediate RGB buffer for format conversion */
    int rgb_step = sw * 3;
    uint8_t* rgb_buf = malloc(sh * rgb_step);
    if (!rgb_buf) return;

    IppiSizeL roiL = { sw, sh };
    IppiSize roi = { sw, sh };

    if (fmt == IPPCOMP_FMT_YUV420P) {
        const Ipp8u* pSrc[3] = {
            frame->plane[0] + sy * frame->stride[0] + sx,
            frame->plane[1] + (sy / 2) * frame->stride[1] + (sx / 2),
            frame->plane[2] + (sy / 2) * frame->stride[2] + (sx / 2)
        };
        int srcStep[3] = { frame->stride[0], frame->stride[1], frame->stride[2] };
        ippiYCbCr420ToRGB_8u_P3C3R(pSrc, srcStep, rgb_buf, rgb_step, roi);
    } else if (fmt == IPPCOMP_FMT_NV12) {
        const Ipp8u* pSrcY = frame->plane[0] + sy * frame->stride[0] + sx;
        const Ipp8u* pSrcCbCr = frame->plane[1] + (sy / 2) * frame->stride[1] + (sx & ~1);
        ippiYCbCr420ToRGB_8u_P2C3R(pSrcY, frame->stride[0], pSrcCbCr, frame->stride[1], rgb_buf, rgb_step, roi);
    } else if (fmt == IPPCOMP_FMT_RGB) {
        const Ipp8u* pSrc = frame->plane[0] + sy * frame->stride[0] + sx * 3;
        ippiCopy_8u_C3R(pSrc, frame->stride[0], rgb_buf, rgb_step, roi);
    } else {
        free(rgb_buf);
        return;
    }

    /* Resize RGB to target size */
    int r_rgb_step = dw * 3;
    uint8_t* r_rgb_buf = malloc(dh * r_rgb_step);
    if (!r_rgb_buf) { free(rgb_buf); return; }

    IppiSizeL dstSize = { dw, dh };
    IppSizeL specSize = 0, initBufSize = 0;
    ippiResizeGetSize_L(roiL, dstSize, ipp8u, ippLinear, 0, &specSize, &initBufSize);

    IppiResizeSpec* pSpec = (IppiResizeSpec*)malloc(specSize);
    uint8_t* pInitBuf = initBufSize > 0 ? malloc(initBufSize) : NULL;

    ippiResizeLinearInit_L(roiL, dstSize, ipp8u, pSpec);

    IppSizeL workBufSize = 0;
    ippiResizeGetBufferSize_L(pSpec, dstSize, 3, &workBufSize);
    uint8_t* pWorkBuf = workBufSize > 0 ? malloc(workBufSize) : NULL;

    IppiPointL dstOffset = { 0, 0 };
    ippiResizeLinear_8u_C3R_L(rgb_buf, rgb_step, r_rgb_buf, r_rgb_step, dstOffset, dstSize, ippBorderRepl, NULL, pSpec, pWorkBuf);

    if (pWorkBuf) free(pWorkBuf);
    if (pInitBuf) free(pInitBuf);
    free(pSpec);
    free(rgb_buf);

    /* Convert to RGBA and apply alpha */
    int rgba_step = dw * 4;
    uint8_t* rgba_buf = malloc(dh * rgba_step);
    if (rgba_buf) {
        uint8_t alpha_val = (uint8_t)(in->alpha * 255.0);
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                rgba_buf[y * rgba_step + x * 4 + 0] = r_rgb_buf[y * r_rgb_step + x * 3 + 0];
                rgba_buf[y * rgba_step + x * 4 + 1] = r_rgb_buf[y * r_rgb_step + x * 3 + 1];
                rgba_buf[y * rgba_step + x * 4 + 2] = r_rgb_buf[y * r_rgb_step + x * 3 + 2];
                rgba_buf[y * rgba_step + x * 4 + 3] = alpha_val;
            }
        }

        /* Blend onto canvas using alpha composite */
        int cx = dx < 0 ? 0 : dx;
        int cy = dy < 0 ? 0 : dy;
        int cw = dx + dw > (int)s->canvas_width ? (int)s->canvas_width - cx : (dx + dw) - cx;
        int ch = dy + dh > (int)s->canvas_height ? (int)s->canvas_height - cy : (dy + dh) - cy;

        int off_x = cx - dx;
        int off_y = cy - dy;

        IppiSize comp_roi = { cw, ch };
        uint8_t* src_ptr = rgba_buf + off_y * rgba_step + off_x * 4;
        uint8_t* dst_ptr = s->canvas_rgba + cy * s->canvas_width * 4 + cx * 4;

        ippiAlphaComp_8u_AC4R(src_ptr, rgba_step, dst_ptr, s->canvas_width * 4, dst_ptr, s->canvas_width * 4, comp_roi, ippAlphaOver);

        free(rgba_buf);
    }
    free(r_rgb_buf);
}

static int
comp_input_cmp(const void* a, const void* b)
{
    const ipp_comp_input_t* ia = *(const ipp_comp_input_t* const*)a;
    const ipp_comp_input_t* ib = *(const ipp_comp_input_t* const*)b;
    if (ia->z_order != ib->z_order) return ia->z_order - ib->z_order;
    return strcmp(ia->name, ib->name);
}

static zst_result_t
comp_render_locked(ipp_comp_sink_t* s, zst_element_t* el)
{
    if (s->null_mode || !s->canvas_rgba) {
        s->composite_count++;
        return ZST_OK;
    }

    /* Clear background */
    uint8_t bg[4] = {
        (uint8_t)(s->bg[0] * 255.0f),
        (uint8_t)(s->bg[1] * 255.0f),
        (uint8_t)(s->bg[2] * 255.0f),
        (uint8_t)(s->bg[3] * 255.0f)
    };
    IppiSize roi = { s->canvas_width, s->canvas_height };
    ippiSet_8u_C4R(bg, s->canvas_rgba, s->canvas_width * 4, roi);

    if (s->nb_inputs > 0) {
        ipp_comp_input_t** sorted = calloc(s->nb_inputs, sizeof(*sorted));
        if (sorted) {
            memcpy(sorted, s->inputs, s->nb_inputs * sizeof(*sorted));
            qsort(sorted, s->nb_inputs, sizeof(*sorted), comp_input_cmp);

            for (uint32_t i = 0; i < s->nb_inputs; i++) {
                ipp_comp_input_t* in = sorted[i];
                if (!in) continue;

                zst_buffer_t* next = NULL;
                while (zst_queue_pop(in->queue, &next, 0) == ZST_OK) {
                    if (in->latest) zst_buffer_unref(in->latest);
                    in->latest = next;
                }
                comp_upload_and_draw(s, in);
            }
            free(sorted);
        }
    }

    s->composite_count++;
    if (s->composite_count % 30 == 0) {
        ZST_LOG_INFO("ippcompsink", "rendered %llu compositor frames", (unsigned long long)s->composite_count);
    }
    return (!s->window_open && !s->null_mode) ? ZST_EOF : ZST_OK;
}

static void*
ippcomp_worker_thread(void* arg)
{
    zst_element_t* el = arg;
    ipp_comp_sink_t* s = el->priv;

    ZST_LOG_INFO("ippcompsink", "starting IPP compositor worker thread (rate=%.2f fps)", s->display_rate);

    pthread_mutex_lock(&s->lock);

    struct timespec next_render;
    clock_gettime(CLOCK_REALTIME, &next_render);

    while (s->running) {
        double interval = 1.0 / s->display_rate;
        long long nsec = next_render.tv_nsec + (long long)(interval * 1000000000.0);
        next_render.tv_sec += nsec / 1000000000LL;
        next_render.tv_nsec = nsec % 1000000000LL;

        if (s->capture_state != CAPTURE_REQUESTED) {
            while (s->running) {
                int res = pthread_cond_timedwait(&s->cond, &s->lock, &next_render);
                if (res == ETIMEDOUT) break;
                if (res != 0) break;
            }
        }

        if (!s->running) break;

        if (comp_render_locked(s, el) == ZST_EOF) {
            s->null_mode = 1;
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_eos(el));
            }
        }

        if (s->capture_state == CAPTURE_REQUESTED && s->canvas_rgba && !s->null_mode) {
            /* If capture size matches canvas size, just copy.
             * Otherwise, resize the canvas to the capture buffer.
             */
            if (s->capture_w == s->canvas_width && s->capture_h == s->canvas_height) {
                memcpy(s->capture_buf, s->canvas_rgba, s->canvas_width * s->canvas_height * 4);
            } else {
                IppiSizeL srcSize = { s->canvas_width, s->canvas_height };
                IppiSizeL dstSize = { s->capture_w, s->capture_h };
                IppSizeL specSize = 0, initBufSize = 0;
                ippiResizeGetSize_L(srcSize, dstSize, ipp8u, ippLinear, 0, &specSize, &initBufSize);
                IppiResizeSpec* pSpec = (IppiResizeSpec*)malloc(specSize);
                uint8_t* pInitBuf = initBufSize > 0 ? malloc(initBufSize) : NULL;
                ippiResizeLinearInit_L(srcSize, dstSize, ipp8u, pSpec);
                IppSizeL workBufSize = 0;
                ippiResizeGetBufferSize_L(pSpec, dstSize, 4, &workBufSize);
                uint8_t* pWorkBuf = workBufSize > 0 ? malloc(workBufSize) : NULL;
                IppiPointL dstOffset = { 0, 0 };
                ippiResizeLinear_8u_C4R_L(s->canvas_rgba, s->canvas_width * 4, s->capture_buf, s->capture_w * 4, dstOffset, dstSize, ippBorderRepl, NULL, pSpec, pWorkBuf);
                if (pWorkBuf) free(pWorkBuf);
                if (pInitBuf) free(pInitBuf);
                free(pSpec);
            }
            s->capture_state = CAPTURE_DONE;
            pthread_cond_signal(&s->capture_cond);
        } else if (s->capture_state == CAPTURE_REQUESTED) {
            s->capture_state = CAPTURE_DONE;
            pthread_cond_signal(&s->capture_cond);
        }
    }

    pthread_mutex_unlock(&s->lock);
    ZST_LOG_INFO("ippcompsink", "IPP compositor worker thread exiting");
    return NULL;
}

zst_result_t
zst_ipp_comp_sink_capture(zst_element_t* el, uint32_t width, uint32_t height, uint8_t* rgba_out)
{
    if (!el || !el->priv || !rgba_out) return ZST_ERROR;
    ipp_comp_sink_t* s = el->priv;

    pthread_mutex_lock(&s->lock);

    if (s->null_mode || !s->canvas_rgba) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    s->capture_buf = rgba_out;
    s->capture_w = width;
    s->capture_h = height;
    s->capture_state = CAPTURE_REQUESTED;
    pthread_cond_signal(&s->cond);

    while (s->capture_state != CAPTURE_DONE) {
        pthread_cond_wait(&s->capture_cond, &s->lock);
    }
    s->capture_state = CAPTURE_IDLE;

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static ipp_comp_input_t*
comp_find_input_by_name(ipp_comp_sink_t* s, const char* name)
{
    if (!s || !name) return NULL;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (strcmp(s->inputs[i]->name, name) == 0) return s->inputs[i];
    }
    return NULL;
}

static zst_result_t comp_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf);

static ipp_comp_input_t*
comp_add_input_locked(zst_element_t* el, const char* requested_name)
{
    ipp_comp_sink_t* s = el->priv;
    char name[IPPCOMP_MAX_NAME];
    if (requested_name && requested_name[0]) {
        snprintf(name, sizeof(name), "%s", requested_name);
        if (strncmp(name, "sink_", 5) != 0) return NULL;
        if (comp_find_input_by_name(s, name)) return comp_find_input_by_name(s, name);
    } else {
        do {
            snprintf(name, sizeof(name), "sink_%u", s->next_pad_index++);
        } while (comp_find_input_by_name(s, name));
    }

    ipp_comp_input_t* in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    in->parent = s;
    snprintf(in->name, sizeof(in->name), "%s", name);
    in->width = s->canvas_width;
    in->height = s->canvas_height;
    in->alpha = 1.0;
    in->visible = 1;
    snprintf(in->scaling, sizeof(in->scaling), "fit");
    in->border_width = 0;
    in->border_color[0] = 1.0f; in->border_color[1] = 1.0f; in->border_color[2] = 1.0f; in->border_color[3] = 1.0f;

    zst_queue_config_t qcfg = {
        .mode = ZST_QUEUE_SYNC,
        .max_buffers = 10
    };
    in->queue = zst_queue_create(&qcfg);
    if (!in->queue) { free(in); return NULL; }

    zst_pad_t* pad = zst_pad_create(in->name, ZST_PAD_SINK);
    if (!pad) { free(in); return NULL; }
    pad->push = comp_sink_pad_push;
    pad->priv = in;
    in->pad = pad;

    ipp_comp_input_t** inputs = realloc(s->inputs, (s->nb_inputs + 1) * sizeof(*s->inputs));
    if (!inputs) {
        zst_pad_destroy(pad);
        free(in);
        return NULL;
    }
    s->inputs = inputs;
    s->inputs[s->nb_inputs++] = in;

    if (zst_element_add_pad(el, pad) != ZST_OK) {
        s->nb_inputs--;
        zst_queue_destroy(in->queue);
        zst_pad_destroy(pad);
        free(in);
        return NULL;
    }
    return in;
}

static void
comp_input_free(ipp_comp_input_t* in)
{
    if (!in) return;
    if (in->latest) zst_buffer_unref(in->latest);
    if (in->queue) zst_queue_destroy(in->queue);
    free(in);
}

zst_result_t
zst_ipp_comp_sink_release_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad || !el->priv) return ZST_ERROR;
    ipp_comp_sink_t* s = el->priv;
    ipp_comp_input_t* in = pad->priv;

    pthread_mutex_lock(&s->lock);

    int idx = -1;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i] == in) {
            idx = (int)i;
            break;
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    zst_element_remove_pad(el, pad);
    memmove(&s->inputs[idx], &s->inputs[idx + 1], (s->nb_inputs - idx - 1) * sizeof(*s->inputs));
    s->nb_inputs--;

    comp_input_free(in);

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

zst_pad_t*
zst_ipp_comp_sink_request_pad(zst_element_t* el, const char* name)
{
    if (!el || !el->priv) return NULL;
    ipp_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    ipp_comp_input_t* in = comp_add_input_locked(el, name);
    pthread_mutex_unlock(&s->lock);
    return in ? in->pad : NULL;
}

static void
comp_auto_layout_locked(ipp_comp_sink_t* s)
{
    if (!s || s->nb_inputs == 0) return;
    uint32_t cols = 1;
    while (cols * cols < s->nb_inputs) cols++;
    uint32_t rows = (s->nb_inputs + cols - 1) / cols;
    uint32_t cell_w = s->canvas_width / cols;
    uint32_t cell_h = s->canvas_height / rows;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        s->inputs[i]->x = (int)((i % cols) * cell_w);
        s->inputs[i]->y = (int)((i / cols) * cell_h);
        s->inputs[i]->width = cell_w;
        s->inputs[i]->height = cell_h;
        s->inputs[i]->z_order = (int)i;
    }
}

static int
parse_bool(const char* value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0;
}

static zst_result_t
parse_background(const char* value, float out[4])
{
    if (!value) return ZST_ERROR;
    if (value[0] == '#') {
        unsigned int r = 0, g = 0, b = 0, a = 255;
        if (strlen(value) == 7 && sscanf(value + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = 1.0f;
            return ZST_OK;
        }
        if (strlen(value) == 9 && sscanf(value + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = a / 255.0f;
            return ZST_OK;
        }
    }
    double r = 0, g = 0, b = 0, a = 1;
    int n = sscanf(value, "%lf,%lf,%lf,%lf", &r, &g, &b, &a);
    if (n == 3 || n == 4) {
        out[0] = (float)r; out[1] = (float)g; out[2] = (float)b; out[3] = (float)a;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
comp_set_pad_property(ipp_comp_input_t* in, const char* prop, const char* value)
{
    if (!in || !prop || !value) return ZST_ERROR;
    if (strcmp(prop, "x") == 0) {
        int val = atoi(value);
        if (val < 0) val = 0;
        if (val > (int)in->parent->canvas_width) val = (int)in->parent->canvas_width;
        in->x = val;
        return ZST_OK;
    }
    if (strcmp(prop, "y") == 0) {
        int val = atoi(value);
        if (val < 0) val = 0;
        if (val > (int)in->parent->canvas_height) val = (int)in->parent->canvas_height;
        in->y = val;
        return ZST_OK;
    }
    if (strcmp(prop, "width") == 0) {
        uint32_t val = (uint32_t)strtoul(value, NULL, 10);
        if (val > in->parent->canvas_width) val = in->parent->canvas_width;
        in->width = val;
        return ZST_OK;
    }
    if (strcmp(prop, "height") == 0) {
        uint32_t val = (uint32_t)strtoul(value, NULL, 10);
        if (val > in->parent->canvas_height) val = in->parent->canvas_height;
        in->height = val;
        return ZST_OK;
    }
    if (strcmp(prop, "z-order") == 0 || strcmp(prop, "z_order") == 0 || strcmp(prop, "z") == 0) {
        in->z_order = atoi(value); return ZST_OK;
    }
    if (strcmp(prop, "alpha") == 0) {
        in->alpha = atof(value);
        if (in->alpha < 0.0) in->alpha = 0.0;
        if (in->alpha > 1.0) in->alpha = 1.0;
        return ZST_OK;
    }
    if (strcmp(prop, "visible") == 0) { in->visible = parse_bool(value); return ZST_OK; }
    if (strcmp(prop, "scaling") == 0) {
        if (strcmp(value, "fit") && strcmp(value, "stretch") && strcmp(value, "crop")) return ZST_ERROR;
        snprintf(in->scaling, sizeof(in->scaling), "%s", value);
        return ZST_OK;
    }
    if (strcmp(prop, "border-width") == 0) {
        in->border_width = (uint32_t)strtoul(value, NULL, 10);
        return ZST_OK;
    }
    if (strcmp(prop, "border-color") == 0) {
        return parse_background(value, in->border_color);
    }
    return ZST_ERROR;
}

static zst_result_t
comp_get_pad_property(ipp_comp_input_t* in, const char* prop, char* out, size_t max)
{
    if (!in || !prop || !out || max == 0) return ZST_ERROR;
    if (strcmp(prop, "x") == 0) { snprintf(out, max, "%d", in->x); return ZST_OK; }
    if (strcmp(prop, "y") == 0) { snprintf(out, max, "%d", in->y); return ZST_OK; }
    if (strcmp(prop, "width") == 0) { snprintf(out, max, "%u", in->width); return ZST_OK; }
    if (strcmp(prop, "height") == 0) { snprintf(out, max, "%u", in->height); return ZST_OK; }
    if (strcmp(prop, "z-order") == 0 || strcmp(prop, "z_order") == 0 || strcmp(prop, "z") == 0) { snprintf(out, max, "%d", in->z_order); return ZST_OK; }
    if (strcmp(prop, "alpha") == 0) { snprintf(out, max, "%.17g", in->alpha); return ZST_OK; }
    if (strcmp(prop, "visible") == 0) { snprintf(out, max, "%s", in->visible ? "true" : "false"); return ZST_OK; }
    if (strcmp(prop, "scaling") == 0) { snprintf(out, max, "%s", in->scaling); return ZST_OK; }
    if (strcmp(prop, "border-width") == 0) { snprintf(out, max, "%u", in->border_width); return ZST_OK; }
    if (strcmp(prop, "border-color") == 0) {
        snprintf(out, max, "#%02x%02x%02x%02x",
                 (unsigned)(in->border_color[0] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[1] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[2] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[3] * 255.0f + 0.5f));
        return ZST_OK;
    }
    if (strcmp(prop, "frame-count") == 0 || strcmp(prop, "frame_count") == 0) { snprintf(out, max, "%llu", (unsigned long long)in->frame_count); return ZST_OK; }
    return ZST_ERROR;
}

static int
split_pad_prop(const char* name, char* pad, size_t pad_len, const char** prop_out)
{
    const char* sep = strstr(name, "::");
    if (!sep) sep = strchr(name, '.');
    if (!sep) return 0;
    size_t n = (size_t)(sep - name);
    if (n == 0 || n >= pad_len) return 0;
    memcpy(pad, name, n);
    pad[n] = '\0';
    *prop_out = sep + ((sep[0] == ':' && sep[1] == ':') ? 2 : 1);
    return 1;
}

static int
comp_all_inputs_eos(ipp_comp_sink_t* s)
{
    if (!s || s->nb_inputs == 0) return 0;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (!s->inputs[i]->eos) return 0;
    }
    return 1;
}

static zst_result_t
comp_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf || !pad->priv) return ZST_ERROR;
    ipp_comp_input_t* in = pad->priv;
    ipp_comp_sink_t* s = in->parent;
    zst_element_t* el = pad->parent;
    zst_result_t ret = ZST_OK;

    if (buf->flags & ZST_BUFFER_FLAG_DROP) {
        return ZST_OK;
    }

    if (el && el->clock && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_time_t current = zst_clock_get_time(el->clock);
        if (current > buf->pts && (current - buf->pts) > (zst_time_t)s->max_lateness) {
            if (current - buf->pts < 5000000000ULL) {
                buf->flags |= ZST_BUFFER_FLAG_DROP;
                if (el->bus) {
                    zst_event_t* qos_ev = zst_event_new_warning(el, ZST_ERROR, "QoS: Frame dropped (too late)");
                    zst_bus_post(el->bus, qos_ev);
                }
                return ZST_OK;
            }
        }
    }

    pthread_mutex_lock(&s->lock);
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        in->eos = 1;
        ret = comp_all_inputs_eos(s) ? ZST_EOF : ZST_OK;
        pthread_cond_signal(&s->cond);
    } else {
        if (zst_queue_push(in->queue, buf, 0) == ZST_OK) {
            in->frame_count++;
            pthread_cond_signal(&s->cond);
        }
        ret = ZST_OK;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_result_t
ippcomp_open(zst_element_t* el)
{
    ipp_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    s->null_mode = 0;
    s->window_open = 0;
    s->composite_count = 0;

    if (s->canvas_width == 0) s->canvas_width = 640;
    if (s->canvas_height == 0) s->canvas_height = 480;

    s->canvas_size = s->canvas_width * s->canvas_height * 4;
    s->canvas_rgba = malloc(s->canvas_size);
    if (!s->canvas_rgba) {
        s->null_mode = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    s->window_open = 1;

    s->running = true;
    pthread_create(&s->worker_thread, NULL, ippcomp_worker_thread, el);

    ZST_LOG_INFO("ippcompsink", "opened IPP compositor '%s' (%ux%u, inputs=%u)",
                 s->window_title, s->canvas_width, s->canvas_height, s->nb_inputs);
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
ippcomp_close(zst_element_t* el)
{
    ipp_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (s->worker_thread) {
        pthread_join(s->worker_thread, NULL);
        s->worker_thread = 0;
    }

    pthread_mutex_lock(&s->lock);
    if (s->canvas_rgba) {
        free(s->canvas_rgba);
        s->canvas_rgba = NULL;
    }
    s->window_open = 0;

    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        comp_input_free(s->inputs[i]);
    }
    free(s->inputs);
    s->inputs = NULL;
    s->nb_inputs = 0;
    s->null_mode = 0;
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
ippcomp_process(zst_element_t* el, zst_buffer_t* inbuf, zst_buffer_t** out)
{
    (void)out;
    if (!inbuf) return ZST_AGAIN;
    zst_pad_t* pad = zst_element_get_pad(el, "sink_0");
    if (!pad || !pad->push) return ZST_ERROR;
    return pad->push(pad, inbuf);
}

static zst_result_t
ippcomp_set_property(zst_element_t* el, const char* name, const char* value)
{
    ipp_comp_sink_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    char pad_name[IPPCOMP_MAX_NAME];
    const char* prop = NULL;
    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        ipp_comp_input_t* in = comp_find_input_by_name(s, pad_name);
        zst_result_t r = in ? comp_set_pad_property(in, prop, value) : ZST_ERROR;
        pthread_mutex_unlock(&s->lock);
        return r;
    }

    zst_result_t ret = ZST_OK;
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(s->window_title, sizeof(s->window_title), "%s", value);
    } else if (strcmp(name, "canvas-width") == 0 || strcmp(name, "width") == 0) {
        s->canvas_width = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(name, "canvas-height") == 0 || strcmp(name, "height") == 0) {
        s->canvas_height = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(name, "background-color") == 0 || strcmp(name, "background") == 0) {
        ret = parse_background(value, s->bg);
    } else if (strcmp(name, "fullscreen") == 0) {
        s->fullscreen = parse_bool(value);
    } else if (strcmp(name, "vsync") == 0) {
        s->vsync = parse_bool(value);
    } else if (strcmp(name, "display-rate") == 0 || strcmp(name, "rate") == 0) {
        s->display_rate = atof(value);
        if (s->display_rate <= 0.0) s->display_rate = 30.0;
    } else if (strcmp(name, "max-lateness") == 0 || strcmp(name, "max_lateness") == 0) {
        s->max_lateness = (int64_t)atoll(value);
    } else if (strcmp(name, "input-count") == 0 || strcmp(name, "inputs") == 0) {
        uint32_t n = (uint32_t)strtoul(value, NULL, 10);
        while (ret == ZST_OK && s->nb_inputs < n) {
            if (!comp_add_input_locked(el, NULL)) ret = ZST_ERROR;
        }
        if (ret == ZST_OK) comp_auto_layout_locked(s);
    } else if (strcmp(name, "request-pad") == 0) {
        if (!comp_add_input_locked(el, (strcmp(value, "auto") == 0) ? NULL : value)) ret = ZST_ERROR;
    } else {
        ret = ZST_ERROR;
    }

    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_result_t
ippcomp_get_property(zst_element_t* el, const char* name, char* out, size_t max)
{
    ipp_comp_sink_t* s = el->priv;
    if (!name || !out || max == 0) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    char pad_name[IPPCOMP_MAX_NAME];
    const char* prop = NULL;
    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        ipp_comp_input_t* in = comp_find_input_by_name(s, pad_name);
        zst_result_t r = in ? comp_get_pad_property(in, prop, out, max) : ZST_ERROR;
        pthread_mutex_unlock(&s->lock);
        return r;
    }

    zst_result_t ret = ZST_OK;
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(out, max, "%s", s->window_title);
    } else if (strcmp(name, "canvas-width") == 0 || strcmp(name, "width") == 0) {
        snprintf(out, max, "%u", s->canvas_width);
    } else if (strcmp(name, "canvas-height") == 0 || strcmp(name, "height") == 0) {
        snprintf(out, max, "%u", s->canvas_height);
    } else if (strcmp(name, "background-color") == 0 || strcmp(name, "background") == 0) {
        snprintf(out, max, "#%02x%02x%02x%02x",
                 (unsigned)(s->bg[0] * 255.0f + 0.5f),
                 (unsigned)(s->bg[1] * 255.0f + 0.5f),
                 (unsigned)(s->bg[2] * 255.0f + 0.5f),
                 (unsigned)(s->bg[3] * 255.0f + 0.5f));
    } else if (strcmp(name, "fullscreen") == 0) {
        snprintf(out, max, "%s", s->fullscreen ? "true" : "false");
    } else if (strcmp(name, "vsync") == 0) {
        snprintf(out, max, "%s", s->vsync ? "true" : "false");
    } else if (strcmp(name, "display-rate") == 0 || strcmp(name, "rate") == 0) {
        snprintf(out, max, "%.17g", s->display_rate);
    } else if (strcmp(name, "max-lateness") == 0) {
        snprintf(out, max, "%lld", (long long)s->max_lateness);
    } else if (strcmp(name, "input-count") == 0 || strcmp(name, "inputs") == 0) {
        snprintf(out, max, "%u", s->nb_inputs);
    } else if (strcmp(name, "composite-count") == 0 || strcmp(name, "frame-count") == 0) {
        snprintf(out, max, "%llu", (unsigned long long)s->composite_count);
    } else if (strcmp(name, "null-mode") == 0) {
        snprintf(out, max, "%s", s->null_mode ? "true" : "false");
    } else {
        ret = ZST_ERROR;
    }

    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_element_ops_t g_ippcompsink_ops = {
    .name = "ippcompsink",
    .open = ippcomp_open,
    .close = ippcomp_close,
    .process = ippcomp_process,
    .set_property = ippcomp_set_property,
    .get_property = ippcomp_get_property,
};

zst_element_t*
zst_ipp_comp_sink_create(void)
{
    ipp_comp_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    snprintf(priv->window_title, sizeof(priv->window_title), "zstreamer IPP Compositor Sink");
    priv->canvas_width = 640;
    priv->canvas_height = 480;
    priv->bg[0] = 0.0f; priv->bg[1] = 0.0f; priv->bg[2] = 0.0f; priv->bg[3] = 1.0f;
    priv->vsync = 1;
    priv->max_lateness = 20000000;
    priv->display_rate = 30.0;
    pthread_mutex_init(&priv->lock, NULL);
    pthread_cond_init(&priv->cond, NULL);
    pthread_cond_init(&priv->capture_cond, NULL);

    zst_element_t* el = zst_element_create(&g_ippcompsink_ops, priv);
    if (!el) {
        pthread_cond_destroy(&priv->capture_cond);
        pthread_cond_destroy(&priv->cond);
        pthread_mutex_destroy(&priv->lock);
        free(priv);
        return NULL;
    }

    pthread_mutex_lock(&priv->lock);
    comp_add_input_locked(el, "sink_0");
    pthread_mutex_unlock(&priv->lock);
    return el;
}

zst_element_t*
zst_ipp_comp_sink_create_with_config(const zst_ipp_comp_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(*config)) return NULL;
    zst_element_t* el = zst_element_factory_make("ippcompsink");
    if (!el) return NULL;
    if (config->window_title) zst_element_set_property_string(el, "window-title", config->window_title);
    if (config->canvas_width > 0) zst_element_set_property_uint(el, "canvas-width", config->canvas_width);
    if (config->canvas_height > 0) zst_element_set_property_uint(el, "canvas-height", config->canvas_height);
    if (config->background_color) zst_element_set_property_string(el, "background-color", config->background_color);
    zst_element_set_property_bool(el, "fullscreen", config->fullscreen ? true : false);
    zst_element_set_property_bool(el, "vsync", config->vsync ? true : false);
    if (config->input_count > 0) zst_element_set_property_uint(el, "input-count", config->input_count);
    if (config->struct_size >= offsetof(zst_ipp_comp_sink_config_t, max_lateness) + sizeof(int64_t)) {
        if (config->max_lateness > 0) {
            zst_element_set_property_int(el, "max-lateness", config->max_lateness);
        }
    }
    if (config->struct_size >= offsetof(zst_ipp_comp_sink_config_t, display_rate) + sizeof(double)) {
        if (config->display_rate > 0) {
            zst_element_set_property_double(el, "display-rate", config->display_rate);
        }
    }
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "ippcompsink") == 0 || strcmp(name, "ippcomp") == 0) {
        return zst_ipp_comp_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_ippcompsink_properties[] = {
    { "window-title",    ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zstreamer IPP Compositor Sink", "Window title" },
    { "canvas-width",    ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Canvas width" },
    { "canvas-height",   ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Canvas height" },
    { "background-color",ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "#000000ff", "Canvas background color (#RRGGBB[AA] or r,g,b[,a])" },
    { "fullscreen",      ZST_PROPERTY_BOOL,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Fullscreen mode" },
    { "vsync",           ZST_PROPERTY_BOOL,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Enable VSync" },
    { "display-rate",    ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30.0", "Composition frame rate" },
    { "max-lateness",    ZST_PROPERTY_INT,    ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "20000000", "Maximum frame lateness in nanoseconds before dropping" },
    { "input-count",     ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Number of sink_%u input pads" }
};

static const zst_pad_template_t g_ippcompsink_pads[] = {
    { "sink_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_ippcompsink_elements[] = {
    {
        .name = "ippcompsink",
        .long_name = "Intel IPP Compositor Sink",
        .category = "Sink/Video",
        .description = "Composites multiple raw video streams into one buffer using Intel IPP",
        .author = "zstreamer",
        .properties = g_ippcompsink_properties,
        .nb_properties = sizeof(g_ippcompsink_properties) / sizeof(g_ippcompsink_properties[0]),
        .pads = g_ippcompsink_pads,
        .nb_pads = sizeof(g_ippcompsink_pads) / sizeof(g_ippcompsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = { .name = "ippcompsink_plugin", .author = "zstreamer", .version = "1.0.0", .init = NULL, .deinit = NULL },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) *nb_elements_out = sizeof(g_ippcompsink_elements) / sizeof(g_ippcompsink_elements[0]);
    return g_ippcompsink_elements;
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
