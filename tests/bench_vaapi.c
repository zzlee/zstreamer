/*=============================================================================
    bench_vaapi.c — VA-API H.264/HEVC Encode/Decode 1080p Performance Benchmark
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_plugin.h"

static int g_frame_count = 0;

static zst_pad_probe_return_t
on_encoded_frame(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad;
    (void)type;
    (void)user_data;

    if (buf && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        g_frame_count++;
        if (g_frame_count % 100 == 0) {
            printf("[BENCHMARK] Encoded %d frames...\n", g_frame_count);
        }
    }
    return ZST_PAD_PROBE_OK;
}

static const char* test_plugin_path(void)
{
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
    }
    return ppath;
}

int main(int argc, char* argv[])
{
    int width = 1920;
    int height = 1080;
    int max_frames = 500;
    const char* codec = "h264";
    const char* mode = "roundtrip";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            codec = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--frames N] [--codec h264|hevc] [--mode generate|encode-only|decode-only|roundtrip]\n", argv[0]);
            return EXIT_SUCCESS;
        }
    }

    if (zst_register_builtin_elements() != ZST_OK ||
        zst_plugin_registry_init() != ZST_OK ||
        zst_plugin_registry_scan(test_plugin_path()) != ZST_OK) {
        fprintf(stderr, "Failed to initialize plugins\n");
        return EXIT_FAILURE;
    }

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_pipeline_set_clock_sync(pipe, 0); // Disable clock sync to measure max throughput
    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&sched_cfg);

    zst_element_t* src = NULL;
    zst_element_t* enc = NULL;
    zst_element_t* dec = NULL;
    zst_element_t* sink = NULL;

    assert(pipe != NULL);
    assert(sched != NULL);

    int is_encode = (strcmp(mode, "encode-only") == 0 || strcmp(mode, "roundtrip") == 0 || strcmp(mode, "generate") == 0);
    int is_decode = (strcmp(mode, "decode-only") == 0 || strcmp(mode, "roundtrip") == 0);

    if (strcmp(mode, "decode-only") == 0) {
        src = zst_element_factory_make("mp4demux");
        assert(src != NULL);
        char filename[64];
        snprintf(filename, sizeof(filename), "test.%s.mp4", codec);
        assert(zst_element_set_property(src, "location", filename) == ZST_OK);
        assert(zst_element_set_property(src, "real-time-pacing", "false") == ZST_OK);
        sink = zst_element_factory_make("fakesink");
    } else {
        src = zst_element_factory_make("videotestsrc");
        assert(src != NULL);
        assert(zst_element_set_property_uint(src, "width", (uint64_t)width) == ZST_OK);
        assert(zst_element_set_property_uint(src, "height", (uint64_t)height) == ZST_OK);
        assert(zst_element_set_property(src, "pixel-format", "I420") == ZST_OK);
        assert(zst_element_set_property(src, "real-time-pacing", "false") == ZST_OK);
        assert(zst_element_set_property_int(src, "num-buffers", (int64_t)max_frames) == ZST_OK);
    }

    if (strcmp(mode, "encode-only") == 0 || strcmp(mode, "generate") == 0) {
        if (strcmp(codec, "hevc") == 0) {
            sink = zst_element_factory_make("filesink");
            assert(sink != NULL);
            char filename[64];
            snprintf(filename, sizeof(filename), "test.%s", codec);
            assert(zst_element_set_property(sink, "path", filename) == ZST_OK);
        } else {
            sink = zst_element_factory_make("mp4mux");
            assert(sink != NULL);
            char filename[64];
            snprintf(filename, sizeof(filename), "test.%s.mp4", codec);
            assert(zst_element_set_property(sink, "location", filename) == ZST_OK);
        }
    } else if (strcmp(mode, "roundtrip") == 0) {
        sink = zst_element_factory_make("fakesink");
    }
    assert(sink != NULL);

    if (is_encode) {
        if (strcmp(mode, "generate") == 0) {
            enc = zst_element_factory_make(strcmp(codec, "h264") == 0 ? "x264enc" : "x265enc");
            assert(enc != NULL);
        } else {
            enc = zst_element_factory_make("vaapienc");
            assert(enc != NULL);
            assert(zst_element_set_property(enc, "codec", codec) == ZST_OK);
            assert(zst_element_set_property(enc, "preset", "speed") == ZST_OK);
            assert(zst_element_set_property(enc, "rate-control", "CQP") == ZST_OK);
        }
    }

    if (is_decode) {
        dec = zst_element_factory_make("vaapidec");
        assert(dec != NULL);
        assert(zst_element_set_property(dec, "codec", codec) == ZST_OK);
    }

    zst_pad_t* sink_pad = NULL;
    if (strcmp(mode, "encode-only") == 0 || strcmp(mode, "generate") == 0) {
        if (strcmp(codec, "hevc") == 0) {
            sink_pad = zst_element_get_pad(sink, "sink");
        } else {
            sink_pad = zst_element_get_pad(sink, "video");
        }
    } else {
        sink_pad = zst_element_get_pad(sink, "sink");
    }
    assert(sink_pad != NULL);
    zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_POST_BUFFER, on_encoded_frame, NULL);

    zst_pipeline_add(pipe, src);
    if (enc) zst_pipeline_add(pipe, enc);
    if (dec) zst_pipeline_add(pipe, dec);
    zst_pipeline_add(pipe, sink);

    if (is_encode && is_decode) {
        assert(zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(enc, "sink")) == ZST_OK);
        assert(zst_pad_link(zst_element_get_pad(enc, "src"), zst_element_get_pad(dec, "sink")) == ZST_OK);
        assert(zst_pad_link(zst_element_get_pad(dec, "src"), sink_pad) == ZST_OK);
    } else if (is_encode) {
        assert(zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(enc, "sink")) == ZST_OK);
        assert(zst_pad_link(zst_element_get_pad(enc, "src"), sink_pad) == ZST_OK);
    } else if (is_decode) {
        // mp4demux pads are dynamic, we will link them after state PLAYING
        assert(zst_pad_link(zst_element_get_pad(dec, "src"), sink_pad) == ZST_OK);
    }

    if (zst_pipeline_set_state(pipe, ZST_STATE_READY) != ZST_OK) {
        fprintf(stderr, "VA-API hardware device unavailable, benchmark skipped\n");
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
        return EXIT_SUCCESS;
    }

    printf("[BENCHMARK] Starting 1080p VA-API %s %s benchmark (%d frames max)...\n", codec, mode, max_frames);
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    if (zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "Failed to start pipeline\n");
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
        return EXIT_FAILURE;
    }

    if (strcmp(mode, "decode-only") == 0) {
        // Now that mp4demux is playing, it has opened the file and created video_0
        zst_pad_t* demux_video = zst_element_get_pad(src, "video_0");
        assert(demux_video != NULL);
        assert(zst_pad_link(demux_video, zst_element_get_pad(dec, "sink")) == ZST_OK);
        
        // Let's also recount since fakesink will only get frames now
    }

    zst_scheduler_attach(sched, pipe);
    zst_scheduler_run(sched);

    // Wait loop for asynchronous pipeline execution
    int timeout_sec = 60;
    struct timespec loop_start;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);
    while (g_frame_count < max_frames) {
        usleep(10000);
        struct timespec loop_now;
        clock_gettime(CLOCK_MONOTONIC, &loop_now);
        double diff = (loop_now.tv_sec - loop_start.tv_sec) + (loop_now.tv_nsec - loop_start.tv_nsec) / 1e9;
        if (diff >= timeout_sec) {
            fprintf(stderr, "[BENCHMARK] Timeout waiting for %d frames\n", max_frames);
            break;
        }
        if (strcmp(mode, "encode-only") == 0 || strcmp(mode, "generate") == 0) {
            // Encode-only doesn't use fakesink probe, so we just wait until pipeline finishes or EOS
            // For videotestsrc, it pushes max_frames then sends EOS
            if (src->state == ZST_STATE_NULL) break;
        }
    }

    if (strcmp(mode, "encode-only") == 0 || strcmp(mode, "generate") == 0) {
        usleep(500000); // Give it a tiny bit to finish writing MP4 atom or file data for encode-only

        if (strcmp(codec, "hevc") == 0) {
            printf("[BENCHMARK] Muxing raw HEVC to MP4...\n");
            system("ffmpeg -y -i test.hevc -c copy test.hevc.mp4 >/dev/null 2>&1");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    if (g_frame_count > 0) {
        double fps = g_frame_count / elapsed;
        printf("[BENCHMARK] Done! Processed %d frames of 1080p in %.3f seconds (%.2f FPS).\n",
               g_frame_count, elapsed, fps);
    } else {
        printf("[BENCHMARK] No frames were processed during the test interval.\n");
    }

    return EXIT_SUCCESS;
}
