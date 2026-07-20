/*=============================================================================
    test_webrtc_restart.c — ICE Restart Support unit test
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "zst_element.h"
#include "zst_bus.h"
#include "zst_pipeline.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

extern zst_result_t zst_register_builtin_elements(void);

static int
wait_for_ice_state(zst_element_t* el_a, zst_element_t* el_b, const char* expected, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        char ice_a[32] = {0};
        char ice_b[32] = {0};
        zst_element_get_property(el_a, "ice-state", ice_a, sizeof(ice_a));
        zst_element_get_property(el_b, "ice-state", ice_b, sizeof(ice_b));
        
        if ((strcmp(ice_a, expected) == 0 || strcmp(ice_a, "completed") == 0) &&
            (strcmp(ice_b, expected) == 0 || strcmp(ice_b, "completed") == 0)) {
            return 1;
        }
        usleep(100000); /* 100ms */
        elapsed += 100;
    }
    return 0;
}

int main(void)
{
    zst_log_set_level(ZST_LOG_LEVEL_INFO);
    printf("=== WebRTC ICE Restart Unit Test ===\n");

    assert(zst_register_builtin_elements() == ZST_OK);

    zst_element_t* el_a = zst_element_factory_make("webrtc_endpoint");
    assert(el_a != NULL);
    zst_element_t* el_b = zst_element_factory_make("webrtc_endpoint");
    assert(el_b != NULL);

    zst_element_set_property(el_a, "stun-servers", "stun:stun.l.google.com:19302");
    zst_element_set_property(el_b, "stun-servers", "stun:stun.l.google.com:19302");

    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    assert(zst_pipeline_add(pipe, el_a) == ZST_OK);
    assert(zst_pipeline_add(pipe, el_b) == ZST_OK);

    assert(zst_element_set_state(el_a, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(el_b, ZST_STATE_READY) == ZST_OK);

    // Add video track to A so there is a media stream
    assert(zst_webrtc_add_video_track(el_a, ZST_WEBRTC_CODEC_H264, 12345, "video") == ZST_OK);
    // Create data channels on both
    assert(zst_webrtc_create_data_channel(el_a, "test-channel") == ZST_OK);
    assert(zst_webrtc_create_data_channel(el_b, "test-channel") == ZST_OK);

    // Initial offer negotiation
    assert(zst_webrtc_create_offer(el_a) == ZST_OK);
    usleep(200000); // 200ms

    char offer_buf[8192] = {0};
    assert(zst_element_get_property(el_a, "local-sdp", offer_buf, sizeof(offer_buf)) == ZST_OK);
    assert(strlen(offer_buf) > 0);

    assert(zst_webrtc_set_remote_description(el_b, "offer", offer_buf) == ZST_OK);
    usleep(200000); // 200ms

    char answer_buf[8192] = {0};
    assert(zst_element_get_property(el_b, "local-sdp", answer_buf, sizeof(answer_buf)) == ZST_OK);
    assert(strlen(answer_buf) > 0);

    assert(zst_webrtc_set_remote_description(el_a, "answer", answer_buf) == ZST_OK);

    printf("Waiting for initial ICE connection...\n");
    int connected = wait_for_ice_state(el_a, el_b, "connected", 5000);
    assert(connected);
    printf("[PASS] Initial connection established.\n");

    // Retrieve initial credentials
    char init_ufrag_a[256] = {0};
    assert(zst_element_get_property(el_a, "local-sdp", offer_buf, sizeof(offer_buf)) == ZST_OK);
    char* ufrag_ptr = strstr(offer_buf, "a=ice-ufrag:");
    if (ufrag_ptr) {
        sscanf(ufrag_ptr + 12, "%255s", init_ufrag_a);
    }
    printf("Initial ice-ufrag: %s\n", init_ufrag_a);

    // TRIGGER ICE RESTART
    printf("Triggering ICE restart on endpoint A...\n");
    assert(zst_webrtc_restart_ice(el_a) == ZST_OK);
    printf("Triggering ICE restart on endpoint B...\n");
    assert(zst_webrtc_restart_ice(el_b) == ZST_OK);
    usleep(200000); // 200ms

    // Retrieve new offer and verify ufrag changed
    char new_offer_buf[8192] = {0};
    assert(zst_element_get_property(el_a, "local-sdp", new_offer_buf, sizeof(new_offer_buf)) == ZST_OK);
    char new_ufrag_a[256] = {0};
    ufrag_ptr = strstr(new_offer_buf, "a=ice-ufrag:");
    if (ufrag_ptr) {
        sscanf(ufrag_ptr + 12, "%255s", new_ufrag_a);
    }
    printf("New ice-ufrag: %s\n", new_ufrag_a);
    assert(strcmp(init_ufrag_a, new_ufrag_a) != 0);
    printf("[PASS] ICE restart successfully generated new ICE credentials.\n");

    // Negotiate new offer
    assert(zst_webrtc_set_remote_description(el_b, "offer", new_offer_buf) == ZST_OK);
    usleep(200000); // 200ms

    char new_answer_buf[8192] = {0};
    assert(zst_element_get_property(el_b, "local-sdp", new_answer_buf, sizeof(new_answer_buf)) == ZST_OK);
    assert(strlen(new_answer_buf) > 0);

    assert(zst_webrtc_set_remote_description(el_a, "answer", new_answer_buf) == ZST_OK);

    printf("Waiting for re-negotiated ICE connection...\n");
    connected = wait_for_ice_state(el_a, el_b, "connected", 5000);
    assert(connected);
    printf("[PASS] Connection re-established after ICE restart.\n");

    // Cleanup
    zst_element_set_state(el_a, ZST_STATE_NULL);
    zst_element_set_state(el_b, ZST_STATE_NULL);

    printf("ICE Restart tests passed successfully!\n");
    return 0;
}
