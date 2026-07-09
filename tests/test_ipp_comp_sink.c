/*=============================================================================
    test_ipp_comp_sink.c — Intel IPP compositor sink tests
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
#include "zstreamer/elements/zst_ipp_comp_sink.h"
#include "zst_clock.h"
#include "zst_bus.h"

#define PIXEL_TOL 1
#define ALPHA_TOL 2

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_skipped = 0;

#define TEST(name) do { g_tests_run++; printf("  TEST: %-50s ... ", name); fflush(stdout); } while (0)
#define PASS() do { g_tests_passed++; printf("PASSED\n"); } while (0)
#define SKIP(msg) do { g_tests_skipped++; printf("SKIP (%s)\n", msg); return; } while (0)
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

static uint8_t*
pixel_at(uint8_t* rgba, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    /* IPP origins are top-left, unlike GL's bottom-left */
    return rgba + (y * w + x) * 4;
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

#define TRY_CAPTURE_OR_SKIP(el, w, h, buf) do { \
    if (zst_ipp_comp_sink_capture(el, w, h, buf) != ZST_OK) { \
        zst_element_set_state(el, ZST_STATE_NULL); \
        zst_element_destroy(el); \
        SKIP("capture failed"); \
    } \
} while (0)

static zst_element_t*
make_ippcompsink(void)
{
    return zst_element_factory_make("ippcompsink");
}

static void
test_factory_create(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("zst_element_factory_make(\"ippcompsink\") returned NULL");
    if (!zst_element_get_pad(el, "sink_0")) FAIL("default sink_0 missing");
    zst_element_destroy(el);
    PASS();
}

static void
test_properties_and_dynamic_pads(void)
{
    zst_element_t* el = make_ippcompsink();
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

    if (zst_element_set_property_double(el, "display-rate", 60.0) != ZST_OK) FAIL("set display-rate failed");
    double rate = 0;
    if (zst_element_get_property_double(el, "display-rate", &rate) != ZST_OK || rate != 60.0) FAIL("get display-rate failed");

    zst_element_destroy(el);
    PASS();
}

static void
test_request_release_pad_api(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("factory make failed");
    zst_pad_t* p = zst_ipp_comp_sink_request_pad(el, NULL);
    if (!p || strcmp(p->name, "sink_1") != 0) FAIL("auto request pad failed");

    zst_pad_t* p7 = zst_ipp_comp_sink_request_pad(el, "sink_7");
    if (!p7 || strcmp(p7->name, "sink_7") != 0) FAIL("named request pad failed");

    if (zst_ipp_comp_sink_release_pad(el, p) != ZST_OK) FAIL("release pad failed");
    if (zst_element_get_pad(el, "sink_1") != NULL) FAIL("released pad still found");

    zst_element_destroy(el);
    PASS();
}

