/*=============================================================================
    test_gl_comp_sink.c — OpenGL compositor sink tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_gl_comp_sink.h"
#include "zst_clock.h"
#include "zst_bus.h"
/* Tolerance for pixel comparison (allow minor GL precision differences) */
#define PIXEL_TOL 1
#define ALPHA_TOL 2

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_skipped = 0;

#define TEST(name) do { g_tests_run++; printf("  TEST: %-50s ... ", name); fflush(stdout); } while (0)
#define PASS() do { g_tests_passed++; printf("PASSED\n"); } while (0)
#define SKIP(msg) do { g_tests_skipped++; printf("SKIP (%s) ", msg); return; } while (0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); return; } while (0)

static void
free_video_buffer(zst_buffer_t* buf)
{
    if (!buf) return;
    free(buf->payload);
    free(buf->memory.data);
}

static zst_buffer_t*
make_yuv420p_buffer(uint32_t width, uint32_t height, unsigned char yv, unsigned char uv, unsigned char vv)
{
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total = y_size + uv_size * 2;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) return NULL;
    buf->destroy = free_video_buffer;
    buf->memory.data = calloc(1, total);
    buf->memory.size = total;
    buf->memory.type = ZST_MEMORY_CPU;

    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame || !buf->memory.data) {
        free(frame);
        zst_buffer_unref(buf);
        return NULL;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 0;
    frame->plane[0] = buf->memory.data;
    frame->plane[1] = (unsigned char*)buf->memory.data + y_size;
    frame->plane[2] = (unsigned char*)buf->memory.data + y_size + uv_size;
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;
    memset(frame->plane[0], yv, y_size);
    memset(frame->plane[1], uv, uv_size);
    memset(frame->plane[2], vv, uv_size);
    buf->payload = frame;
    buf->duration = 33333333;
    return buf;
}

static zst_buffer_t*
make_rgb_buffer(uint32_t width, uint32_t height, unsigned char r, unsigned char g, unsigned char b)
{
    size_t total = width * height * 3;
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    if (!buf) return NULL;
    buf->destroy = free_video_buffer;
    buf->memory.data = malloc(total);
    buf->memory.size = total;
    buf->memory.type = ZST_MEMORY_CPU;

    zst_video_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame || !buf->memory.data) {
        free(frame);
        zst_buffer_unref(buf);
        return NULL;
    }
    unsigned char* p = buf->memory.data;
    for (size_t i = 0; i < total; i += 3) {
        p[i + 0] = r;
        p[i + 1] = g;
        p[i + 2] = b;
    }
    frame->width = width;
    frame->height = height;
    frame->format = 2;
    frame->plane[0] = p;
    frame->stride[0] = width * 3;
    buf->payload = frame;
    buf->duration = 33333333;
    return buf;
}

/* ─── Pixel readback helpers ─────────────────────────────────────────── */

static uint8_t*
pixel_at(uint8_t* rgba, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    /* glReadPixels origin is bottom-left; convert (x,y) from top-left */
    uint32_t row = h - 1 - y;
    return rgba + (row * w + x) * 4;
}

static int
pixel_check(uint8_t* rgba, uint32_t w, uint32_t h,
            uint32_t x, uint32_t y,
            uint8_t r, uint8_t g, uint8_t b, uint8_t a,
            int tol)
{
    uint8_t* p = pixel_at(rgba, w, h, x, y);
    return abs((int)p[0] - r) <= tol &&
           abs((int)p[1] - g) <= tol &&
           abs((int)p[2] - b) <= tol &&
           abs((int)p[3] - a) <= tol;
}

/* Helper: attempt a capture.  If GL context is not available (null mode),
 * clean up and record as SKIP. */
#define TRY_CAPTURE_OR_SKIP(el, w, h, buf) do { \
    if (zst_gl_comp_sink_capture(el, w, h, buf) != ZST_OK) { \
        zst_element_set_state(el, ZST_STATE_NULL); \
        zst_element_destroy(el); \
        SKIP("no GL context"); \
    } \
} while (0)

static zst_element_t*
make_glcompsink(void)
{
    return zst_element_factory_make("glcompsink");
}

static void
test_factory_create(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("zst_element_factory_make(\"glcompsink\") returned NULL");
    if (!zst_element_get_pad(el, "sink_0")) FAIL("default sink_0 missing");
    zst_element_destroy(el);
    PASS();
}

