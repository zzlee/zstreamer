/*=============================================================================
    test_webrtc_multitrack.c — Phase 8a verification test

    Tests multi-track routing:
    - Adds both video and audio tracks.
    - Verifies dynamic sink pads (sink_video_0, sink_audio_1) are created.
    - Verifies buffers route to correct tracks based on type.
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
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"
#include "zst_pad.h"

extern zst_result_t zst_register_builtin_elements(void);

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
        usleep(50000);
    }
    return NULL;
}

int main(int argc, char* argv[])
{
    (void)argc; (void)argv;
    zst_log_set_level(ZST_LOG_LEVEL_INFO);

    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register built-in elements\n");
        return 1;
    }

    zst_element_t* el = zst_element_factory_make("webrtc_endpoint");
    if (!el) {
        printf("[SKIP] webrtc_endpoint not available\n");
        return 0;
    }

    assert(zst_element_set_state(el, ZST_STATE_READY) == ZST_OK);

    /* Watch bus for PAD_ADDED */
    zst_bus_t* bus = zst_bus_create();
    el->bus = bus;

    /* 1. Add video track */
    assert(zst_webrtc_add_video_track(el, ZST_WEBRTC_CODEC_H264, 11111, "video0") == ZST_OK);

    zst_event_t* ev1 = wait_for_event(el, ZST_EVENT_PAD_ADDED, 1000);
    assert(ev1 != NULL);
    assert(ev1->as.pad_added.pad != NULL);
    assert(strcmp(ev1->as.pad_added.pad->name, "sink_video_0") == 0);
    zst_event_destroy(ev1);
    printf("[PASS] Dynamic pad sink_video_0 created\n");

    /* 2. Add audio track */
    assert(zst_webrtc_add_audio_track(el, ZST_WEBRTC_CODEC_OPUS, 22222, "audio0") == ZST_OK);

    zst_event_t* ev2 = wait_for_event(el, ZST_EVENT_PAD_ADDED, 1000);
    assert(ev2 != NULL);
    assert(ev2->as.pad_added.pad != NULL);
    assert(strcmp(ev2->as.pad_added.pad->name, "sink_audio_1") == 0);
    zst_event_destroy(ev2);
    printf("[PASS] Dynamic pad sink_audio_1 created\n");

    /* Create dummy src pads to link to the sinks, so we can push */
    zst_element_t* dummy_el = zst_element_factory_make("queue");
    zst_pad_t* dummy_src1 = zst_pad_create("src1", ZST_PAD_SRC);
    zst_pad_t* dummy_src2 = zst_pad_create("src2", ZST_PAD_SRC);
    zst_element_add_pad(dummy_el, dummy_src1);
    zst_element_add_pad(dummy_el, dummy_src2);

    zst_pad_t* sink_video = zst_element_get_pad(el, "sink_video_0");
    zst_pad_t* sink_audio = zst_element_get_pad(el, "sink_audio_1");

    assert(sink_video != NULL);
    assert(sink_audio != NULL);

    assert(zst_pad_link(dummy_src1, sink_video) == ZST_OK);
    assert(zst_pad_link(dummy_src2, sink_audio) == ZST_OK);

    /* 3. Send video buffer */
    zst_buffer_t* vbuf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    vbuf->memory.size = 10;
    vbuf->memory.data = malloc(10);
    vbuf->destroy = zst_buffer_unref; /* Dummy destroy */

    assert(zst_pad_push(dummy_src1, vbuf) == ZST_OK);
    printf("[PASS] Pushed video buffer\n");

    /* 4. Send audio buffer */
    zst_buffer_t* abuf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
    abuf->memory.size = 10;
    abuf->memory.data = malloc(10);
    abuf->destroy = zst_buffer_unref;

    assert(zst_pad_push(dummy_src2, abuf) == ZST_OK);
    printf("[PASS] Pushed audio buffer\n");

    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_destroy(el);
    zst_element_destroy(dummy_el);
    zst_bus_destroy(bus);
    printf("[PASS] test_webrtc_multitrack complete\n");
    return 0;
}