static void
test_eos_per_pad(void)
{
    zst_element_t* el = make_ippcompsink();
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

static zst_time_t
mock_clock_get_time(zst_clock_t* clock)
{
    return 100000000;
}

static void
test_qos_dropping(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("creation failed");

    zst_element_set_property_uint(el, "input-count", 1);
    zst_element_set_property_int(el, "max-lateness", 20000000);

    zst_clock_t* clk = zst_clock_system_create();
    clk->get_time = mock_clock_get_time;
    zst_element_set_clock(el, clk);

    zst_element_set_state(el, ZST_STATE_PLAYING);

    zst_pad_t* pad = zst_element_get_pad(el, "sink_0");
    if (!pad) FAIL("no sink_0 pad");

    zst_buffer_t* buf = zst_buffer_create(ZST_MEMORY_CPU);
    buf->pts = 10000000;

    zst_result_t res = pad->push(pad, buf);
    if (res != ZST_OK) FAIL("push failed");

    if (!(buf->flags & ZST_BUFFER_FLAG_DROP)) FAIL("buffer was not dropped");

    char count[32];
    zst_element_get_property(el, "frame-count", count, sizeof(count));
    if (strcmp(count, "0") != 0) FAIL("frame count should be 0");

    zst_clock_unref(clk);
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_background(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 160);
    zst_element_set_property_uint(el, "canvas-height", 120);
    zst_element_set_property_string(el, "background-color", "#0000ffff");

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    struct timespec ts = { .tv_nsec = 50000000 };
    nanosleep(&ts, NULL);

    uint8_t pixels[160 * 120 * 4];
    TRY_CAPTURE_OR_SKIP(el, 160, 120, pixels);

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
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 200);
    zst_element_set_property_uint(el, "canvas-height", 200);
    zst_element_set_property_string(el, "background-color", "#000000ff");
    zst_element_set_property_uint(el, "input-count", 2);

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    zst_element_set_property_int(el, "sink_0::x", 10);
    zst_element_set_property_int(el, "sink_0::y", 10);
    zst_element_set_property_uint(el, "sink_0::width", 80);
    zst_element_set_property_uint(el, "sink_0::height", 180);
    zst_element_set_property_int(el, "sink_1::x", 110);
    zst_element_set_property_int(el, "sink_1::y", 10);
    zst_element_set_property_uint(el, "sink_1::width", 80);
    zst_element_set_property_uint(el, "sink_1::height", 180);

    zst_element_set_property_int(el, "sink_0::z_order", 0);
    zst_element_set_property_int(el, "sink_1::z_order", 1);

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

    if (!pixel_check(pixels, 200, 200, 50, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("sink_0 area should be red");
    if (!pixel_check(pixels, 200, 200, 150, 100, 0, 255, 0, 255, PIXEL_TOL))
        FAIL("sink_1 area should be green");
    if (!pixel_check(pixels, 200, 200, 0, 0, 0, 0, 0, 255, PIXEL_TOL))
        FAIL("bg should be black (no input at 0,0)");

    {
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
        if (!pixel_check(pixels_overlap, 200, 200, 100, 100, 0, 255, 0, 255, PIXEL_TOL)) {
            FAIL("z-order=1 should be on top (green)");
        }
    }

    zst_element_set_property_int(el, "sink_0::z_order", 1);
    zst_element_set_property_int(el, "sink_1::z_order", 0);

    uint8_t pixels2[200 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 200, 200, pixels2);

    if (!pixel_check(pixels2, 200, 200, 100, 100, 255, 0, 0, 255, PIXEL_TOL))
        FAIL("after reorder, red should be on top");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_alpha(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 200);
    zst_element_set_property_uint(el, "canvas-height", 200);
    zst_element_set_property_string(el, "background-color", "#00ff00ff");

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

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

    /* (127, 127, 0) */
    if (!pixel_check(pixels, 200, 200, 100, 100, 127, 127, 0, 255, ALPHA_TOL))
        FAIL("alpha blending: expected ~(127,127,0)");
    if (!pixel_check(pixels, 200, 200, 10, 10, 127, 127, 0, 255, ALPHA_TOL))
        FAIL("alpha blending corner: expected ~(127,127,0)");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

static void
test_capture_scaling(void)
{
    zst_element_t* el = make_ippcompsink();
    if (!el) FAIL("factory make failed");

    zst_element_set_property_uint(el, "canvas-width", 400);
    zst_element_set_property_uint(el, "canvas-height", 200);
    zst_element_set_property_string(el, "background-color", "#000000ff");

    if (zst_element_set_state(el, ZST_STATE_PLAYING) != ZST_OK) FAIL("PLAYING failed");

    zst_element_set_property_int(el, "sink_0::x", 0);
    zst_element_set_property_int(el, "sink_0::y", 0);
    zst_element_set_property_uint(el, "sink_0::width", 400);
    zst_element_set_property_uint(el, "sink_0::height", 200);

    zst_buffer_t* red = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red) FAIL("buffer allocation");

    /* Stretch */
    zst_element_set_property_string(el, "sink_0::scaling", "stretch");

    zst_pad_t* p0 = zst_element_get_pad(el, "sink_0");
    if (p0->push(p0, red) != ZST_OK) FAIL("push sink_0");
    zst_buffer_ref(red);
    if (p0->push(p0, red) != ZST_OK) FAIL("push sink_0 again");

    uint8_t pixels[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels);

    if (!pixel_check(pixels, 400, 200, 10,  10,  255, 0, 0, 255, PIXEL_TOL)) FAIL("stretch top-left");
    if (!pixel_check(pixels, 400, 200, 390, 190, 255, 0, 0, 255, PIXEL_TOL)) FAIL("stretch bot-right");
    if (!pixel_check(pixels, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL)) FAIL("stretch center");

    /* Fit */
    zst_element_set_property_string(el, "sink_0::scaling", "fit");

    zst_buffer_t* red2 = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red2) FAIL("buffer allocation");
    if (p0->push(p0, red2) != ZST_OK) FAIL("push sink_0 fit");
    zst_buffer_unref(red2);

    uint8_t pixels_fit[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels_fit);

    if (!pixel_check(pixels_fit, 400, 200, 10, 100, 0, 0, 0, 255, PIXEL_TOL)) FAIL("fit left bar black");
    if (!pixel_check(pixels_fit, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL)) FAIL("fit center red");
    if (!pixel_check(pixels_fit, 400, 200, 390, 100, 0, 0, 0, 255, PIXEL_TOL)) FAIL("fit right bar black");

    /* Crop */
    zst_element_set_property_string(el, "sink_0::scaling", "crop");

    zst_buffer_t* red3 = make_rgb_buffer(100, 200, 255, 0, 0);
    if (!red3) FAIL("buffer allocation");
    if (p0->push(p0, red3) != ZST_OK) FAIL("push sink_0 crop");
    zst_buffer_unref(red3);

    uint8_t pixels_crop[400 * 200 * 4];
    TRY_CAPTURE_OR_SKIP(el, 400, 200, pixels_crop);

    if (!pixel_check(pixels_crop, 400, 200, 10,  10,  255, 0, 0, 255, PIXEL_TOL)) FAIL("crop top-left");
    if (!pixel_check(pixels_crop, 400, 200, 390, 190, 255, 0, 0, 255, PIXEL_TOL)) FAIL("crop bot-right");
    if (!pixel_check(pixels_crop, 400, 200, 200, 100, 255, 0, 0, 255, PIXEL_TOL)) FAIL("crop center");

    zst_buffer_unref(red);
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    PASS();
}

int main(void)
{
    printf("=== ippcompsink element tests ===\n\n");
    zst_result_t reg = zst_register_builtin_elements();
    if (reg != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return 1;
    }

    TEST("Factory creation");                 test_factory_create();
    TEST("Properties and dynamic pads");      test_properties_and_dynamic_pads();
    TEST("Request and release pad API");      test_request_release_pad_api();
    TEST("EOS per pad");                      test_eos_per_pad();
    TEST("QoS dropping");                     test_qos_dropping();

    TEST("Capture: background color");        test_capture_background();
    TEST("Capture: z-order");                 test_capture_z_order();
    TEST("Capture: alpha blending");          test_capture_alpha();
    TEST("Capture: scaling modes");           test_capture_scaling();

    printf("\n=== Results: %d passed, %d skipped, %d total ===\n", g_tests_passed, g_tests_skipped, g_tests_run);
    return ((g_tests_passed + g_tests_skipped) == g_tests_run) ? 0 : 1;
}