static void
test_properties_and_dynamic_pads(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    if (zst_element_set_property_string(el, "window-title", "Compositor Test") != ZST_OK) FAIL("set window-title failed");
    if (zst_element_set_property_uint(el, "canvas-width", 800) != ZST_OK) FAIL("set canvas-width failed");
    if (zst_element_set_property_uint(el, "canvas-height", 600) != ZST_OK) FAIL("set canvas-height failed");
    if (zst_element_set_property_string(el, "background-color", "#203040ff") != ZST_OK) FAIL("set background-color failed");
    if (zst_element_set_property_uint(el, "input-count", 4) != ZST_OK) FAIL("set input-count failed");

    for (int i = 0; i < 4; i++) {
        char pad_name[16];
        snprintf(pad_name, sizeof(pad_name), "sink_%d", i);
        if (!zst_element_get_pad(el, pad_name)) FAIL("requested sink pad missing");
    }

    if (zst_element_set_property_int(el, "sink_1::x", 123) != ZST_OK) FAIL("set pad x failed");
    if (zst_element_set_property_int(el, "sink_1::y", 45) != ZST_OK) FAIL("set pad y failed");
    if (zst_element_set_property_uint(el, "sink_1::width", 320) != ZST_OK) FAIL("set pad width failed");
    if (zst_element_set_property_uint(el, "sink_1::height", 240) != ZST_OK) FAIL("set pad height failed");
    if (zst_element_set_property_double(el, "sink_1::alpha", 0.5) != ZST_OK) FAIL("set pad alpha failed");
    if (zst_element_set_property_string(el, "sink_1::scaling", "crop") != ZST_OK) FAIL("set pad scaling failed");
    if (zst_element_set_property_string(el, "sink_1::scaling", "invalid") == ZST_OK) FAIL("invalid scaling should fail");

    char buf[128];
    if (zst_element_get_property_string(el, "window-title", buf, sizeof(buf)) != ZST_OK || strcmp(buf, "Compositor Test") != 0) FAIL("get window-title failed");
    if (zst_element_get_property_string(el, "sink_1::scaling", buf, sizeof(buf)) != ZST_OK || strcmp(buf, "crop") != 0) FAIL("get pad scaling failed");
    uint64_t n = 0;
    if (zst_element_get_property_uint(el, "input-count", &n) != ZST_OK || n != 4) FAIL("get input-count failed");

    /* Test new properties */
    if (zst_element_set_property_double(el, "display-rate", 60.0) != ZST_OK) FAIL("set display-rate failed");
    double rate = 0;
    if (zst_element_get_property_double(el, "display-rate", &rate) != ZST_OK || rate != 60.0) FAIL("get display-rate failed");

    if (zst_element_set_property_uint(el, "sink_0::border-width", 2) != ZST_OK) FAIL("set border-width failed");
    if (zst_element_set_property_string(el, "sink_0::border-color", "#ff0000") != ZST_OK) FAIL("set border-color failed");

    zst_element_destroy(el);
    PASS();
}

