/*=============================================================================
    test_x265enc.c — Unit tests for x265enc element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_x265_encoder.h"

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

static void
test_x265enc_creation_and_factory(void)
{
    TEST("x265enc creation and factory");

    /* Direct constructor */
    zst_element_t* enc1 = zst_x265_encoder_create();
    assert(enc1 != NULL);
    assert(strcmp(enc1->ops->name, "x265enc") == 0);
    assert(enc1->nb_sink_pads == 1);
    assert(enc1->nb_src_pads == 1);
    zst_element_destroy(enc1);

    /* Factory lookup */
    zst_register_builtin_elements();
    zst_element_t* enc2 = zst_element_factory_make("x265enc");
    assert(enc2 != NULL);
    assert(strcmp(enc2->ops->name, "x265enc") == 0);
    zst_element_destroy(enc2);

    PASS();
}

static void
test_x265enc_properties(void)
{
    TEST("x265enc properties (preset, tune, crf)");

    zst_element_t* enc = zst_x265_encoder_create();
    assert(enc != NULL);

    zst_result_t res;

    /* 1. preset */
    res = zst_element_set_property_string(enc, "preset", "fast");
    assert(res == ZST_OK);

    char preset[32] = {0};
    res = zst_element_get_property_string(enc, "preset", preset, sizeof(preset));
    assert(res == ZST_OK);
    assert(strcmp(preset, "fast") == 0);

    /* 2. tune */
    res = zst_element_set_property_string(enc, "tune", "zerolatency");
    assert(res == ZST_OK);

    char tune[32] = {0};
    res = zst_element_get_property_string(enc, "tune", tune, sizeof(tune));
    assert(res == ZST_OK);
    assert(strcmp(tune, "zerolatency") == 0);

    /* 3. crf */
    res = zst_element_set_property_double(enc, "crf", 28.5);
    assert(res == ZST_OK);

    double crf = 0.0;
    res = zst_element_get_property_double(enc, "crf", &crf);
    assert(res == ZST_OK);
    assert(crf > 28.4 && crf < 28.6);

    /* Invalid property set/get */
    res = zst_element_set_property_string(enc, "invalid_prop", "val");
    assert(res == ZST_ERROR);

    char val[32] = {0};
    res = zst_element_get_property_string(enc, "invalid_prop", val, sizeof(val));
    assert(res == ZST_ERROR);

    zst_element_destroy(enc);

    PASS();
}

static void
test_x265enc_process_and_draining(void)
{
    TEST("x265enc process frames, keyframe event & EOS drain");

    zst_element_t* vsrc = zst_video_test_src_create();
    assert(vsrc != NULL);
    zst_element_set_property_string(vsrc, "width", "320");
    zst_element_set_property_string(vsrc, "height", "240");
    zst_element_set_property_string(vsrc, "fps", "30");
    zst_element_set_state(vsrc, ZST_STATE_READY);

    zst_element_t* enc = zst_x265_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    /* Test property modification after initialization is forbidden */
    /* First frame initializes encoder */
    zst_buffer_t* in_frame1 = NULL;
    zst_result_t r_src = vsrc->ops->process(vsrc, NULL, &in_frame1);
    assert(r_src == ZST_OK && in_frame1 != NULL);

    zst_buffer_t* out1 = NULL;
    zst_result_t r_enc1 = enc->ops->process(enc, in_frame1, &out1);
    assert(r_enc1 == ZST_OK);
    zst_buffer_unref(in_frame1);
    if (out1) zst_buffer_unref(out1);

    /* Property set should now fail because encoder is initialized */
    zst_result_t r_mod = zst_element_set_property_string(enc, "preset", "medium");
    assert(r_mod == ZST_ERROR);

    /* Test get_pool */
    zst_buffer_pool_t* pool = enc->ops->get_pool(enc);
    assert(pool != NULL);

    /* Test force keyframe event */
    zst_pad_event_t kf_event = { .type = ZST_PAD_EVENT_FORCE_KEYFRAME };
    zst_result_t r_event = enc->ops->event(enc, enc->sink_pads[0], &kf_event);
    assert(r_event == ZST_OK);

    /* Test invalid event */
    zst_pad_event_t inv_event = { .type = (zst_pad_event_type_t)999 };
    zst_result_t r_inv_event = enc->ops->event(enc, enc->sink_pads[0], &inv_event);
    assert(r_inv_event == ZST_ERROR);

    /* Process several more frames */
    for (int i = 0; i < 5; i++) {
        zst_buffer_t* in_f = NULL;
        vsrc->ops->process(vsrc, NULL, &in_f);
        assert(in_f != NULL);

        zst_buffer_t* out_f = NULL;
        zst_result_t r = enc->ops->process(enc, in_f, &out_f);
        assert(r == ZST_OK);
        zst_buffer_unref(in_f);
        if (out_f) {
            assert(out_f->memory.data != NULL);
            assert(out_f->memory.size > 0);
            zst_buffer_unref(out_f);
        }
    }

    /* Test EOS buffer processing & draining */
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(eos_buf != NULL);
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;

    zst_buffer_t* eos_out = NULL;
    zst_result_t r_eos = enc->ops->process(enc, eos_buf, &eos_out);
    assert(r_eos == ZST_OK);
    zst_buffer_unref(eos_buf);

    int found_eos = 0;
    while (eos_out) {
        if (eos_out->flags & ZST_BUFFER_FLAG_EOS) {
            found_eos = 1;
        }
        zst_buffer_unref(eos_out);
        eos_out = NULL;
        enc->ops->process(enc, NULL, &eos_out);
    }
    assert(found_eos == 1);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);

    zst_element_set_state(vsrc, ZST_STATE_NULL);
    zst_element_destroy(vsrc);

    PASS();
}

