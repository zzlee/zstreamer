#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_bus.h"

#define CHECK_OK(expr, label) \
    do { \
        zst_result_t _r = (expr); \
        if (_r != ZST_OK) { \
            fprintf(stderr, "%s failed: %d\n", (label), (int)_r); \
            goto fail; \
        } \
    } while (0)

#define CHECK_PTR(ptr, label) \
    do { \
        if (!(ptr)) { \
            fprintf(stderr, "%s failed\n", (label)); \
            goto fail; \
        } \
    } while (0)

int main(int argc, char** argv)
{
    const char* output = argc > 1 ? argv[1] : "vaapi_output.h264";
    const int width = 640;
    const int height = 480;
    const int fps = 30;
    const int seconds = 5;

    zst_pipeline_t* pipe = NULL;
    zst_scheduler_t* sched = NULL;
    zst_element_t* video_src = NULL;
    zst_element_t* vaapi_enc = NULL;
    zst_element_t* sink = NULL;
    int rc = 1;

    CHECK_OK(zst_register_builtin_elements(), "register builtins");

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    video_src = zst_element_factory_make("videotestsrc");
    vaapi_enc = zst_element_factory_make("vaapienc");
    sink      = zst_element_factory_make("filesink");

    CHECK_PTR(video_src, "factory make videotestsrc");
    CHECK_PTR(vaapi_enc, "factory make vaapienc");
    CHECK_PTR(sink,      "factory make filesink");

    CHECK_OK(zst_element_set_property_int(video_src, "width",       width),    "video width");
    CHECK_OK(zst_element_set_property_int(video_src, "height",      height),   "video height");
    CHECK_OK(zst_element_set_property_int(video_src, "fps",         fps),      "video fps");
    CHECK_OK(zst_element_set_property_string(video_src, "pattern",  "bars"),   "video pattern");
    CHECK_OK(zst_element_set_property_int(video_src, "num-buffers", fps * seconds), "video num-buffers");
    CHECK_OK(zst_element_set_property_bool(video_src, "use-clock",  false),    "video use-clock");

    CHECK_OK(zst_element_set_property_string(vaapi_enc, "codec", "h264"), "vaapi codec");
    CHECK_OK(zst_element_set_property_int(vaapi_enc, "bitrate", 2000000), "vaapi bitrate");

    CHECK_OK(zst_element_set_property_string(sink, "location", output), "sink location");

    CHECK_OK(zst_pipeline_add(pipe, video_src), "add video_src");
    CHECK_OK(zst_pipeline_add(pipe, vaapi_enc), "add vaapi_enc");
    CHECK_OK(zst_pipeline_add(pipe, sink),      "add sink");

    CHECK_OK(zst_pad_link(zst_element_get_pad(video_src, "src"),
                          zst_element_get_pad(vaapi_enc, "sink")), "link video_src->vaapi_enc");
    CHECK_OK(zst_pad_link(zst_element_get_pad(vaapi_enc, "src"),
                          zst_element_get_pad(sink,  "sink")), "link vaapi_enc->sink");

    CHECK_OK(zst_scheduler_attach(sched, pipe), "scheduler attach");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY),  "pipeline READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "pipeline PLAYING");
    CHECK_OK(zst_scheduler_run(sched), "scheduler run");

    printf("Writing %ds %dx%d H.264 to %s using VA-API ...\n", seconds, width, height, output);

    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 15000);
        if (r == ZST_TIMEOUT) {
            fprintf(stderr, "Timed out waiting for EOS\n");
            break;
        }
        if (r != ZST_OK || !ev) {
            fprintf(stderr, "Bus error while waiting for EOS: %d\n", (int)r);
            break;
        }
        if (ev->type == ZST_EVENT_ERROR) {
            fprintf(stderr, "Pipeline error: %s (%d)\n",
                    ev->as.error.message ? ev->as.error.message : "unknown",
                    (int)ev->as.error.result);
            zst_event_destroy(ev);
            break;
        }
        if (ev->type == ZST_EVENT_EOS) {
            zst_event_destroy(ev);
            rc = 0;
            break;
        }
        zst_event_destroy(ev);
    }

fail:
    if (sched) zst_scheduler_stop(sched);
    if (pipe) zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    if (sched) zst_scheduler_destroy(sched);
    if (pipe) zst_pipeline_destroy(pipe);

    if (rc == 0) {
        printf("Done: %s\n", output);
    }
    return rc;
}