static void
test_input_event_handling(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    /* 1. Verify ZST_EVENT_KEY_PRESS creation, fields, and destruction */
    zst_event_t* ev = zst_event_new_key_press(el, 65, 38, "a");
    if (!ev) {
        FAIL("zst_event_new_key_press returned NULL");
    }
    if (ev->type != ZST_EVENT_KEY_PRESS) {
        FAIL("event type is not ZST_EVENT_KEY_PRESS");
    }
    if (ev->src != el) {
        FAIL("event src is not matching");
    }
    if (ev->as.key_press.key_sym != 65) {
        FAIL("key_sym is not matching");
    }
    if (ev->as.key_press.key_code != 38) {
        FAIL("key_code is not matching");
    }
    if (strcmp(ev->as.key_press.key_str, "a") != 0) {
        FAIL("key_str is not matching");
    }
    zst_event_destroy(ev);

    /* 4. Verify ZST_EVENT_MOUSE_BUTTON creation, fields, and destruction */
    zst_event_t* mev = zst_event_new_mouse_button(el, 1, 1, 100, 150);
    if (!mev) {
        FAIL("zst_event_new_mouse_button returned NULL");
    }
    if (mev->type != ZST_EVENT_MOUSE_BUTTON) {
        FAIL("event type is not ZST_EVENT_MOUSE_BUTTON");
    }
    if (mev->src != el) {
        FAIL("event src is not matching");
    }
    if (mev->as.mouse_button.button != 1) {
        FAIL("mouse button is not matching");
    }
    if (mev->as.mouse_button.pressed != 1) {
        FAIL("mouse pressed state is not matching");
    }
    if (mev->as.mouse_button.x != 100 || mev->as.mouse_button.y != 150) {
        FAIL("mouse coordinates are not matching");
    }
    zst_event_destroy(mev);

    /* 5. Verify ZST_EVENT_MOUSE_MOTION creation, fields, and destruction */
    zst_event_t* mmev = zst_event_new_mouse_motion(el, 200, 250);
    if (!mmev) {
        FAIL("zst_event_new_mouse_motion returned NULL");
    }
    if (mmev->type != ZST_EVENT_MOUSE_MOTION) {
        FAIL("event type is not ZST_EVENT_MOUSE_MOTION");
    }
    if (mmev->src != el) {
        FAIL("event src is not matching");
    }
    if (mmev->as.mouse_motion.x != 200 || mmev->as.mouse_motion.y != 250) {
        FAIL("mouse motion coordinates are not matching");
    }
    zst_event_destroy(mmev);

    zst_element_destroy(el);
    PASS();
}

static void
test_request_release_pad_api(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_pad_t* p = zst_gl_comp_sink_request_pad(el, NULL);
    if (!p || strcmp(p->name, "sink_1") != 0) FAIL("auto request pad failed");

    zst_pad_t* p7 = zst_gl_comp_sink_request_pad(el, "sink_7");
    if (!p7 || strcmp(p7->name, "sink_7") != 0) FAIL("named request pad failed");

    if (zst_gl_comp_sink_release_pad(el, p) != ZST_OK) FAIL("release pad failed");
    if (zst_element_get_pad(el, "sink_1") != NULL) FAIL("released pad still found");

    zst_element_destroy(el);
    PASS();
}

static void
test_null_mode_multi_input(void)
{
    char* old_display = getenv("DISPLAY") ? strdup(getenv("DISPLAY")) : NULL;
    unsetenv("DISPLAY");

    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_result_t r = zst_element_set_state(el, ZST_STATE_READY);
    if (r != ZST_OK) FAIL("NULL -> READY failed");
    r = zst_element_set_state(el, ZST_STATE_PLAYING);
    if (r != ZST_OK) FAIL("READY -> PLAYING failed");

    zst_buffer_t* a = make_yuv420p_buffer(160, 120, 80, 90, 240);
    zst_buffer_t* b = make_rgb_buffer(160, 120, 10, 200, 40);
    if (!a || !b) FAIL("buffer allocation failed");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    if (!p0 || !p1 || !p0->push || !p1->push) FAIL("sink pad push missing");
    if (p0->push(p0, a) != ZST_OK) FAIL("push sink_0 failed");
    if (p1->push(p1, b) != ZST_OK) FAIL("push sink_1 failed");
    zst_buffer_unref(a);
    zst_buffer_unref(b);

    char val[64];
    if (zst_element_get_property_string(el, "null-mode", val, sizeof(val)) != ZST_OK || strcmp(val, "true") != 0) FAIL("null-mode property not true");
    if (zst_element_get_property_string(el, "sink_1::frame-count", val, sizeof(val)) != ZST_OK || strcmp(val, "1") != 0) FAIL("pad frame-count wrong");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    if (old_display) { setenv("DISPLAY", old_display, 1); free(old_display); }
    PASS();
}

static void
test_eos_per_pad(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_USER);
    if (!eos) FAIL("eos alloc failed");
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    if (p0->push(p0, eos) != ZST_OK) FAIL("first EOS should not finish all pads");
    if (p1->push(p1, eos) != ZST_EOF) FAIL("second EOS should return ZST_EOF");
    zst_buffer_unref(eos);
    zst_element_destroy(el);
    PASS();
}

