#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

zst_element_t* zst_x265_encoder_create(void);
zst_element_t* zst_video_test_src_create(void);

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name)                                              \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  TEST: %-50s ... ", name);                     \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        g_tests_passed++;                                       \
        printf("PASS\n");                                       \
    } while (0)

static zst_element_t*
create_video_source(int width, int height, int fps, const char* pattern)
{
    zst_element_t* src = zst_video_test_src_create();
    assert(src != NULL);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", width);
    zst_element_set_property(src, "width", buf);
    snprintf(buf, sizeof(buf), "%d", height);
    zst_element_set_property(src, "height", buf);
    snprintf(buf, sizeof(buf), "%d", fps);
    zst_element_set_property(src, "fps", buf);
    zst_element_set_property(src, "pattern", pattern);
    zst_element_set_property(src, "num-buffers", "-1");

    zst_element_set_state(src, ZST_STATE_READY);
    return src;
}

static zst_buffer_t*
generate_video_frame(zst_element_t* src)
{
    zst_buffer_t* out = NULL;
    zst_result_t r = src->ops->process(src, NULL, &out);
    assert(r == ZST_OK);
    assert(out != NULL);
    return out;
}

static void test_x265_basic_encode() {
    TEST("x265 basic encode");
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    zst_buffer_t* in = generate_video_frame(src);
    zst_buffer_t* out = NULL;
    zst_result_t r = enc->ops->process(enc, in, &out);
    assert(r == ZST_OK);
    zst_buffer_unref(in);

    if (out) zst_buffer_unref(out);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

static void test_x265_properties() {
    TEST("x265 properties");
    zst_element_t* enc = zst_x265_encoder_create();

    // Default properties should get set appropriately.
    zst_result_t r;
    r = zst_element_set_property(enc, "preset", "fast");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "tune", "zerolatency");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "crf", "22.5");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "bitrate", "1000000");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "gop-size", "60");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "keyint-min", "30");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "profile", "main");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "fps", "60/1");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "threads", "4");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "bframes", "2");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "vbv-maxrate", "2000000");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "max-slices", "4");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "output-nal-units", "true");
    assert(r == ZST_OK);
    r = zst_element_set_property(enc, "level", "4.0");
    assert(r == ZST_OK);

    char val[64];
    r = zst_element_get_property(enc, "preset", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "fast") == 0);
    r = zst_element_get_property(enc, "tune", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "zerolatency") == 0);
    r = zst_element_get_property(enc, "crf", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "22.50") == 0);
    r = zst_element_get_property(enc, "bitrate", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "1000000") == 0);
    r = zst_element_get_property(enc, "gop-size", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "60") == 0);
    r = zst_element_get_property(enc, "keyint-min", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "30") == 0);
    r = zst_element_get_property(enc, "profile", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "main") == 0);
    r = zst_element_get_property(enc, "fps", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "60/1") == 0);
    r = zst_element_get_property(enc, "threads", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "4") == 0);
    r = zst_element_get_property(enc, "bframes", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "2") == 0);
    r = zst_element_get_property(enc, "vbv-maxrate", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "2000000") == 0);
    r = zst_element_get_property(enc, "max-slices", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "4") == 0);
    r = zst_element_get_property(enc, "output-nal-units", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "true") == 0);
    r = zst_element_get_property(enc, "level", val, sizeof(val));
    assert(r == ZST_OK && strcmp(val, "4.0") == 0);

    // Also test get_property for non-existent properties
    r = zst_element_get_property(enc, "unknown_prop", val, sizeof(val));
    assert(r == ZST_ERROR);
    r = zst_element_set_property(enc, "unknown_prop", val);
    assert(r == ZST_ERROR);

    zst_element_destroy(enc);
    PASS();
}

