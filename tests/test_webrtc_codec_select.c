/*=============================================================================
    test_webrtc_codec_select.c — Phase 8f: Receiver-Side Codec Selection

    Verifies that zst_webrtc_select_codecs() correctly:
      1. Parses multi-codec SDP offers (Chrome-like VP8+H264).
      2. Selects the preferred codec (H264 by default).
      3. Rewrites the m= line to include only the selected payload type.
      4. Strips rtpmap/fmtp/rtcp-fb lines for non-selected codecs.
      5. Respects user-provided codec preference override.
      6. Exposes selected codec via element properties.
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

/*── Realistic Chrome-like SDP offer with multiple video codecs ──────────────*/
static const char* g_chrome_sdp_offer =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:abc123\r\n"
    "a=ice-pwd:def456ghi789\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "a=rtpmap:97 H264/90000\r\n"
    "a=fmtp:97 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtcp-fb:97 nack\r\n"
    "a=rtcp-fb:97 nack pli\r\n"
    "a=rtpmap:98 VP9/90000\r\n"
    "a=rtcp-fb:98 nack\r\n"
    "a=rtcp-fb:98 nack pli\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:abc123\r\n"
    "a=ice-pwd:def456ghi789\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "";

/* ── Test 1: Default preference (H264 > VP8 > VP9) ─────────────────────── */
static void test_default_preference(void)
{
    printf("  test_default_preference...\n");

    char video[32] = {0};
    char audio[32] = {0};
    char* result = zst_webrtc_select_codecs(
        g_chrome_sdp_offer, NULL,
        video, sizeof(video),
        audio, sizeof(audio));

    assert(result != NULL);

    /* H264 should be selected (it's the default #1 preference) */
    assert(strcmp(video, "H264") == 0);
    printf("    Selected video codec: %s [PASS]\n", video);

    /* opus should be selected */
    assert(strcmp(audio, "opus") == 0);
    printf("    Selected audio codec: %s [PASS]\n", audio);

    /* m=video line should only contain pt 97 (H264) */
    assert(strstr(result, "m=video 9 UDP/TLS/RTP/SAVPF 97\r\n") != NULL);
    printf("    m=video rewritten to pt=97 only [PASS]\n");

    /* m=audio line should only contain pt 111 (opus) */
    assert(strstr(result, "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n") != NULL);
    printf("    m=audio rewritten to pt=111 only [PASS]\n");

    /* rtpmap for H264 (pt 97) should be present */
    assert(strstr(result, "a=rtpmap:97 H264/90000") != NULL);
    printf("    a=rtpmap:97 H264 preserved [PASS]\n");

    /* rtpmap for VP8 (pt 96) should be removed */
    assert(strstr(result, "a=rtpmap:96") == NULL);
    printf("    a=rtpmap:96 VP8 removed [PASS]\n");

    /* rtpmap for VP9 (pt 98) should be removed */
    assert(strstr(result, "a=rtpmap:98") == NULL);
    printf("    a=rtpmap:98 VP9 removed [PASS]\n");

    /* fmtp for H264 (pt 97) should be preserved */
    assert(strstr(result, "a=fmtp:97") != NULL);
    printf("    a=fmtp:97 preserved [PASS]\n");

    /* rtcp-fb for VP8 (pt 96) should be removed */
    assert(strstr(result, "a=rtcp-fb:96") == NULL);
    printf("    a=rtcp-fb:96 removed [PASS]\n");

    /* rtcp-fb for H264 (pt 97) should be preserved */
    assert(strstr(result, "a=rtcp-fb:97") != NULL);
    printf("    a=rtcp-fb:97 preserved [PASS]\n");

    /* PCMU (pt 0) rtpmap should be removed from audio */
    assert(strstr(result, "a=rtpmap:0 PCMU") == NULL);
    printf("    a=rtpmap:0 PCMU removed [PASS]\n");

    /* opus fmtp should be preserved */
    assert(strstr(result, "a=fmtp:111") != NULL);
    printf("    a=fmtp:111 opus preserved [PASS]\n");

    /* Session-level attributes should be preserved */
    assert(strstr(result, "a=group:BUNDLE") != NULL);
    printf("    Session-level BUNDLE preserved [PASS]\n");

    free(result);
    printf("  [PASS] test_default_preference\n");
}