static void
test_xvfb_gl_smoke(void)
{
    const char* display = getenv("DISPLAY");
    if (!display || display[0] == '\0') {
        SKIP("DISPLAY not set");
    }

    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");
    zst_element_set_property_uint(el, "canvas-width", 320);
    zst_element_set_property_uint(el, "canvas-height", 240);
    zst_element_set_property_string(el, "background-color", "#000000ff");
    zst_element_set_property_uint(el, "input-count", 2);
    zst_element_set_property_int(el, "sink_0::x", 0);
    zst_element_set_property_int(el, "sink_0::y", 0);
    zst_element_set_property_uint(el, "sink_0::width", 160);
    zst_element_set_property_uint(el, "sink_0::height", 240);
    zst_element_set_property_int(el, "sink_1::x", 160);
    zst_element_set_property_int(el, "sink_1::y", 0);
    zst_element_set_property_uint(el, "sink_1::width", 160);
    zst_element_set_property_uint(el, "sink_1::height", 240);

    if (zst_element_set_state(el, ZST_STATE_READY) != ZST_OK) FAIL("open failed");
    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("start failed");

    zst_buffer_t* a = make_yuv420p_buffer(160, 120, 128, 128, 128);
    zst_buffer_t* b = make_rgb_buffer(160, 120, 255, 0, 0);
    if (!a || !b) FAIL("buffer allocation failed");
    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    if (p0->push(p0, a) != ZST_OK) FAIL("gl push sink_0 failed");
    if (p1->push(p1, b) != ZST_OK) FAIL("gl push sink_1 failed");
    zst_buffer_unref(a);
    zst_buffer_unref(b);

    /* Wait for the worker thread to render at least 2 frames */
    uint64_t composites = 0;
    for (int i = 0; i < 100; i++) {
        if (zst_element_get_property_uint(el, "composite-count", &composites) == ZST_OK && composites >= 2) break;
        struct timespec ts = { .tv_nsec = 5000000 }; /* 5ms */
        nanosleep(&ts, NULL);
    }
    if (composites < 2) FAIL("composite-count too small");
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static zst_time_t
mock_clock_get_time(zst_clock_t* clock)
{
    return 100000000; /* 100ms */
}

static void
test_qos_dropping(void)
{
    /* Force null-mode by providing an invalid display name.
     * The test only exercises the QoS dropping mechanism, not actual GL output. */
    char* qos_display_saved = getenv("DISPLAY") ? strdup(getenv("DISPLAY")) : NULL;
    setenv("DISPLAY", "nosuchdisplay", 1);

    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("creation failed");

    zst_element_set_property_uint(el, "input-count", 1);
    zst_element_set_property_int(el, "max-lateness", 20000000); /* 20ms */

    zst_clock_t* clk = zst_clock_system_create();
    clk->get_time = mock_clock_get_time;
    zst_element_set_clock(el, clk);

    zst_element_set_state(el, ZST_STATE_PLAYING);

    zst_pad_t* pad = zst_element_get_pad(el, "sink_0");
    if (!pad) FAIL("no sink_0 pad");

    zst_buffer_t* buf = zst_buffer_create(ZST_MEMORY_CPU);
    buf->pts = 10000000; /* 10ms - late by 90ms (current=100ms, max-lateness=20ms) */

    /* Simulate a direct push to the sink pad, bypassing zst_pad_push which expects a src pad */
    zst_result_t res = pad->push(pad, buf);
    if (res != ZST_OK) FAIL("push failed");

    if (!(buf->flags & ZST_BUFFER_FLAG_DROP)) FAIL("buffer was not dropped");

    char count[32];
    zst_element_get_property(el, "frame-count", count, sizeof(count));
    if (strcmp(count, "0") != 0) FAIL("frame count should be 0");

    zst_clock_unref(clk);
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);

    if (qos_display_saved) { setenv("DISPLAY", qos_display_saved, 1); free(qos_display_saved); } else { unsetenv("DISPLAY"); }
    PASS();
}

/* ─── Pixel readback tests (require GL context) ──────────────────────── */

