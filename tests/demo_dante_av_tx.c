/*=============================================================================
    demo_dante_av_tx.c -- Dante H.264 video plus DEP PCM32 audio transmitter

    Video: videotestsrc -> x264enc -> dantevideocoordinator -> DVR-managed RTP
    Audio: audiotestsrc(S32LE) -> dantedepaudiosink -> Dante DEP shared memory

    The DVR must create a TX video flow after this endpoint connects.  The DEP
    shared-memory endpoint must exist before starting this demo.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"
#include "zstreamer/elements/zst_dante_session.h"
#include "zstreamer/elements/zst_dante_video_coordinator.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signal)
{
    (void)signal;
    g_stop = 1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static void sleep_until(uint64_t deadline)
{
    while (!g_stop) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) return;
        uint64_t remaining = deadline - now;
        struct timespec delay = {
            .tv_sec = (time_t)(remaining / UINT64_C(1000000000)),
            .tv_nsec = (long)(remaining % UINT64_C(1000000000))
        };
        nanosleep(&delay, NULL);
    }
}

static int set_string(zst_element_t* element, const char* name, const char* value)
{
    if (zst_element_set_property_string(element, name, value) == ZST_OK) return 1;
    fprintf(stderr, "failed to set %s=%s on %s\n", name, value, element->ops->name);
    return 0;
}

static void handle_dvr_events(zst_bus_t* bus, zst_element_t* coordinator)
{
    for (;;) {
        zst_event_t* event = NULL;
        if (zst_bus_pop(bus, &event, 0) != ZST_OK) return;
        if (event->type == ZST_EVENT_DANTE_FLOW_CREATED) {
            const zst_dante_flow_t* flow = &event->as.dante_flow.flow;
            if (flow->direction == ZST_DANTE_FLOW_TX && flow->channel_index == 0) {
                if (zst_dante_video_coordinator_apply_flow(coordinator, flow) == ZST_OK)
                    printf("DVR enabled video TX flow %u\n", flow->flow_index);
                else
                    fprintf(stderr, "failed to apply video TX flow %u\n", flow->flow_index);
            }
        } else if (event->type == ZST_EVENT_DANTE_FLOW_DELETED) {
            const zst_dante_flow_t* flow = &event->as.dante_flow.flow;
            if (flow->direction == ZST_DANTE_FLOW_TX)
                (void)zst_dante_video_coordinator_remove_flow(coordinator, flow);
        } else if (event->type == ZST_EVENT_WARNING) {
            fprintf(stderr, "Dante warning: %s\n", event->as.error.message);
        }
        zst_event_destroy(event);
    }
}

int main(int argc, char** argv)
{
    const char* socket_path = argc > 1 ? argv[1] : ZST_DANTE_SESSION_DEFAULT_SOCKET_PATH;
    const char* shm_name = argc > 2 ? argv[2] : "DanteEP";
    unsigned long seconds = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
    zst_pipeline_t* pipeline = zst_pipeline_create();
    zst_element_t* session = zst_dante_session_create(socket_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();
    zst_element_t* video_source = zst_video_test_src_create();
    zst_element_t* encoder = zst_x264_encoder_create();
    zst_element_t* audio_source = zst_audio_test_src_create();
    zst_element_t* dep_sink = zst_dante_dep_audio_sink_create();
    zst_clock_t* clock = zst_clock_system_create();
    zst_pad_t* tx_pad = NULL;
    int result = 1;

    if (!pipeline || !session || !coordinator || !video_source || !encoder ||
        !audio_source || !dep_sink || !clock) {
        fprintf(stderr, "failed to create Dante A/V transmitter elements\n");
        goto done;
    }

    if (!set_string(session, "tx-video-channels", "1") ||
        !set_string(session, "rx-video-channels", "0") ||
        !set_string(video_source, "width", "1280") ||
        !set_string(video_source, "height", "720") ||
        !set_string(video_source, "fps", "30") ||
        !set_string(video_source, "use-clock", "true") ||
        !set_string(encoder, "preset", "ultrafast") ||
        !set_string(encoder, "tune", "zerolatency") ||
        !set_string(encoder, "fps", "30/1") ||
        !set_string(audio_source, "sample-rate", "48000") ||
        !set_string(audio_source, "channels", "2") ||
        !set_string(audio_source, "sample-format", "S32LE") ||
        !set_string(audio_source, "samples-per-buffer", "1024") ||
        !set_string(audio_source, "use-clock", "true") ||
        !set_string(dep_sink, "shm-name", shm_name) ||
        !set_string(dep_sink, "channels", "0,1") ||
        !set_string(dep_sink, "expected-sample-rate", "48000"))
        goto done;

    zst_element_set_clock(video_source, clock);
    zst_element_set_clock(audio_source, clock);
    if (zst_pipeline_add(pipeline, coordinator) != ZST_OK ||
        zst_pipeline_add(pipeline, video_source) != ZST_OK ||
        zst_pipeline_add(pipeline, encoder) != ZST_OK ||
        zst_pipeline_add(pipeline, audio_source) != ZST_OK ||
        zst_pipeline_add(pipeline, dep_sink) != ZST_OK) {
        fprintf(stderr, "failed to add elements to pipeline\n");
        goto done;
    }
    if (zst_dante_video_coordinator_attach_session(coordinator, session) != ZST_OK ||
        !(tx_pad = zst_dante_video_coordinator_request_tx_input_pad(coordinator, 0)) ||
        zst_pad_link(zst_element_get_pad(encoder, "src"), tx_pad) != ZST_OK) {
        fprintf(stderr, "failed to link Dante A/V transmitter pipeline\n");
        goto done;
    }
    if (zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "failed to start pipeline; verify the DEP endpoint exists\n");
        goto done;
    }
    session->bus = pipeline->bus;
    if (zst_element_set_state(session, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "failed to start Dante DVR control session\n");
        goto done;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("Dante A/V TX: DVR=%s DEP=%s, Ctrl-C to stop%s\n", socket_path, shm_name,
           seconds ? " (or automatic timeout)" : "");

    uint64_t start = monotonic_ns();
    uint64_t end = seconds ? start + seconds * UINT64_C(1000000000) : UINT64_MAX;
    uint64_t next_video = start;
    uint64_t next_audio = start;
    const uint64_t video_interval = UINT64_C(1000000000) / 30;
    const uint64_t audio_interval = UINT64_C(1024) * UINT64_C(1000000000) / 48000;
    while (!g_stop && monotonic_ns() < end) {
        uint64_t now = monotonic_ns();
        handle_dvr_events(pipeline->bus, coordinator);
        if (now >= next_video) {
            zst_buffer_t* raw = NULL;
            zst_buffer_t* encoded = NULL;
            if (video_source->ops->process(video_source, NULL, &raw) == ZST_OK && raw) {
                if (encoder->ops->process(encoder, raw, &encoded) == ZST_OK && encoded) {
                    (void)zst_pad_push(zst_element_get_pad(encoder, "src"), encoded);
                    zst_buffer_unref(encoded);
                }
                zst_buffer_unref(raw);
            }
            next_video += video_interval;
        }
        if (now >= next_audio) {
            zst_buffer_t* audio = NULL;
            if (audio_source->ops->process(audio_source, NULL, &audio) == ZST_OK && audio) {
                (void)dep_sink->ops->process(dep_sink, audio, NULL);
                zst_buffer_unref(audio);
            }
            next_audio += audio_interval;
        }
        sleep_until(next_video < next_audio ? next_video : next_audio);
    }
    result = 0;

done:
    if (session) (void)zst_element_set_state(session, ZST_STATE_NULL);
    if (video_source) (void)zst_element_set_state(video_source, ZST_STATE_NULL);
    if (encoder) (void)zst_element_set_state(encoder, ZST_STATE_NULL);
    if (audio_source) (void)zst_element_set_state(audio_source, ZST_STATE_NULL);
    if (dep_sink) (void)zst_element_set_state(dep_sink, ZST_STATE_NULL);
    if (coordinator) (void)zst_element_set_state(coordinator, ZST_STATE_NULL);
    if (pipeline) (void)zst_pipeline_set_state(pipeline, ZST_STATE_NULL);
    if (coordinator && session) (void)zst_dante_video_coordinator_attach_session(coordinator, NULL);
    if (session) session->bus = NULL;
    if (pipeline) zst_pipeline_destroy(pipeline);
    else {
        zst_element_destroy(session);
        zst_element_destroy(coordinator);
        zst_element_destroy(video_source);
        zst_element_destroy(encoder);
        zst_element_destroy(audio_source);
        zst_element_destroy(dep_sink);
    }
    if (pipeline) zst_element_destroy(session);
    if (clock) zst_clock_unref(clock);
    return result;
}
