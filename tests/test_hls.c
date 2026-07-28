#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>

#include "zst_types.h"
#include "zst_pipeline.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_scheduler.h"
#include "zst_log.h"

int main()
{
    zst_log_set_level(ZST_LOG_LEVEL_DEBUG);
    zst_log_set_handler(zst_log_default_handler);
    zst_register_builtin_elements();

    zst_pipeline_t* p = zst_pipeline_create();

    zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
    zst_element_t* venc = zst_element_factory_make("x264enc");
    zst_element_t* asrc = zst_element_factory_make("audiotestsrc");
    zst_element_t* aenc = zst_element_factory_make("aacenc");
    zst_element_t* sink = zst_element_factory_make("hls_sink");
    zst_element_t* server = zst_element_factory_make("http_server");

    if (!vsrc) fprintf(stderr, "vsrc is NULL\n");
    if (!venc) fprintf(stderr, "venc is NULL\n");
    if (!asrc) fprintf(stderr, "asrc is NULL\n");
    if (!aenc) fprintf(stderr, "aenc is NULL\n");
    if (!sink) fprintf(stderr, "sink is NULL\n");
    if (!server) fprintf(stderr, "server is NULL\n");
    assert(vsrc && venc && asrc && aenc && sink && server);

    zst_element_set_property(vsrc, "width", "640");
    zst_element_set_property(vsrc, "height", "480");
    zst_element_set_property(vsrc, "fps", "30");

    zst_element_set_property(venc, "gop-size", "30"); // 1 second GOP

    zst_element_set_property(sink, "location", "test_hls/playlist.m3u8");
    zst_element_set_property(sink, "format", "fmp4");
    zst_element_set_property(sink, "target-duration", "2"); // 2 seconds per segment
    zst_element_set_property(sink, "playlist-length", "5");
    zst_element_set_property(sink, "width", "640");
    zst_element_set_property(sink, "height", "480");
    zst_element_set_property(sink, "sample-rate", "44100");
    zst_element_set_property(sink, "channels", "2");

    zst_element_set_property(server, "port", "8080");
    zst_element_set_property(server, "document-root", "test_hls");

    zst_pipeline_add(p, vsrc);
    zst_pipeline_add(p, venc);
    zst_pipeline_add(p, asrc);
    zst_pipeline_add(p, aenc);
    zst_pipeline_add(p, sink);
    zst_pipeline_add(p, server);

    zst_pad_t* vsrc_src = zst_element_get_pad(vsrc, "src");
    zst_pad_t* venc_sink = zst_element_get_pad(venc, "sink");
    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* sink_v = zst_element_get_pad(sink, "video");
    
    zst_pad_t* asrc_src = zst_element_get_pad(asrc, "src");
    zst_pad_t* aenc_sink = zst_element_get_pad(aenc, "sink");
    zst_pad_t* aenc_src = zst_element_get_pad(aenc, "src");
    zst_pad_t* sink_a = zst_element_get_pad(sink, "audio");

    zst_pad_link(vsrc_src, venc_sink);
    zst_pad_link(venc_src, sink_v);
    
    zst_pad_link(asrc_src, aenc_sink);
    zst_pad_link(aenc_src, sink_a);

    // Create dir
    mkdir("test_hls", 0777);

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, p);
    zst_pipeline_set_state(p, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    // Sleep for 6 seconds to generate a few segments
    sleep(6);
    
    // Verify HTTP server works while it's still running
    int curl_ret = system("curl -s -f http://127.0.0.1:8080/playlist.m3u8 > /dev/null");
    assert(curl_ret == 0);

    zst_scheduler_stop(sched);
    zst_pipeline_set_state(p, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(p);

    // Verify files exist
    FILE* m3u8 = fopen("test_hls/playlist.m3u8", "r");
    assert(m3u8 != NULL);
    fclose(m3u8);
    
    FILE* init = fopen("test_hls/init.mp4", "r");
    assert(init != NULL);
    fclose(init);

    FILE* seg0 = fopen("test_hls/playlist0.m4s", "r");
    assert(seg0 != NULL);
    fclose(seg0);

    printf("HLS sink test passed!\n");
    
    // Cleanup
    system("rm -rf test_hls");
    
    return 0;
}
