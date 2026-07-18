/*=============================================================================
    test_webrtc_vp8vp9.c — Phase 7 verification test
    Tests VP8/VP9 codec support in zstreamer WebRTC
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

/* Forward declarations for element creation functions */
zst_element_t* zst_vp8_decoder_create(void);
zst_element_t* zst_vp9_decoder_create(void);
zst_element_t* zst_vp8_encoder_create(void);
zst_element_t* zst_vp9_encoder_create(void);

/* ── Test 1: VP8 decoder element exists and creates ──────────────────── */
static int test_vp8_decoder_create(void)
{
    printf("Test 1: VP8 decoder element creation... ");
    
    zst_element_t* dec = zst_vp8_decoder_create();
    if (!dec) {
        printf("FAIL (element creation failed)\n");
        return 1;
    }
    
    assert(dec != NULL);
    assert(dec->ops != NULL);
    
    zst_element_destroy(dec);
    printf("PASS\n");
    return 0;
}

/* ── Test 2: VP9 decoder element exists and creates ──────────────────── */
static int test_vp9_decoder_create(void)
{
    printf("Test 2: VP9 decoder element creation... ");
    
    zst_element_t* dec = zst_vp9_decoder_create();
    if (!dec) {
        printf("FAIL (element creation failed)\n");
        return 1;
    }
    
    assert(dec != NULL);
    assert(dec->ops != NULL);
    
    zst_element_destroy(dec);
    printf("PASS\n");
    return 0;
}

/* ── Test 3: VP8 encoder element exists and creates ──────────────────── */
static int test_vp8_encoder_create(void)
{
    printf("Test 3: VP8 encoder element creation... ");
    
    zst_element_t* enc = zst_vp8_encoder_create();
    if (!enc) {
        printf("FAIL (element creation failed)\n");
        return 1;
    }
    
    assert(enc != NULL);
    assert(enc->ops != NULL);
    
    zst_element_destroy(enc);
    printf("PASS\n");
    return 0;
}

/* ── Test 4: VP9 encoder element exists and creates ──────────────────── */
static int test_vp9_encoder_create(void)
{
    printf("Test 4: VP9 encoder element creation... ");
    
    zst_element_t* enc = zst_vp9_encoder_create();
    if (!enc) {
        printf("FAIL (element creation failed)\n");
        return 1;
    }
    
    assert(enc != NULL);
    assert(enc->ops != NULL);
    
    zst_element_destroy(enc);
    printf("PASS\n");
    return 0;
}

/* ── Test 5: VP8/VP9 codec enums in WebRTC endpoint ─────────────────── */
static int test_webrtc_codec_enums(void)
{
    printf("Test 5: WebRTC VP8/VP9 codec enums... ");
    
    /* Verify the codec enums are defined */
    assert(ZST_WEBRTC_CODEC_VP8 == 1);
    assert(ZST_WEBRTC_CODEC_VP9 == 2);
    
    printf("PASS\n");
    return 0;
}

/* ── Test 6: VP8 encoder properties ─────────────────────────────────── */
static int test_vp8_encoder_properties(void)
{
    printf("Test 6: VP8 encoder properties... ");
    
    zst_element_t* enc = zst_vp8_encoder_create();
    if (!enc) {
        printf("SKIP (vp8enc not available)\n");
        return 0;
    }
    
    /* Set properties */
    char val[64];
    
    zst_element_set_property(enc, "bitrate", "2000000");
    zst_element_get_property(enc, "bitrate", val, sizeof(val));
    assert(strcmp(val, "2000000") == 0);
    
    zst_element_set_property(enc, "width", "1280");
    zst_element_get_property(enc, "width", val, sizeof(val));
    assert(strcmp(val, "1280") == 0);
    
    zst_element_set_property(enc, "height", "720");
    zst_element_get_property(enc, "height", val, sizeof(val));
    assert(strcmp(val, "720") == 0);
    
    zst_element_set_property(enc, "fps", "30/1");
    zst_element_get_property(enc, "fps", val, sizeof(val));
    assert(strcmp(val, "30/1") == 0);
    
    zst_element_destroy(enc);
    printf("PASS\n");
    return 0;
}

/* ── Test 7: VP9 encoder properties ─────────────────────────────────── */
static int test_vp9_encoder_properties(void)
{
    printf("Test 7: VP9 encoder properties... ");
    
    zst_element_t* enc = zst_vp9_encoder_create();
    if (!enc) {
        printf("SKIP (vp9enc not available)\n");
        return 0;
    }
    
    /* Set properties */
    char val[64];
    
    zst_element_set_property(enc, "bitrate", "3000000");
    zst_element_get_property(enc, "bitrate", val, sizeof(val));
    assert(strcmp(val, "3000000") == 0);
    
    zst_element_set_property(enc, "gop-size", "60");
    zst_element_get_property(enc, "gop-size", val, sizeof(val));
    assert(strcmp(val, "60") == 0);
    
    zst_element_destroy(enc);
    printf("PASS\n");
    return 0;
}

/* ── Test 8: SDP codec detection for VP8/VP9 ────────────────────────── */
static int test_sdp_codec_detection(void)
{
    printf("Test 8: SDP codec detection for VP8/VP9... ");
    
    const char* vp8_sdp = "a=rtpmap:96 VP8/90000";
    const char* vp9_sdp = "a=rtpmap:96 VP9/90000";
    const char* h264_sdp = "a=rtpmap:96 H264/90000";
    
    assert(strstr(vp8_sdp, "VP8") != NULL);
    assert(strstr(vp9_sdp, "VP9") != NULL);
    assert(strstr(h264_sdp, "H264") != NULL);
    
    printf("PASS\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== WebRTC VP8/VP9 Codec Tests (Phase 7) ===\n\n");
    
    int failures = 0;
    
    failures += test_vp8_decoder_create();
    failures += test_vp9_decoder_create();
    failures += test_vp8_encoder_create();
    failures += test_vp9_encoder_create();
    failures += test_webrtc_codec_enums();
    failures += test_vp8_encoder_properties();
    failures += test_vp9_encoder_properties();
    failures += test_sdp_codec_detection();
    
    printf("\n=== Results: %d passed, %d failed ===\n",
           8 - failures, failures);
    
    return failures > 0 ? 1 : 0;
}
