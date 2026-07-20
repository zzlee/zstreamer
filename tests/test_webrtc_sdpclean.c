/*=============================================================================
    test_webrtc_sdpclean.c — Phase 8b verification test

    Tests SDP filtering/cleaning:
    - Passes an SDP with TWCC, abs-send-time, playout-delay, ssrc-audio-level,
      and other unsupported extensions through zst_webrtc_filter_sdp().
    - Verifies that these unsupported extensions are successfully removed.
    - Verifies that supported extensions (like mid, rtp-stream-id) are preserved.
    - Verifies that RTCP feedback lines with transport-cc are removed.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "zst_log.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

static const char* TEST_SDP =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE video audio\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:video\r\n"
    "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
    "a=extmap:2 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id\r\n"
    "a=extmap:3 urn:ietf:params:rtp-hdrext:transport-wide-cc-02\r\n"
    "a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
    "a=extmap:5 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "a=rtcp-fb:96 transport-cc\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
    "a=mid:audio\r\n"
    "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
    "a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
    "a=rtcp-fb:97 transport-cc\r\n"
    "a=rtcp-fb:97 nack\r\n";

static const char* TEST_COMPAT_INPUT_SDP =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:video\r\n"
    "a=ssrc:12345 cname:some-random-uuid\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:audio\r\n";

int main(int argc, char* argv[])
{
    (void)argc; (void)argv;
    zst_log_set_level(ZST_LOG_LEVEL_INFO);

    printf("Starting SDP clean/filter test...\n");

    char* filtered = zst_webrtc_filter_sdp(TEST_SDP);
    assert(filtered != NULL);

    printf("--- FILTERED SDP OUTPUT ---\n%s\n---------------------------\n", filtered);

    // Verify removed lines
    assert(strstr(filtered, "transport-wide-cc-02") == NULL);
    assert(strstr(filtered, "abs-send-time") == NULL);
    assert(strstr(filtered, "ssrc-audio-level") == NULL);
    assert(strstr(filtered, "playout-delay") == NULL);
    assert(strstr(filtered, "transport-cc") == NULL);

    // Verify preserved lines
    assert(strstr(filtered, "v=0") != NULL);
    assert(strstr(filtered, "a=group:BUNDLE") != NULL);
    assert(strstr(filtered, "a=mid:video") != NULL);
    assert(strstr(filtered, "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid") != NULL);
    assert(strstr(filtered, "a=extmap:2 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id") != NULL);
    assert(strstr(filtered, "a=rtcp-fb:96 nack") != NULL);
    assert(strstr(filtered, "a=rtcp-fb:96 nack pli") != NULL);
    assert(strstr(filtered, "a=rtcp-fb:97 nack") != NULL);

    free(filtered);

    printf("Starting local SDP compatibility test...\n");
    char* compat = zst_webrtc_compat_local_sdp(TEST_COMPAT_INPUT_SDP);
    assert(compat != NULL);
    printf("--- COMPAT SDP OUTPUT ---\n%s\n-------------------------\n", compat);

    // Verify bundle group added
    assert(strstr(compat, "a=group:BUNDLE video audio") != NULL);

    // Verify rtcp-mux added in both sections
    // Should contain at least two instances of a=rtcp-mux
    char* first_rtcp_mux = strstr(compat, "a=rtcp-mux");
    assert(first_rtcp_mux != NULL);
    char* second_rtcp_mux = strstr(first_rtcp_mux + 10, "a=rtcp-mux");
    assert(second_rtcp_mux != NULL);

    // Verify msid added with correct track names
    assert(strstr(compat, "a=msid:zstreamer-stream zstreamer-track-video") != NULL);
    assert(strstr(compat, "a=msid:zstreamer-stream zstreamer-track-audio") != NULL);

    // Verify rewritten cname
    assert(strstr(compat, "a=ssrc:12345 cname:zstreamer-cname") != NULL);

    // Verify fallback ssrc generated with consistent cname
    assert(strstr(compat, "a=ssrc:1002 cname:zstreamer-cname") != NULL);

    free(compat);

    printf("SDP clean/filter and compat tests PASSED.\n");
    return 0;
}