static void test_x265_eos() {
    TEST("x265 EOS handling");
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    // Push some frames
    for(int i = 0; i < 5; i++) {
        zst_buffer_t* in = generate_video_frame(src);
        zst_buffer_t* out = NULL;
        enc->ops->process(enc, in, &out);
        zst_buffer_unref(in);
        if (out) zst_buffer_unref(out);
    }

    // Push EOS
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* eos_out = NULL;
    enc->ops->process(enc, eos, &eos_out);
    zst_buffer_unref(eos);

    // Must handle all pending frames
    while (eos_out) {
        zst_buffer_unref(eos_out);
        enc->ops->process(enc, NULL, &eos_out);
        break; // Just testing the eos flow does not crash.
    }

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

static void test_x265_event() {
    TEST("x265 event handling (FORCE_KEYFRAME)");
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    zst_pad_event_t event;
    event.type = ZST_PAD_EVENT_FORCE_KEYFRAME;
    zst_pad_t* sink_pad = zst_element_get_pad(enc, "sink");

    zst_result_t r = enc->ops->event(enc, sink_pad, &event);
    assert(r == ZST_OK);

    event.type = ZST_PAD_EVENT_EOS;
    r = enc->ops->event(enc, sink_pad, &event);
    assert(r == ZST_ERROR);

    zst_buffer_t* in = generate_video_frame(src);
    zst_buffer_t* out = NULL;
    r = enc->ops->process(enc, in, &out);
    assert(r == ZST_OK);
    zst_buffer_unref(in);
    if (out) zst_buffer_unref(out);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

static void test_x265_coverage() {
    TEST("x265 coverage tests");
    zst_element_t* enc = zst_x265_encoder_create();

    // Test errors
    zst_result_t r;
    r = zst_element_set_property(enc, "bitrate", "-1");
    r = zst_element_set_property(enc, "gop-size", "-1");
    r = zst_element_set_property(enc, "keyint-min", "-1");
    r = zst_element_set_property(enc, "max-slices", "-1");
    r = zst_element_set_property(enc, "vbv-maxrate", "-1");
    r = zst_element_set_property(enc, "threads", "-1");
    r = zst_element_set_property(enc, "fps", "invalid");

    // get_pool
    enc->ops->get_pool(enc);

    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    // initialize encoder
    zst_buffer_t* in = generate_video_frame(src);
    zst_buffer_t* out = NULL;
    enc->ops->process(enc, in, &out);
    if(out) zst_buffer_unref(out);

    // Properties after initialized
    r = zst_element_set_property(enc, "bitrate", "2000");
    assert(r == ZST_ERROR);

    zst_pad_t* sink_pad = zst_element_get_pad(enc, "sink");
    zst_pad_t* src_pad = zst_element_get_pad(enc, "src");

    // Simulate push properly connected
    // Create a fake peer to receive
    zst_pad_t* fake_sink = zst_pad_create("fake_sink", ZST_PAD_SINK);
    // Fake sink push to do nothing
    zst_result_t fake_push(zst_pad_t* p, zst_buffer_t* b) { return ZST_OK; }
    fake_sink->push = fake_push;
    zst_element_t* fake_element = zst_element_create(NULL, NULL);
    zst_element_add_pad(fake_element, fake_sink);
    zst_pad_link(src_pad, fake_sink);

    zst_buffer_t* in2 = generate_video_frame(src);

    // In order for sink_push to be called, we might need to invoke sink_pad->push manually since zst_pad_push might behave differently.
    // Let's call sink_pad->push explicitly to ensure it runs:
    if (sink_pad->push) {
        sink_pad->push(sink_pad, in2);
    }

    // Also test without peer
    zst_pad_unlink(src_pad);
    zst_buffer_t* in3 = generate_video_frame(src);
    if (sink_pad->push) {
        sink_pad->push(sink_pad, in3);
    }

    // test sink push error conditions
    if (sink_pad->push) {
        sink_pad->push(NULL, in2);
        sink_pad->push(sink_pad, NULL);
        zst_pad_t bad_pad = {0};
        sink_pad->push(&bad_pad, in2);
    }

    zst_buffer_unref(in);
    zst_buffer_unref(in2);
    zst_buffer_unref(in3);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    zst_element_destroy(fake_element);
    PASS();
}

static void test_x265_coverage_extra() {
    TEST("x265 extra coverage");

    // Let's test bitrate > 0
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_property(enc, "bitrate", "5000000");
    zst_element_set_property(enc, "gop-size", "30");
    zst_element_set_property(enc, "keyint-min", "15");
    zst_element_set_property(enc, "max-slices", "2");
    zst_element_set_property(enc, "vbv-maxrate", "6000000");
    zst_element_set_property(enc, "profile", "main");

    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    zst_buffer_t* in = generate_video_frame(src);
    zst_buffer_t* out = NULL;
    enc->ops->process(enc, in, &out);
    zst_buffer_unref(in);
    if(out) zst_buffer_unref(out);

    // push empty EOS through uninitialized
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_element_t* enc2 = zst_x265_encoder_create();
    zst_buffer_t* eos_out = NULL;
    enc2->ops->process(enc2, eos, &eos_out);
    if(eos_out) zst_buffer_unref(eos_out);
    zst_buffer_unref(eos);
    zst_element_destroy(enc2);

    // call process with NULL input
    zst_buffer_t* dummy_out = NULL;
    enc->ops->process(enc, NULL, &dummy_out);
    if (dummy_out) zst_buffer_unref(dummy_out);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

static void test_x265_draining() {
    TEST("x265 draining");
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_property(enc, "bframes", "4"); // large b-frames
    zst_element_set_property(enc, "threads", "1");
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    for(int i=0; i<15; i++) {
        zst_buffer_t* in = generate_video_frame(src);
        zst_buffer_t* out = NULL;
        enc->ops->process(enc, in, &out);
        zst_buffer_unref(in);
        if(out) zst_buffer_unref(out);
    }

    // push eos
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    zst_buffer_t* out = NULL;
    enc->ops->process(enc, eos, &out);
    zst_buffer_unref(eos);

    while(out) {
        zst_buffer_unref(out);
        enc->ops->process(enc, NULL, &out);
    }

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

static void test_x265_nal_units() {
    TEST("x265 output NAL units");
    zst_element_t* enc = zst_x265_encoder_create();
    zst_element_set_property(enc, "output-nal-units", "true");
    zst_element_set_state(enc, ZST_STATE_READY);
    zst_element_t* src = create_video_source(320, 240, 30, "gradient");

    // Push frame
    zst_buffer_t* in = generate_video_frame(src);
    zst_buffer_t* out = NULL;
    zst_result_t r = enc->ops->process(enc, in, &out);
    assert(r == ZST_OK);
    zst_buffer_unref(in);

    if (out) {
        // Just verify it doesn't crash
        zst_buffer_unref(out);
    }

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    PASS();
}

int main() {
    printf("\n=== x265 encoder tests ===\n\n");
    test_x265_properties();
    test_x265_basic_encode();
    test_x265_eos();
    test_x265_event();

    test_x265_coverage();
    test_x265_coverage_extra();
    test_x265_draining();
    test_x265_nal_units();

    printf("\n--- Results: %d/%d passed ---\n\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
