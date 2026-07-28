#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>
#include <signal.h>

#include "zst_types.h"
#include "zst_pipeline.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_scheduler.h"
#include "zst_log.h"

static volatile int g_keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_keep_running = 0;
}

int main()
{
    zst_log_set_level(ZST_LOG_LEVEL_INFO);
    zst_log_set_handler(zst_log_default_handler);
    zst_register_builtin_elements();
    
    signal(SIGINT, handle_sigint);

    // Create web_root and live directories
    mkdir("web_root", 0777);
    mkdir("web_root/live", 0777);

    // Create a demo index.html file
    FILE* html = fopen("web_root/index.html", "w");
    if (html) {
        fprintf(html, "<!DOCTYPE html>\n<html>\n<head>\n");
        fprintf(html, "    <title>zstreamer HLS Demo</title>\n");
        fprintf(html, "    <script src=\"https://cdn.jsdelivr.net/npm/hls.js@latest\"></script>\n");
        fprintf(html, "    <style>\n");
        fprintf(html, "        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #0f172a; color: #f8fafc; text-align: center; padding: 20px; }\n");
        fprintf(html, "        h1 { color: #38bdf8; }\n");
        fprintf(html, "        .video-container { max-width: 800px; margin: 0 auto; box-shadow: 0 10px 30px rgba(0,0,0,0.5); border-radius: 12px; overflow: hidden; }\n");
        fprintf(html, "        video { width: 100%%; display: block; }\n");
        fprintf(html, "        .status { margin-top: 20px; font-weight: bold; padding: 10px; border-radius: 8px; display: inline-block; }\n");
        fprintf(html, "        .status.online { background-color: #059669; }\n");
        fprintf(html, "        .status.offline { background-color: #dc2626; }\n");
        fprintf(html, "    </style>\n");
        fprintf(html, "</head>\n<body>\n");
        fprintf(html, "    <h1>zstreamer HLS Live Stream</h1>\n");
        fprintf(html, "    <div class=\"video-container\">\n");
        fprintf(html, "        <video id=\"video\" controls autoplay muted></video>\n");
        fprintf(html, "    </div>\n");
        fprintf(html, "    <div id=\"status\" class=\"status offline\">Connecting...</div>\n");
        fprintf(html, "    <script>\n");
        fprintf(html, "        var video = document.getElementById('video');\n");
        fprintf(html, "        var status = document.getElementById('status');\n");
        fprintf(html, "        var videoSrc = '/live/playlist.m3u8';\n");
        fprintf(html, "        if (Hls.isSupported()) {\n");
        fprintf(html, "            var hls = new Hls({ enableWorker: true, liveSyncDuration: 3 });\n");
        fprintf(html, "            hls.loadSource(videoSrc);\n");
        fprintf(html, "            hls.attachMedia(video);\n");
        fprintf(html, "            hls.on(Hls.Events.MANIFEST_PARSED, function() { video.play(); status.textContent = 'Live'; status.className = 'status online'; });\n");
        fprintf(html, "            hls.on(Hls.Events.ERROR, function(e, data) { if (data.fatal) { status.textContent = 'Stream Offline'; status.className = 'status offline'; } });\n");
        fprintf(html, "        } else if (video.canPlayType('application/vnd.apple.mpegurl')) {\n");
        fprintf(html, "            video.src = videoSrc;\n");
        fprintf(html, "            video.addEventListener('loadedmetadata', function() { video.play(); status.textContent = 'Live'; status.className = 'status online'; });\n");
        fprintf(html, "            video.addEventListener('error', function() { status.textContent = 'Stream Offline'; status.className = 'status offline'; });\n");
        fprintf(html, "        }\n");
        fprintf(html, "    </script>\n");
        fprintf(html, "</body>\n</html>\n");
        fclose(html);
    }

    zst_pipeline_t* p = zst_pipeline_create();

    // 1. Video Pipeline: videotestsrc -> x264enc
    zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
    zst_element_t* vtext = zst_element_factory_make("textoverlay");
    zst_element_t* venc = zst_element_factory_make("x264enc");
    
    // 2. Audio Pipeline: audiotestsrc -> aacenc
    zst_element_t* asrc = zst_element_factory_make("audiotestsrc");
    zst_element_t* aenc = zst_element_factory_make("aacenc");

    // 3. HLS Sink & HTTP Server
    zst_element_t* sink = zst_element_factory_make("hls_sink");
    zst_element_t* server = zst_element_factory_make("http_server");

    if (!vsrc || !venc || !asrc || !aenc || !sink || !server) {
        fprintf(stderr, "Failed to create elements\n");
        return 1;
    }

    // Configure Video Source
    zst_element_set_property(vsrc, "width", "640");
    zst_element_set_property(vsrc, "height", "480");
    zst_element_set_property(vsrc, "fps", "30");
    zst_element_set_property(vsrc, "real-time-pacing", "true"); // Pace for live streaming

    // Configure Text Overlay
    zst_element_set_property(vtext, "timecode", "true");
    zst_element_set_property(vtext, "font-size", "48");

    // Configure Video Encoder
    zst_element_set_property(venc, "gop-size", "60"); // 2 seconds GOP (30fps * 2)

    // Configure Audio Source
    zst_element_set_property(asrc, "real-time-pacing", "true");

    // Configure HLS Sink
    zst_element_set_property(sink, "location", "web_root/live/playlist.m3u8");
    zst_element_set_property(sink, "format", "fmp4");
    zst_element_set_property(sink, "target-duration", "2"); // 2 seconds per segment
    zst_element_set_property(sink, "playlist-length", "10"); // Keep last 10 segments
    zst_element_set_property(sink, "width", "640");
    zst_element_set_property(sink, "height", "480");
    zst_element_set_property(sink, "sample-rate", "44100");
    zst_element_set_property(sink, "channels", "2");

    // Configure HTTP Server
    zst_element_set_property(server, "port", "8080");
    zst_element_set_property(server, "document-root", "web_root");

    // Add elements to pipeline
    zst_pipeline_add(p, vsrc);
    zst_pipeline_add(p, vtext);
    zst_pipeline_add(p, venc);
    zst_pipeline_add(p, asrc);
    zst_pipeline_add(p, aenc);
    zst_pipeline_add(p, sink);
    zst_pipeline_add(p, server);

    // Link Video
    zst_pad_t* vsrc_src = zst_element_get_pad(vsrc, "src");
    zst_pad_t* vtext_sink = zst_element_get_pad(vtext, "sink");
    zst_pad_t* vtext_src = zst_element_get_pad(vtext, "src");
    zst_pad_t* venc_sink = zst_element_get_pad(venc, "sink");
    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* sink_v = zst_element_get_pad(sink, "video");
    
    zst_pad_link(vsrc_src, vtext_sink);
    zst_pad_link(vtext_src, venc_sink);
    zst_pad_link(venc_src, sink_v);
    
    // Link Audio
    zst_pad_t* asrc_src = zst_element_get_pad(asrc, "src");
    zst_pad_t* aenc_sink = zst_element_get_pad(aenc, "sink");
    zst_pad_t* aenc_src = zst_element_get_pad(aenc, "src");
    zst_pad_t* sink_a = zst_element_get_pad(sink, "audio");

    zst_pad_link(asrc_src, aenc_sink);
    zst_pad_link(aenc_src, sink_a);

    // Setup scheduler
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, p);
    
    printf("Starting HLS streaming demo...\n");
    printf("HLS stream is available at http://127.0.0.1:8080/live/playlist.m3u8\n");
    printf("Open http://127.0.0.1:8080/index.html in your browser to view.\n");
    printf("Press Ctrl+C to stop.\n");

    zst_pipeline_set_state(p, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    while (g_keep_running) {
        sleep(1);
    }

    printf("Stopping HLS streaming demo...\n");

    zst_scheduler_stop(sched);
    zst_pipeline_set_state(p, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(p);

    return 0;
}