static zst_result_t
test_sink_dummy_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)el;
    (void)in;
    if (out) *out = NULL;
    return ZST_OK;
}

static void
test_x265enc_sink_push_and_error_handling(void)
{
    TEST("x265enc sink_push and error handling paths");

    zst_element_t* enc = zst_x265_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    zst_pad_t* sink_pad = zst_element_get_pad(enc, "sink");
    zst_pad_t* src_pad  = zst_element_get_pad(enc, "src");
    assert(sink_pad != NULL && src_pad != NULL);

    /* 1. NULL arguments to sink_push */
    assert(sink_pad->push(NULL, NULL) == ZST_ERROR);
    assert(sink_pad->push(sink_pad, NULL) == ZST_ERROR);

    /* 2. NULL out parameter in x265_process */
    assert(enc->ops->process(enc, NULL, NULL) == ZST_ERROR);

    /* 3. NULL input buffer without pending packets */
    zst_buffer_t* dummy_out = NULL;
    assert(enc->ops->process(enc, NULL, &dummy_out) == ZST_ERROR);

    /* 4. Buffer with NULL payload */
    zst_buffer_t* empty_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(empty_buf != NULL);
    assert(enc->ops->process(enc, empty_buf, &dummy_out) == ZST_ERROR);
    zst_buffer_unref(empty_buf);

    /* 5. Create a downstream element and link to test pad_push in x265_sink_push */
    zst_element_ops_t dummy_ops = {
        .name = "dummy_sink",
        .process = test_sink_dummy_process
    };
    zst_element_t* dummy_elem = zst_element_create(&dummy_ops, NULL);
    zst_pad_t* dummy_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(dummy_elem, dummy_pad);

    assert(zst_pad_link(src_pad, dummy_pad) == ZST_OK);

    /* Push valid frame via sink_pad->push */
    zst_element_t* vsrc = zst_video_test_src_create();
    zst_element_set_property_string(vsrc, "width", "160");
    zst_element_set_property_string(vsrc, "height", "120");
    zst_element_set_state(vsrc, ZST_STATE_READY);

    zst_buffer_t* frame_buf = NULL;
    vsrc->ops->process(vsrc, NULL, &frame_buf);
    assert(frame_buf != NULL);

    zst_result_t push_res = sink_pad->push(sink_pad, frame_buf);
    assert(push_res == ZST_OK);
    zst_buffer_unref(frame_buf);

    /* Push EOS via sink_pad->push to flush through linked pad */
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
    assert(sink_pad->push(sink_pad, eos_buf) == ZST_OK);
    zst_buffer_unref(eos_buf);

    zst_element_set_state(vsrc, ZST_STATE_NULL);
    zst_element_destroy(vsrc);
    zst_element_destroy(dummy_elem);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_destroy(enc);

    PASS();
}

int main(void)
{
    printf("\n=== x265enc unit tests ===\n\n");

    test_x265enc_creation_and_factory();
    test_x265enc_properties();
    test_x265enc_process_and_draining();
    test_x265enc_sink_push_and_error_handling();

    printf("\n--- Results: %d/%d passed ---\n\n",
           g_tests_passed, g_tests_run);

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