static void
test_capture_background(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 160);
    zst_element_set_property_uint(el, "canvas-height", 120);
    zst_element_set_property_string(el, "background-color", "#0000ffff"); /* blue bg */

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    /* Let the worker thread complete its first render cycle */
    struct timespec ts = { .tv_nsec = 50000000 }; /* 50ms */
    nanosleep(&ts, NULL);

    uint8_t pixels[160 * 120 * 4];
    TRY_CAPTURE_OR_SKIP(el, 160, 120, pixels);

    /* All pixels should be blue bg */
    if (!pixel_check(pixels, 160, 120, 0,   0,   0, 0, 255, 255, PIXEL_TOL)) FAIL("top-left bg");
    if (!pixel_check(pixels, 160, 120, 159, 0,   0, 0, 255, 255, PIXEL_TOL)) FAIL("top-right bg");
    if (!pixel_check(pixels, 160, 120, 0,   119, 0, 0, 255, 255, PIXEL_TOL)) FAIL("bot-left bg");
    if (!pixel_check(pixels, 160, 120, 80,  60,  0, 0, 255, 255, PIXEL_TOL)) FAIL("center bg");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_z_order(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 200);
    zst_element_set_property_uint(el, "canvas-height", 200);
    zst_element_set_property_string(el, "background-color", "#000000ff"); /* black bg */
    zst_element_set_property_uint(el, "input-count", 2);

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    /* Position both inputs overlapping at (25,25,150,150) */
    /* Place inputs side-by-side with margins so bg is visible */
    zst_element_set_property_int(el, "sink_0::x", 10);
    zst_element_set_property_int(el, "sink_0::y", 10);
    zst_element_set_property_uint(el, "sink_0::width", 80);
    zst_element_set_property_uint(el, "sink_0::height", 180);
    zst_element_set_property_int(el, "sink_1::x", 110);
    zst_element_set_property_int(el, "sink_1::y", 10);
    zst_element_set_property_uint(el, "sink_1::width", 80);
    zst_element_set_property_uint(el, "sink_1::height", 180);

    /* sink_0: red (255,0,0) at z=0, sink_1: green (0,255,0) at z=1 */
    zst_element_set_property_int(el, "sink_0::z_order", 0);
    zst_element_set_property_int(el, "sink_1::z_order", 1);

    /* Push buffers */
    zst_buffer_t* red   = make_rgb_buffer(80, 180, 255, 0, 0);
    zst_buffer_t* green = make_rgb_buffer(80, 180, 0, 255, 0);
    if (!red || !green) FAIL("buffer allocation");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    zst_pad_t* p1 = zst_element_get_pad(el, "sink_1");
    if (p0->push(p0, red) != ZST_OK)   FAIL("push sink_0");
    if (p1->push(p1, green) != ZST_OK) FAIL("push sink_1");
    zst_buffer_unref(red);
    zst_buffer_unref(green);

    uint8_t pixels[200 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 200, 200, pixels);

    {
        uint8_t* pp = pixel_at(pixels, 200, 200, 100, 100);
        printf("  [z-order] pxl=(%u,%u,%u,%u) ", pp[0], pp[1], pp[2], pp[3]);
    }

    /* sink_0 area: pixel(50,100) should be red */
    if (!pixel_check(pixels, 200, 200, 50, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("sink_0 area should be red");

    /* sink_1 area: pixel(150,100) should be green */
    if (!pixel_check(pixels, 200, 200, 150, 100, 0, 255, 0, 255, PIXEL_TOL))
        FAIL("sink_1 area should be green");

    /* bg area: pixel(0,0) should be black */
    if (!pixel_check(pixels, 200, 200, 0, 0, 0, 0, 0, 255, PIXEL_TOL))
        FAIL("bg should be black (no input at 0,0)");

    /* Verify z-order: with overlapping, higher z = on top */
    {
        /* Temporarily move inputs to overlap for z-order verification */
        zst_element_set_property_int(el, "sink_0::x", 25);
        zst_element_set_property_int(el, "sink_0::y", 25);
        zst_element_set_property_uint(el, "sink_0::width", 150);
        zst_element_set_property_uint(el, "sink_0::height", 150);
        zst_element_set_property_int(el, "sink_1::x", 25);
        zst_element_set_property_int(el, "sink_1::y", 25);
        zst_element_set_property_uint(el, "sink_1::width", 150);
        zst_element_set_property_uint(el, "sink_1::height", 150);
        zst_element_set_property_int(el, "sink_0::z_order", 0);
        zst_element_set_property_int(el, "sink_1::z_order", 1);
        uint8_t pixels_overlap[200*200*4];
        TRY_CAPTURE_OR_SKIP(el, 200, 200, pixels_overlap);
        /* With z0=0, z1=1, overlap at (100,100) should be green */
        if (!pixel_check(pixels_overlap, 200, 200, 100, 100, 0, 255, 0, 255, PIXEL_TOL)) {
            uint8_t* pp = pixel_at(pixels_overlap, 200, 200, 100, 100);
            printf("overlap_dbg=(%u,%u,%u,%u) ", pp[0], pp[1], pp[2], pp[3]);
            FAIL("z-order=1 should be on top (green)");
        }
    }

    /* ── Reorder: swap z-order values ── */
    zst_element_set_property_int(el, "sink_0::z_order", 1);
    zst_element_set_property_int(el, "sink_1::z_order", 0);

    uint8_t pixels2[200 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 200, 200, pixels2);

    /* After re-order, sink_0 (red) should now be on top */
    {
        uint8_t* pp = pixel_at(pixels2, 200, 200, 100, 100);
        printf("[reorder] pxl=(%u,%u,%u,%u) ", pp[0], pp[1], pp[2], pp[3]);
    }
    if (!pixel_check(pixels2, 200, 200, 100, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("after reorder, red should be on top");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_alpha(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 200);
    zst_element_set_property_uint(el, "canvas-height", 200);
    /* Fill canvas with a bright green background */
    zst_element_set_property_string(el, "background-color", "#00ff00ff"); /* green bg */

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    /* Push a semi-transparent red input covering the whole canvas */
    zst_element_set_property_int(el, "sink_0::x", 0);
    zst_element_set_property_int(el, "sink_0::y", 0);
    zst_element_set_property_uint(el, "sink_0::width", 200);
    zst_element_set_property_uint(el, "sink_0::height", 200);
    zst_element_set_property_double(el, "sink_0::alpha", 0.5);

    zst_buffer_t* red = make_rgb_buffer(200, 200, 255, 0, 0);
    if (!red) FAIL("buffer allocation");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    if (p0->push(p0, red) != ZST_OK) FAIL("push sink_0");
    zst_buffer_unref(red);

    uint8_t pixels[200 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 200, 200, pixels);

    /* Expected: src=(255,0,0) alpha=0.5 blended with dst=(0,255,0)
     * result = src*0.5 + dst*0.5 = (127, 127, 0) */
    if (!pixel_check(pixels, 200, 200, 100, 100, 127, 127, 0, 255, ALPHA_TOL))
        FAIL("alpha blending: expected ~(127,127,0)");

    /* Check a corner too */
    if (!pixel_check(pixels, 200, 200, 10, 10, 127, 127, 0, 255, ALPHA_TOL))
        FAIL("alpha blending corner: expected ~(127,127,0)");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_scaling(void)
{
    zst_element_t* el = make_glcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 400);
    zst_element_set_property_uint(el, "canvas-height", 200);
    zst_element_set_property_string(el, "background-color", "#000000ff"); /* black bg */

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    /* Place a single input covering most of the canvas with default stretch */
    zst_element_set_property_int(el, "sink_0::x", 0);
    zst_element_set_property_int(el, "sink_0::y", 0);
    zst_element_set_property_uint(el, "sink_0::width", 400);
    zst_element_set_property_uint(el, "sink_0::height", 200);

    /* Create a 100x200 (portrait) RGB input filled with red */
    zst_buffer_t* red = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red) FAIL("buffer allocation");

    /* ── Test 1: Stretch mode ──────────────────────────────────────── */
    zst_element_set_property_string(el, "sink_0::scaling", "stretch");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    if (p0->push(p0, red) != ZST_OK) FAIL("push sink_0");
    /* Must re-apply buffer for each mode test; push it again for next capture */
    zst_buffer_ref(red);
    if (p0->push(p0, red) != ZST_OK) FAIL("push sink_0 again");

    uint8_t pixels[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels);

    /* Under stretch, the input fills the entire 400x200 area */
    if (!pixel_check(pixels, 400, 200, 10,  10,  255, 0, 0, 255, PIXEL_TOL))
        FAIL("stretch: top-left should be red");
    if (!pixel_check(pixels, 400, 200, 390, 190, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("stretch: bottom-right should be red");
    if (!pixel_check(pixels, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("stretch: center should be red");

    /* ── Test 2: Fit mode (portrait input in landscape dest) ────────── */
    /* Refresh the buffer — we consumed it above but latest is cached */
    zst_element_set_property_string(el, "sink_0::scaling", "fit");

    /* Push fresh buffers for the fit test */
    zst_buffer_t* red2 = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red2) FAIL("buffer allocation");
    if (p0->push(p0, red2) != ZST_OK) FAIL("push sink_0 fit");
    zst_buffer_unref(red2);

    uint8_t pixels_fit[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels_fit);

    /* Under "fit" with 100x200 (portrait) image in 400x200 (landscape) dest:
     * img_aspect = 100/200 = 0.5, dst_aspect = 400/200 = 2.0
     * img_aspect < dst_aspect, so:
     *   nw = h * img_aspect = 200 * 0.5 = 100
     *   x += (400-100)/2 = 150
     * Image rendered at x=150, y=0, w=100, h=200 (centered with pillarbox)
     * Left bar: 0..150, Right bar: 250..400
     */
    /* Left edge of canvas should be black bg (pillarbox) */
    if (!pixel_check(pixels_fit, 400, 200, 10, 100, 0, 0, 0, 255, PIXEL_TOL))
        FAIL("fit: left bar should be black");
    /* Center of image area should be red */
    if (!pixel_check(pixels_fit, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("fit: center should be red");
    /* Right edge should be black bg */
    if (!pixel_check(pixels_fit, 400, 200, 390, 100, 0, 0, 0, 255, PIXEL_TOL))
        FAIL("fit: right bar should be black");
    /* Top of image area should be red */
    if (!pixel_check(pixels_fit, 400, 200, 200, 10, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("fit: top center should be red");

    /* ── Test 3: Crop mode ─────────────────────────────────────────── */
    zst_element_set_property_string(el, "sink_0::scaling", "crop");

    zst_buffer_t* red3 = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red3) FAIL("buffer allocation");
    if (p0->push(p0, red3) != ZST_OK) FAIL("push sink_0 crop");
    zst_buffer_unref(red3);

    uint8_t pixels_crop[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels_crop);

    /* Under "crop" with 100x200 (portrait) in 400x200 (landscape) dest:
     * img_aspect = 0.5, dst_aspect = 2.0, img_aspect < dst_aspect, so:
     *   vis = 0.5 / 2.0 = 0.25
     *   ty0 = (1 - 0.25) / 2 = 0.375
     *   ty1 = 1 - 0.375 = 0.625
     * Texture coordinates (0, 0.375) to (1, 0.625) — middle 25% of image
     * Since the input is uniformly red, the output should also be red everywhere
     */
    if (!pixel_check(pixels_crop, 400, 200, 10,  10,  255, 0, 0, 255, PIXEL_TOL))
        FAIL("crop: top-left should be red");
    if (!pixel_check(pixels_crop, 400, 200, 390, 190, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("crop: bottom-right should be red");
    if (!pixel_check(pixels_crop, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("crop: center should be red");

    zst_buffer_unref(red);
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

/* Saved DISPLAY value restored before running GL-dependent tests */
static char* g_saved_display = NULL;

int main(void)
{
    /* Save the initial DISPLAY so GL-dependent tests can restore it */
    const char* d = getenv("DISPLAY");
    if (d) g_saved_display = strdup(d);

    printf("=== glcompsink element tests ===\n\n");
    zst_result_t reg = zst_register_builtin_elements();
    if (reg != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return 1;
    }

    TEST("Factory creation");                 test_factory_create();
    TEST("Properties and dynamic pads");      test_properties_and_dynamic_pads();
    TEST("Keyboard/Mouse event handling");    test_input_event_handling();
    TEST("Request and release pad API");      test_request_release_pad_api();
    TEST("Null-mode multi-input processing"); test_null_mode_multi_input();
    TEST("EOS per pad");                      test_eos_per_pad();
    TEST("QoS dropping");                     test_qos_dropping();

    /* Restore DISPLAY before running GL-dependent tests */
    if (g_saved_display) setenv("DISPLAY", g_saved_display, 1);

    TEST("Xvfb GL smoke");                    test_xvfb_gl_smoke();
    TEST("Capture: background color");        test_capture_background();
    TEST("Capture: z-order");                 test_capture_z_order();
    TEST("Capture: alpha blending");          test_capture_alpha();
    TEST("Capture: scaling modes");           test_capture_scaling();

    if (g_saved_display) free(g_saved_display);

    printf("\n=== Results: %d passed, %d skipped, %d total ===\n", g_tests_passed, g_tests_skipped, g_tests_run);
    return ((g_tests_passed + g_tests_skipped) == g_tests_run) ? 0 : 1;
}