/* ── Test 2: User preference override (VP8 preferred) ───────────────────── */
static void test_user_preference(void)
{
    printf("  test_user_preference...\n");

    char video[32] = {0};
    char audio[32] = {0};
    char* result = zst_webrtc_select_codecs(
        g_chrome_sdp_offer, "VP8,H264,VP9",
        video, sizeof(video),
        audio, sizeof(audio));

    assert(result != NULL);

    /* VP8 should be selected when it's the top preference */
    assert(strcmp(video, "VP8") == 0);
    printf("    Selected video codec: %s [PASS]\n", video);

    /* m=video line should only contain pt 96 (VP8) */
    assert(strstr(result, "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n") != NULL);
    printf("    m=video rewritten to pt=96 only [PASS]\n");

    /* H264 lines should be removed */
    assert(strstr(result, "a=rtpmap:97") == NULL);
    printf("    H264 rtpmap removed [PASS]\n");

    free(result);
    printf("  [PASS] test_user_preference\n");
}

/* ── Test 3: Single-codec section passes through unchanged ──────────────── */
static void test_single_codec_passthrough(void)
{
    printf("  test_single_codec_passthrough...\n");

    const char* single_sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "";

    char video[32] = {0};
    char* result = zst_webrtc_select_codecs(
        single_sdp, NULL,
        video, sizeof(video),
        NULL, 0);

    assert(result != NULL);

    /* H264 still selected */
    assert(strcmp(video, "H264") == 0);
    printf("    Selected video codec: %s [PASS]\n", video);

    /* Single-codec section should not be rewritten (num_codecs == 1) */
    assert(strstr(result, "a=rtpmap:96 H264/90000") != NULL);
    printf("    Single codec rtpmap preserved [PASS]\n");

    free(result);
    printf("  [PASS] test_single_codec_passthrough\n");
}

/* ── Test 4: VP9 preferred but not offered → fallback ───────────────────── */
static void test_fallback_codec(void)
{
    printf("  test_fallback_codec...\n");

    /* Offer only VP8 and H264, no VP9 */
    const char* no_vp9_sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "";

    char video[32] = {0};
    /* Prefer VP9 first, then H264, then VP8 */
    char* result = zst_webrtc_select_codecs(
        no_vp9_sdp, "VP9,H264,VP8",
        video, sizeof(video),
        NULL, 0);

    assert(result != NULL);

    /* VP9 not offered, so H264 (next in pref) should be selected */
    assert(strcmp(video, "H264") == 0);
    printf("    Fallback selected: %s [PASS]\n", video);

    free(result);
    printf("  [PASS] test_fallback_codec\n");
}

/* ── Test 5: Element property integration ───────────────────────────────── */
extern zst_result_t zst_register_builtin_elements(void);

static void test_element_properties(void)
{
    printf("  test_element_properties...\n");

    assert(zst_register_builtin_elements() == ZST_OK);

    zst_element_t* el = zst_element_factory_make("webrtc_endpoint");
    assert(el != NULL);

    /* Set codec preference before opening */
    assert(zst_element_set_property(el, "codec-preference", "VP8,H264") == ZST_OK);

    char val[256] = {0};
    assert(zst_element_get_property(el, "codec-preference", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "VP8,H264") == 0);
    printf("    codec-preference property: %s [PASS]\n", val);

    /* Initially, selected codecs should be empty */
    assert(zst_element_get_property(el, "selected-video-codec", val, sizeof(val)) == ZST_OK);
    assert(val[0] == '\0');
    printf("    selected-video-codec initially empty [PASS]\n");

    assert(zst_element_get_property(el, "selected-audio-codec", val, sizeof(val)) == ZST_OK);
    assert(val[0] == '\0');
    printf("    selected-audio-codec initially empty [PASS]\n");

    /* Transition to NULL to trigger close/cleanup, then destroy */
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    printf("  [PASS] test_element_properties\n");
}

/* ── Test 6: NULL/empty SDP handling ────────────────────────────────────── */
static void test_null_sdp(void)
{
    printf("  test_null_sdp...\n");

    char* result = zst_webrtc_select_codecs(NULL, NULL, NULL, 0, NULL, 0);
    assert(result == NULL);
    printf("    NULL SDP returns NULL [PASS]\n");

    printf("  [PASS] test_null_sdp\n");
}

int main(void)
{
    zst_log_set_level(ZST_LOG_LEVEL_INFO);
    printf("=== WebRTC Codec Selection Unit Test (Phase 8f) ===\n");

    test_default_preference();
    test_user_preference();
    test_single_codec_passthrough();
    test_fallback_codec();
    test_element_properties();
    test_null_sdp();

    printf("\n=== All Phase 8f codec selection tests passed ===\n");
    return 0;
}
