/*=============================================================================
    demo_rtsp_mod.c — RTSP Media-On-Demand Demo Application

    This demo runs an RTSP server with a dynamic mount callback.
    When a client connects and requests a stream:
      - /colorbar: dynamically creates and starts a synthetic H.264/AAC source
      - /bunny: dynamically starts an HTTPS source pulling Big Buck Bunny
                 and demuxing H.264/AAC packets directly to the client.
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_rtsp_server.h"
#include "zstreamer/elements/zst_mp4_demuxer.h"

typedef struct {
    zst_element_t* demux;
    zst_element_t* vdec;
    zst_element_t* venc;
    zst_element_t* vq;
    zst_element_t* adec;
    zst_element_t* aenc;
    zst_element_t* aq;
    int active;
    time_t last_active_time;
} bunny_pipeline_t;

static bunny_pipeline_t g_bunny = {0};

static void cleanup_bunny_pipeline(zst_pipeline_t* pipe, zst_element_t* server) {
    if (!g_bunny.active) return;
    printf("[Demo App] Cleaning up bunny pipeline...\n");
    fflush(stdout);

    // 1. Stop the elements first to abort any pending processing / threads
    zst_element_set_state(g_bunny.demux, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.vdec, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.venc, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.vq, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.adec, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.aenc, ZST_STATE_NULL);
    zst_element_set_state(g_bunny.aq, ZST_STATE_NULL);

    // 2. Unlink all pads (demux pads are dynamic: video_0, audio_0)
    zst_pad_unlink(zst_element_get_pad(g_bunny.demux, "video_0"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.vdec, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.vdec, "src"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.venc, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.venc, "src"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.vq, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.vq, "src"));

    zst_pad_unlink(zst_element_get_pad(g_bunny.demux, "audio_0"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.adec, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.adec, "src"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.aenc, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.aenc, "src"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.aq, "sink"));
    zst_pad_unlink(zst_element_get_pad(g_bunny.aq, "src"));

    // 3. Remove from pipeline
    zst_pipeline_remove(pipe, g_bunny.demux);
    zst_pipeline_remove(pipe, g_bunny.vdec);
    zst_pipeline_remove(pipe, g_bunny.venc);
    zst_pipeline_remove(pipe, g_bunny.vq);
    zst_pipeline_remove(pipe, g_bunny.adec);
    zst_pipeline_remove(pipe, g_bunny.aenc);
    zst_pipeline_remove(pipe, g_bunny.aq);

    // 4. Destroy elements
    zst_element_destroy(g_bunny.demux);
    zst_element_destroy(g_bunny.vdec);
    zst_element_destroy(g_bunny.venc);
    zst_element_destroy(g_bunny.vq);
    zst_element_destroy(g_bunny.adec);
    zst_element_destroy(g_bunny.aenc);
    zst_element_destroy(g_bunny.aq);

    // 5. Remove session from server
    zst_rtsp_server_remove_session(server, "bunny");

    // 6. Recalculate topological sort in pipeline
    zst_pipeline_topological_sort(pipe);

    // Reset structure
    memset(&g_bunny, 0, sizeof(g_bunny));
    printf("[Demo App] Bunny pipeline cleaned up successfully.\n");
    fflush(stdout);
}

static volatile int g_running = 1;
static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static zst_result_t on_demand_mount(zst_element_t* server, const char* name, void* user_data) {
    zst_pipeline_t* pipe = (zst_pipeline_t*)user_data;
    printf("[Demo App] Dynamic mount request received for session: /%s\n", name);

    if (strcmp(name, "colorbar") == 0) {
        printf("[Demo App] Creating colorbar source pipeline...\n");

        // 1. Add session to server
        if (zst_rtsp_server_add_session(server, name) != ZST_OK) {
            fprintf(stderr, "[Demo App] Failed to add RTSP session for /%s\n", name);
            return ZST_ERROR;
        }

        // 2. Create elements
        zst_element_t* video_src = zst_element_factory_make("videotestsrc");
        zst_element_t* overlay   = zst_element_factory_make("textoverlay");
        zst_element_t* h264      = zst_element_factory_make("x264enc");
        zst_element_t* audio_src = zst_element_factory_make("audiotestsrc");
        zst_element_t* aac       = zst_element_factory_make("aacenc");

        if (!video_src || !overlay || !h264 || !audio_src || !aac) {
            fprintf(stderr, "[Demo App] Failed to create colorbar pipeline elements\n");
            return ZST_ERROR;
        }

        // Configure elements
        zst_element_set_property_int(video_src, "width", 320);
        zst_element_set_property_int(video_src, "height", 240);
        zst_element_set_property_int(video_src, "fps", 30);
        zst_element_set_property_string(video_src, "pattern", "bars");
        zst_element_set_property_bool(video_src, "use-clock", true);
        zst_element_set_property_bool(video_src, "real-time-pacing", true);

        zst_element_set_property_int(h264, "gop-size", 30);

        zst_element_set_property_bool(overlay, "timecode", true);
        zst_element_set_property_int(overlay, "font-size", 24);
        zst_element_set_property_int(overlay, "x", 10);
        zst_element_set_property_int(overlay, "y", 35);

        zst_element_set_property_int(audio_src, "sample-rate", 44100);
        zst_element_set_property_int(audio_src, "channels", 2);
        zst_element_set_property_string(audio_src, "sample-format", "S16LE");
        zst_element_set_property_string(audio_src, "wave", "sine");
        zst_element_set_property_int(audio_src, "frequency", 1000);
        zst_element_set_property_int(audio_src, "samples-per-buffer", 1024);
        zst_element_set_property_bool(audio_src, "use-clock", true);
        zst_element_set_property_bool(audio_src, "real-time-pacing", true);

        // 3. Add to pipeline
        zst_pipeline_add(pipe, video_src);
        zst_pipeline_add(pipe, overlay);
        zst_pipeline_add(pipe, h264);
        zst_pipeline_add(pipe, audio_src);
        zst_pipeline_add(pipe, aac);

        // 4. Link pads
        // Video: video_src -> overlay -> h264 -> server (colorbar_video)
        zst_pad_link(zst_element_get_pad(video_src, "src"), zst_element_get_pad(overlay, "sink"));
        zst_pad_link(zst_element_get_pad(overlay, "src"), zst_element_get_pad(h264, "sink"));
        
        char pad_name[128];
        snprintf(pad_name, sizeof(pad_name), "%s_video", name);
        zst_pad_link(zst_element_get_pad(h264, "src"), zst_element_get_pad(server, pad_name));

        // Audio: audio_src -> aac -> server (colorbar_audio)
        zst_pad_link(zst_element_get_pad(audio_src, "src"), zst_element_get_pad(aac, "sink"));
        snprintf(pad_name, sizeof(pad_name), "%s_audio", name);
        zst_pad_link(zst_element_get_pad(aac, "src"), zst_element_get_pad(server, pad_name));

        // 5. Update pipeline execution sorting
        zst_pipeline_topological_sort(pipe);

        // 6. Set states
        zst_element_set_state(video_src, ZST_STATE_READY);
        zst_element_set_state(overlay, ZST_STATE_READY);
        zst_element_set_state(h264, ZST_STATE_READY);
        zst_element_set_state(audio_src, ZST_STATE_READY);
        zst_element_set_state(aac, ZST_STATE_READY);

        zst_element_set_state(video_src, ZST_STATE_PLAYING);
        zst_element_set_state(overlay, ZST_STATE_PLAYING);
        zst_element_set_state(h264, ZST_STATE_PLAYING);
        zst_element_set_state(audio_src, ZST_STATE_PLAYING);
        zst_element_set_state(aac, ZST_STATE_PLAYING);

        printf("[Demo App] Successfully mounted /%s source pipeline\n", name);
        return ZST_OK;
    }
    else if (strcmp(name, "bunny") == 0) {
        printf("[Demo App] Creating HTTP video source pipeline (transcoded & paced)...\n");

        // 1. Add session to server
        if (zst_rtsp_server_add_session(server, name) != ZST_OK) {
            fprintf(stderr, "[Demo App] Failed to add RTSP session for /%s\n", name);
            return ZST_ERROR;
        }

        // 2. Create elements
        zst_element_t* demux = zst_element_factory_make("mp4demux");
        zst_element_t* vdec  = zst_element_factory_make("h264dec");
        zst_element_t* venc  = zst_element_factory_make("x264enc");
        zst_element_t* vq    = zst_element_factory_make("queue");

        zst_element_t* adec  = zst_element_factory_make("aacdec");
        zst_element_t* aenc  = zst_element_factory_make("aacenc");
        zst_element_t* aq    = zst_element_factory_make("queue");

        if (!demux || !vdec || !venc || !vq || !adec || !aenc || !aq) {
            fprintf(stderr, "[Demo App] Failed to create elements for bunny transcoding\n");
            return ZST_ERROR;
        }

        g_bunny.demux = demux;
        g_bunny.vdec  = vdec;
        g_bunny.venc  = venc;
        g_bunny.vq    = vq;
        g_bunny.adec  = adec;
        g_bunny.aenc  = aenc;
        g_bunny.aq    = aq;
        g_bunny.active = 1;
        g_bunny.last_active_time = time(NULL);

        // Configure demux location directly to the HTTPS URL
        const char* url = "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_1MB.mp4";
        zst_element_set_property(demux, "location", url);

        // Configure venc properties
        zst_element_set_property_int(venc, "gop-size", 30);

        // 3. Add to pipeline
        zst_pipeline_add(pipe, demux);
        zst_pipeline_add(pipe, vdec);
        zst_pipeline_add(pipe, venc);
        zst_pipeline_add(pipe, vq);
        zst_pipeline_add(pipe, adec);
        zst_pipeline_add(pipe, aenc);
        zst_pipeline_add(pipe, aq);

        // Disable clock sync and QoS on the queue elements to prevent them from dropping packages
        // due to initial decoding/encoding startup latency. Pacing is already done by demux.
        zst_element_set_clock(vq, NULL);
        zst_element_set_clock(aq, NULL);

        // 4. Set states first so demux opens the file and creates dynamic pads
        zst_element_set_state(demux, ZST_STATE_READY);
        zst_element_set_state(vdec, ZST_STATE_READY);
        zst_element_set_state(venc, ZST_STATE_READY);
        zst_element_set_state(vq, ZST_STATE_READY);
        zst_element_set_state(adec, ZST_STATE_READY);
        zst_element_set_state(aenc, ZST_STATE_READY);
        zst_element_set_state(aq, ZST_STATE_READY);

        zst_element_set_state(demux, ZST_STATE_PLAYING);
        zst_element_set_state(vdec, ZST_STATE_PLAYING);
        zst_element_set_state(venc, ZST_STATE_PLAYING);
        zst_element_set_state(vq, ZST_STATE_PLAYING);
        zst_element_set_state(adec, ZST_STATE_PLAYING);
        zst_element_set_state(aenc, ZST_STATE_PLAYING);
        zst_element_set_state(aq, ZST_STATE_PLAYING);

        // 5. Link pads (demux pads are now dynamic: video_0, audio_0)
        // Video: demux(video_0) -> vdec -> venc -> vq -> server(bunny_video)
        zst_pad_link(zst_element_get_pad(demux, "video_0"), zst_element_get_pad(vdec, "sink"));
        zst_pad_link(zst_element_get_pad(vdec, "src"), zst_element_get_pad(venc, "sink"));
        zst_pad_link(zst_element_get_pad(venc, "src"), zst_element_get_pad(vq, "sink"));
        
        char pad_name[128];
        snprintf(pad_name, sizeof(pad_name), "%s_video", name);
        zst_pad_link(zst_element_get_pad(vq, "src"), zst_element_get_pad(server, pad_name));

        // Audio: demux(audio_0) -> adec -> aenc -> aq -> server(bunny_audio)
        zst_pad_link(zst_element_get_pad(demux, "audio_0"), zst_element_get_pad(adec, "sink"));
        zst_pad_link(zst_element_get_pad(adec, "src"), zst_element_get_pad(aenc, "sink"));
        zst_pad_link(zst_element_get_pad(aenc, "src"), zst_element_get_pad(aq, "sink"));

        snprintf(pad_name, sizeof(pad_name), "%s_audio", name);
        zst_pad_link(zst_element_get_pad(aq, "src"), zst_element_get_pad(server, pad_name));

        // 6. Update pipeline execution sorting
        zst_pipeline_topological_sort(pipe);
        zst_element_set_state(aq, ZST_STATE_PLAYING);

        printf("[Demo App] Successfully mounted /%s HTTP transcode pipeline\n", name);
        fflush(stdout);
        return ZST_OK;
    }


    fprintf(stderr, "[Demo App] Unknown session requested: /%s\n", name);
    return ZST_ERROR;
}

int main(int argc, char** argv) {
    int port = 8554;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // Register built-in elements
    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register builtin elements\n");
        return 1;
    }

    // Create pipeline
    zst_pipeline_t* pipe = zst_pipeline_create();
    if (!pipe) {
        fprintf(stderr, "Failed to create pipeline\n");
        return 1;
    }
    zst_pipeline_set_clock_sync(pipe, 1);

    // Create RTSP server
    zst_element_t* server = zst_rtsp_server_create();
    if (!server) {
        fprintf(stderr, "Failed to create RTSP server\n");
        zst_pipeline_destroy(pipe);
        return 1;
    }

    // Configure port. Force RTP/RTSP over TCP so the demo works reliably when
    // run from Docker with only the RTSP TCP port published (cvlc defaults to
    // UDP otherwise, which cannot be routed back to external clients through
    // Docker port mapping).
    zst_element_set_property_int(server, "listen-port", port);
    // zst_element_set_property_bool(server, "force-tcp", true);

    // Set dynamic mount callback
    zst_rtsp_server_set_mount_callback(server, on_demand_mount, pipe);

    // Add server to pipeline
    zst_pipeline_add(pipe, server);

    // Create scheduler
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    if (!sched) {
        fprintf(stderr, "Failed to create scheduler\n");
        zst_pipeline_destroy(pipe);
        return 1;
    }

    // Attach pipeline and start
    zst_scheduler_attach(sched, pipe);
    zst_pipeline_set_state(pipe, ZST_STATE_READY);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    // Register SIGINT handler
    signal(SIGINT, sigint_handler);

    printf("\n=========================================================\n");
    printf("  RTSP Media-On-Demand Demo App Running on port %d\n", port);
    printf("=========================================================\n");
    printf("  Available streams:\n");
    printf("    rtsp://127.0.0.1:%d/colorbar\n", port);
    printf("    rtsp://127.0.0.1:%d/bunny\n", port);
    printf("\n  Press Ctrl+C to stop the server...\n");
    printf("=========================================================\n\n");

    // Listen to pipeline events (like errors) and run main loop
    while (g_running) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 200);
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "[Pipeline Error] %s (%d)\n",
                        ev->as.error.message ? ev->as.error.message : "unknown",
                        (int)ev->as.error.result);
            } else if (ev->type == ZST_EVENT_EOS && g_bunny.active && ev->src == g_bunny.demux) {
                printf("[Demo App] EOS received from bunny demuxer. Triggering pipeline cleanup...\n");
                fflush(stdout);
                cleanup_bunny_pipeline(pipe, server);
            }
            zst_event_destroy(ev);
        }

        if (g_bunny.active) {
            int clients = zst_rtsp_server_session_client_count(server, "bunny");
            if (clients > 0) {
                g_bunny.last_active_time = time(NULL);
            } else if (time(NULL) - g_bunny.last_active_time >= 2) {
                cleanup_bunny_pipeline(pipe, server);
            }
        }
    }

    printf("\nStopping RTSP server...\n");

    // Clean up
    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    printf("Server stopped cleanly.\n");
    return 0;
}
