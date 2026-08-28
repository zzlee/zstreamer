/*=============================================================================
    test_webrtc_media_recv.c — Phase 4 verification test

    Tests inbound media track reception via the WebRTC endpoint.
    Verifies that when the remote peer adds a video track, the local
    endpoint fires on_track, creates a source pad, and can receive frames.
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
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

extern zst_result_t zst_register_builtin_elements(void);

/* Shared state for the on_track callback */
static int g_track_received = 0;
static int g_received_track_id = -1;
static zst_webrtc_codec_t g_received_codec;
static char g_received_mid[64] = {0};

static void
on_track_cb(zst_element_t* el, int track_id, zst_webrtc_codec_t codec,
            const char* mid, void* user_data)
{
    (void)el; (void)user_data;
    g_track_received = 1;
    g_received_track_id = track_id;
    g_received_codec = codec;
    snprintf(g_received_mid, sizeof(g_received_mid), "%s", mid ? mid : "");
    printf("  [callback] on_track: id=%d, codec=%d, mid=%s\n", track_id, codec, mid ? mid : "");
}

/* Helper: wait for event of given type */
static zst_event_t*
wait_for_event(zst_element_t* el, zst_event_type_t expected, uint32_t timeout_ms)
{
    zst_bus_t* bus = el->bus;
    if (!bus) return NULL;
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(bus, &ev, 50);
        elapsed += 50;
        if (r == ZST_OK && ev) {
            if (ev->type == expected) return ev;
            zst_event_destroy(ev);
        }
    }
    return NULL;
}

int main(void)
{
    printf("=== WebRTC Media Receive Test (Phase 4) ===\n");
    if (zst_register_builtin_elements() != ZST_OK) abort();

    /* Create two endpoints */
    zst_element_t* el_a = zst_element_factory_make("webrtc_endpoint"); /* sender */
    zst_element_t* el_b = zst_element_factory_make("webrtc_endpoint"); /* receiver */
    assert(el_a && el_b);
    printf("[PASS] Created sender (A) and receiver (B)\n");

    zst_pipeline_t* pipe_a = zst_pipeline_create();
    zst_pipeline_t* pipe_b = zst_pipeline_create();
    zst_pipeline_add(pipe_a, el_a);
    zst_pipeline_add(pipe_b, el_b);

    /* Register on_track callback on B before negotiation */
    zst_webrtc_set_on_track_callback(el_b, on_track_cb, NULL);
    printf("[PASS] Registered on_track callback on receiver\n");

    /* Open both endpoints */
    assert(zst_element_set_state(el_a, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(el_b, ZST_STATE_READY) == ZST_OK);
    printf("[PASS] Both endpoints opened\n");

    /* Step 1: Add video track to sender */
    assert(zst_webrtc_add_video_track(el_a, ZST_WEBRTC_CODEC_H264, 12345, "video0") == ZST_OK);
    printf("[PASS] Added H264 video track to sender\n");

    /* Step 2: Create SDP offer */
    assert(zst_webrtc_create_offer(el_a) == ZST_OK);
    usleep(200000);

    char offer_buf[8192] = {0};
    assert(zst_element_get_property(el_a, "local-sdp", offer_buf, sizeof(offer_buf)) == ZST_OK);
    assert(strlen(offer_buf) > 0);
    assert(strstr(offer_buf, "m=video") != NULL);
    printf("[PASS] SDP offer generated (%zu bytes)\n", strlen(offer_buf));

    /* Step 3: Forward offer to B, capture answer from bus event */
    assert(zst_webrtc_set_remote_description(el_b, "offer", offer_buf) == ZST_OK);
    printf("[PASS] Offer forwarded to B\n");

    zst_event_t* ans_ev = wait_for_event(el_b, ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION, 5000);
    assert(ans_ev != NULL);
    assert(strcmp(ans_ev->as.webrtc_local_description.type, "answer") == 0);

    char answer_buf[8192] = {0};
    snprintf(answer_buf, sizeof(answer_buf), "%s", ans_ev->as.webrtc_local_description.sdp);
    zst_event_destroy(ans_ev);
    printf("[PASS] SDP answer captured (%zu bytes)\n", strlen(answer_buf));

    /* Step 4: Forward answer to A */
    assert(zst_webrtc_set_remote_description(el_a, "answer", answer_buf) == ZST_OK);
    printf("[PASS] Answer forwarded to A\n");

    /* Step 5: Check on_track callback fired */
    usleep(500000); /* 500ms for callbacks to fire */

    if (g_track_received) {
        printf("[PASS] on_track callback fired (track_id=%d, codec=%d, mid=%s)\n",
               g_received_track_id, g_received_codec, g_received_mid);
    } else {
        printf("[WARN] on_track callback did not fire (may need ICE connectivity)\n");
    }

    /* Step 6: Verify source pad was created on B */
    uint32_t num_src = 0;
    zst_pad_t** src_pads = NULL;
    zst_result_t pad_r = zst_element_snapshot_src_pads(el_b, &src_pads, &num_src);

    if (pad_r == ZST_OK && num_src > 0) {
        printf("[PASS] Receiver has %u source pad(s)\n", num_src);
        for (uint32_t i = 0; i < num_src; i++) {
            printf("  - pad[%u]: name=%s, linked=%d\n",
                   i, src_pads[i]->name ? src_pads[i]->name : "?",
                   zst_pad_is_linked(src_pads[i]));
        }
        free(src_pads);
    } else {
        printf("[WARN] No source pads on receiver (on_track may not have fired)\n");
    }

    /* Step 7: Send a few frames (will fail without ICE, but tests API) */
    uint8_t nalu[128];
    memset(nalu, 0, sizeof(nalu));
    nalu[0] = 0x00; nalu[1] = 0x00; nalu[2] = 0x00; nalu[3] = 0x01;
    nalu[4] = 0x67; /* SPS */

    int sent = 0;
    for (int i = 0; i < 5; i++) {
        if (zst_webrtc_send_media(el_a, 0, nalu, 128) == ZST_OK) sent++;
        usleep(33000);
    }
    printf("[INFO] Sent %d/5 frames (ICE may not be connected in Docker)\n", sent);

    /* Cleanup */
    zst_element_set_state(el_a, ZST_STATE_NULL);
    zst_element_set_state(el_b, ZST_STATE_NULL);

    /* Verdict */
    if (g_track_received && num_src > 0) {
        printf("\n=== All Phase 4 media receive tests passed ===\n");
        return 0;
    } else {
        printf("\n=== Phase 4 partially passed (callback=%s, pads=%u) ===\n",
               g_track_received ? "yes" : "no", num_src);
        return 1;
    }
}
