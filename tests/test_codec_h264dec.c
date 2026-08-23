/*============================================================================
    test_codec_h264dec.c — H.264 Decoder Tests
    
    Tests h264dec: basic decode, properties, and pipeline integration.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <libavcodec/avcodec.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"

/* Extern element constructors */
zst_element_t* zst_x264_encoder_create(void);
zst_element_t* zst_h264_decoder_create(void);
zst_element_t* zst_video_test_src_create(void);

static int g_tests_run   = 0;
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

/* ═══════════════════════════════════════════════════════════════
   Helpers
   ═══════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════
   Test 1: Basic decode — encode with x264enc, decode with h264dec
   using process() directly, verify dimensions and format
   ═══════════════════════════════════════════════════════════════ */

static void
test_h264_decode_basic(void)
{
    TEST("h264 decoder decodes x264 bitstream correctly");

    const int W = 320, H = 240, NUM_FRAMES = 15;

    zst_element_t* src = create_video_source(W, H, 30, "gradient");
    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    zst_element_t* dec = zst_h264_decoder_create();
    assert(dec != NULL);
    zst_element_set_state(dec, ZST_STATE_READY);

    int decoded_count = 0;
    uint32_t last_w = 0, last_h = 0, last_fmt = 0;

    /* Encode then decode each frame via process() directly */
    for (int i = 0; i < NUM_FRAMES; i++) {
        zst_buffer_t* raw = generate_video_frame(src);
        zst_buffer_t* pkt = NULL;
        zst_result_t r = enc->ops->process(enc, raw, &pkt);
        assert(r == ZST_OK);
        zst_buffer_unref(raw);

        if (pkt) {
            /* Decode */
            zst_buffer_t* dec_out = NULL;
            zst_result_t dr = dec->ops->process(dec, pkt, &dec_out);
            assert(dr == ZST_OK || dr == ZST_AGAIN);
            zst_buffer_unref(pkt);

            /* Collect decoded frame */
            if (dec_out && dec_out->type == ZST_BUFFER_VIDEO_FRAME && dec_out->payload) {
                zst_video_frame_t* f = dec_out->payload;
                decoded_count++;
                last_w = f->width;
                last_h = f->height;
                last_fmt = f->format;
            }
            if (dec_out) zst_buffer_unref(dec_out);
        }
    }

    /* Flush encoder */
    {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        zst_buffer_t* eos_pkt = NULL;
        enc->ops->process(enc, eos, &eos_pkt);
        zst_buffer_unref(eos);
        if (eos_pkt) {
            zst_buffer_t* dec_out = NULL;
            dec->ops->process(dec, eos_pkt, &dec_out);
            if (dec_out && dec_out->type == ZST_BUFFER_VIDEO_FRAME && dec_out->payload) {
                zst_video_frame_t* f = dec_out->payload;
                decoded_count++;
                last_w = f->width;
                last_h = f->height;
                last_fmt = f->format;
            }
            if (dec_out) zst_buffer_unref(dec_out);
            zst_buffer_unref(eos_pkt);
        }
    }

    /* Flush decoder */
    {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        zst_buffer_t* dec_out = NULL;
        zst_result_t dr = dec->ops->process(dec, eos, &dec_out);
        (void)dr;
        zst_buffer_unref(eos);
        if (dec_out && dec_out->type == ZST_BUFFER_VIDEO_FRAME && dec_out->payload) {
            zst_video_frame_t* f = dec_out->payload;
            decoded_count++;
            last_w = f->width;
            last_h = f->height;
            last_fmt = f->format;
        }
        if (dec_out) zst_buffer_unref(dec_out);
    }

    assert(decoded_count >= 1);
    assert(last_w == (uint32_t)W);
    assert(last_h == (uint32_t)H);
    assert(last_fmt == 0); /* AV_PIX_FMT_YUV420P */

    printf("\n        `-- decoded %d frames, %ux%u, format=%u\n",
           decoded_count, last_w, last_h, last_fmt);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(dec, ZST_STATE_NULL);
    zst_element_destroy(src);
    zst_element_destroy(enc);
    zst_element_destroy(dec);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Test 2: Property get/set roundtrip
   ═══════════════════════════════════════════════════════════════ */

static void
test_h264_decode_properties(void)
{
    TEST("h264 decoder property get/set roundtrip");

    zst_element_t* dec = zst_h264_decoder_create();
    assert(dec != NULL);

    char val[64];
    int64_t ival;

    /* threads (default 0 = auto) */
    assert(zst_element_get_property(dec, "threads", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "0") == 0);
    assert(zst_element_set_property(dec, "threads", "4") == ZST_OK);
    assert(zst_element_get_property(dec, "threads", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "4") == 0);
    assert(zst_element_set_property_int(dec, "threads", 2) == ZST_OK);
    assert(zst_element_get_property_int(dec, "threads", &ival) == ZST_OK);
    assert(ival == 2);
    /* negative => clamped to 0 */
    assert(zst_element_set_property(dec, "threads", "-1") == ZST_OK);
    assert(zst_element_get_property_int(dec, "threads", &ival) == ZST_OK);
    assert(ival == 0);

    /* low-latency (default false) */
    assert(zst_element_get_property(dec, "low-latency", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "false") == 0);
    assert(zst_element_set_property(dec, "low-latency", "true") == ZST_OK);
    assert(zst_element_get_property(dec, "low-latency", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "true") == 0);
    assert(zst_element_set_property(dec, "low-latency", "1") == ZST_OK);
    assert(zst_element_get_property(dec, "low-latency", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "true") == 0);
    assert(zst_element_set_property(dec, "low-latency", "false") == ZST_OK);
    assert(zst_element_get_property(dec, "low-latency", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "false") == 0);
    assert(zst_element_set_property(dec, "low-latency", "0") == ZST_OK);
    assert(zst_element_get_property(dec, "low-latency", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "false") == 0);

    /* unknown property */
    assert(zst_element_set_property(dec, "nonexistent", "42") == ZST_ERROR);
    assert(zst_element_get_property(dec, "nonexistent", val, sizeof(val)) == ZST_ERROR);

    /* null guards */
    assert(zst_element_set_property(dec, NULL, "1") == ZST_ERROR);
    assert(zst_element_set_property(dec, "threads", NULL) == ZST_ERROR);
    assert(zst_element_get_property(dec, NULL, val, sizeof(val)) == ZST_ERROR);
    assert(zst_element_get_property(dec, "threads", NULL, sizeof(val)) == ZST_ERROR);
    assert(zst_element_get_property(dec, "threads", val, 0) == ZST_ERROR);

    zst_element_set_state(dec, ZST_STATE_NULL);
    zst_element_destroy(dec);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Test 3: Pipeline — enc→dec via process(), verify caps
   ═══════════════════════════════════════════════════════════════ */

static void
test_h264_decode_pipeline(void)
{
    TEST("h264 decode pipeline (enc -> dec -> caps verification)");

    const int W = 352, H = 288, NUM_FRAMES = 10;

    zst_element_t* src = create_video_source(W, H, 30, "bars");

    zst_element_t* enc = zst_x264_encoder_create();
    assert(enc != NULL);
    zst_element_set_state(enc, ZST_STATE_READY);

    zst_element_t* dec = zst_h264_decoder_create();
    assert(dec != NULL);
    zst_element_set_property(dec, "threads", "1");
    zst_element_set_state(dec, ZST_STATE_READY);

    int decoded_count = 0;

    for (int i = 0; i < NUM_FRAMES; i++) {
        zst_buffer_t* raw = generate_video_frame(src);
        zst_buffer_t* pkt = NULL;
        zst_result_t r = enc->ops->process(enc, raw, &pkt);
        assert(r == ZST_OK);
        zst_buffer_unref(raw);

        if (pkt) {
            zst_buffer_t* dec_out = NULL;
            dec->ops->process(dec, pkt, &dec_out);
            if (dec_out) {
                if (dec_out->type == ZST_BUFFER_VIDEO_FRAME) decoded_count++;
                zst_buffer_unref(dec_out);
            }
            zst_buffer_unref(pkt);
        }
    }

    /* Flush encoder */
    {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        zst_buffer_t* pkt = NULL;
        enc->ops->process(enc, eos, &pkt);
        zst_buffer_unref(eos);
        if (pkt) {
            zst_buffer_t* dec_out = NULL;
            dec->ops->process(dec, pkt, &dec_out);
            if (dec_out) {
                if (dec_out->type == ZST_BUFFER_VIDEO_FRAME) decoded_count++;
                zst_buffer_unref(dec_out);
            }
            zst_buffer_unref(pkt);
        }
    }

    /* Flush decoder */
    {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        eos->flags |= ZST_BUFFER_FLAG_EOS;
        zst_buffer_t* dec_out = NULL;
        dec->ops->process(dec, eos, &dec_out);
        zst_buffer_unref(eos);
        if (dec_out) {
            if (dec_out->type == ZST_BUFFER_VIDEO_FRAME) decoded_count++;
            zst_buffer_unref(dec_out);
        }
    }

    assert(decoded_count >= 1);

    /* Verify decoder caps after decoding */
    zst_caps_t* caps = zst_pad_get_caps(dec->src_pads[0]);
    assert(caps != NULL && caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "video/x-raw") == 0);
    assert(caps->structs->video.width == W);
    assert(caps->structs->video.height == H);
    zst_caps_destroy(caps);

    printf("\n        `-- decoded %d/%d frames, caps: %ux%u\n",
           decoded_count, NUM_FRAMES, W, H);

    zst_element_set_state(enc, ZST_STATE_NULL);
    zst_element_set_state(dec, ZST_STATE_NULL);
    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);
    zst_element_destroy(enc);
    zst_element_destroy(dec);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n=== zstreamer h264 decoder tests ===\n\n");

    test_h264_decode_basic();
    test_h264_decode_properties();
    test_h264_decode_pipeline();

    printf("\n--- Results: %d/%d passed ---\n\n",
           g_tests_passed, g_tests_run);

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
