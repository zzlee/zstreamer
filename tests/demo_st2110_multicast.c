/*=============================================================================
    demo_st2110_multicast.c — SMPTE ST2110 Phase 1 Integration Test

    Terminal 1 (Sender):
      ./demo_st2110_multicast send 239.255.42.42 10

    Terminal 2 (Receiver):
      ./demo_st2110_multicast recv 239.255.42.42 10

    Sender pipeline:
      Video: videotestsrc -> videoscaler(I422_BE) -> st2110_20_payloader -> netsink
      Audio: audiotestsrc -> st2110_30_payloader -> netsink

    Receiver pipeline:
      Video: netsource -> st2110_20_depayloader -> fakesink
      Audio: netsource -> st2110_30_depayloader -> fakesink
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

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

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int run_sender(const char* group, int seconds) {
    zst_pipeline_t* pipe = NULL;
    zst_scheduler_t* sched = NULL;
    zst_element_t* vsrc = NULL;
    zst_element_t* vscale = NULL;
    zst_element_t* vpay = NULL;
    zst_element_t* vsink = NULL;
    zst_element_t* asrc = NULL;
    zst_element_t* apay = NULL;
    zst_element_t* asink = NULL;

    printf("Starting ST2110 Sender to %s for %d seconds\n", group, seconds);

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    /* Create video elements */
    vsrc   = zst_element_factory_make("videotestsrc");
    vscale = zst_element_factory_make("videoscaler");
    vpay   = zst_element_factory_make("st2110_20_payloader");
    vsink  = zst_element_factory_make("netsink");
    CHECK_PTR(vsrc, "vsrc");
    CHECK_PTR(vscale, "vscale");
    CHECK_PTR(vpay, "vpay");
    CHECK_PTR(vsink, "vsink");

    /* Create audio elements */
    asrc  = zst_element_factory_make("audiotestsrc");
    apay  = zst_element_factory_make("st2110_30_payloader");
    asink = zst_element_factory_make("netsink");
    CHECK_PTR(asrc, "asrc");
    CHECK_PTR(apay, "apay");
    CHECK_PTR(asink, "asink");

    /* Configure Video */
    CHECK_OK(zst_element_set_property_int(vsrc, "width", 1280), "vsrc width");
    CHECK_OK(zst_element_set_property_int(vsrc, "height", 720), "vsrc height");
    CHECK_OK(zst_element_set_property_int(vsrc, "fps", 30), "vsrc fps");
    CHECK_OK(zst_element_set_property_bool(vsrc, "use-clock", true), "vsrc use-clock");
    CHECK_OK(zst_element_set_property_bool(vsrc, "real-time-pacing", true), "vsrc real-time-pacing");
    
    CHECK_OK(zst_element_set_property_string(vscale, "pixel-format", "I422_BE"), "vscale format");

    CHECK_OK(zst_element_set_property_int(vpay, "width", 1280), "vpay width");
    CHECK_OK(zst_element_set_property_int(vpay, "height", 720), "vpay height");

    CHECK_OK(zst_element_set_property_string(vsink, "protocol", "udp"), "vsink protocol");
    CHECK_OK(zst_element_set_property_string(vsink, "host", group), "vsink host");
    CHECK_OK(zst_element_set_property_int(vsink, "port", 5004), "vsink port");
    CHECK_OK(zst_element_set_property_bool(vsink, "timestamp-pacing", true), "vsink timestamp-pacing");

    /* Configure Audio */
    CHECK_OK(zst_element_set_property_int(asrc, "sample-rate", 48000), "asrc sample-rate");
    CHECK_OK(zst_element_set_property_int(apay, "bit-depth", 16), "apay bit-depth");
    CHECK_OK(zst_element_set_property_int(apay, "channels", 2), "apay channels");
    CHECK_OK(zst_element_set_property_int(asrc, "channels", 2), "asrc channels");
    CHECK_OK(zst_element_set_property_string(asrc, "sample-format", "S16LE"), "asrc format");
    CHECK_OK(zst_element_set_property_bool(asrc, "use-clock", true), "asrc use-clock");
    CHECK_OK(zst_element_set_property_bool(asrc, "real-time-pacing", true), "asrc real-time-pacing");

    CHECK_OK(zst_element_set_property_string(asink, "protocol", "udp"), "asink protocol");
    CHECK_OK(zst_element_set_property_string(asink, "host", group), "asink host");
    CHECK_OK(zst_element_set_property_int(asink, "port", 5006), "asink port");
    CHECK_OK(zst_element_set_property_bool(asink, "timestamp-pacing", true), "asink timestamp-pacing");

    /* Build Pipeline */
    CHECK_OK(zst_pipeline_add(pipe, vsrc), "add vsrc");
    CHECK_OK(zst_pipeline_add(pipe, vscale), "add vscale");
    CHECK_OK(zst_pipeline_add(pipe, vpay), "add vpay");
    CHECK_OK(zst_pipeline_add(pipe, vsink), "add vsink");

    CHECK_OK(zst_pipeline_add(pipe, asrc), "add asrc");
    CHECK_OK(zst_pipeline_add(pipe, apay), "add apay");
    CHECK_OK(zst_pipeline_add(pipe, asink), "add asink");

    /* Link Video */
    CHECK_OK(zst_pad_link(zst_element_get_pad(vsrc, "src"), zst_element_get_pad(vscale, "sink")), "link vsrc->vscale");
    CHECK_OK(zst_pad_link(zst_element_get_pad(vscale, "src"), zst_element_get_pad(vpay, "sink")), "link vscale->vpay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(vpay, "src"), zst_element_get_pad(vsink, "sink")), "link vpay->vsink");

    /* Link Audio */
    CHECK_OK(zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(apay, "sink")), "link asrc->apay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(apay, "src"), zst_element_get_pad(asink, "sink")), "link apay->asink");

    /* Run */
    CHECK_OK(zst_scheduler_attach(sched, pipe), "scheduler attach");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "pipeline PLAYING");
    CHECK_OK(zst_scheduler_run(sched), "scheduler run");

    for (int i = 0; i < seconds && !g_stop; i++) {
        sleep(1);
    }

    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    zst_scheduler_stop(sched);
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    zst_scheduler_stop(sched);
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_NULL), "pipeline NULL");

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
    return 0;

fail:
    if (sched) zst_scheduler_destroy(sched);
    if (pipe) zst_pipeline_destroy(pipe);
    return 1;
}

static int run_receiver(const char* group, int seconds) {
    zst_pipeline_t* pipe = NULL;
    zst_scheduler_t* sched = NULL;
    zst_element_t* vsrc = NULL;
    zst_element_t* vdepay = NULL;
    zst_element_t* vsink = NULL;
    zst_element_t* asrc = NULL;
    zst_element_t* adepay = NULL;
    zst_element_t* asink = NULL;

    printf("Starting ST2110 Receiver from %s for %d seconds\n", group, seconds);

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    /* Create video elements */
    vsrc   = zst_element_factory_make("netsource");
    vdepay = zst_element_factory_make("st2110_20_depayloader");
    vsink  = zst_element_factory_make("fakesink");
    CHECK_PTR(vsrc, "vsrc");
    CHECK_PTR(vdepay, "vdepay");
    CHECK_PTR(vsink, "vsink");

    /* Create audio elements */
    asrc   = zst_element_factory_make("netsource");
    adepay = zst_element_factory_make("st2110_30_depayloader");
    asink  = zst_element_factory_make("fakesink");
    CHECK_PTR(asrc, "asrc");
    CHECK_PTR(adepay, "adepay");
    CHECK_PTR(asink, "asink");

    /* Configure Video */
    CHECK_OK(zst_element_set_property_string(vsrc, "protocol", "udp"), "vsrc protocol");
    CHECK_OK(zst_element_set_property_string(vsrc, "host", group), "vsrc host");
    CHECK_OK(zst_element_set_property_int(vsrc, "port", 5004), "vsrc port");

    CHECK_OK(zst_element_set_property_bool(vsink, "push-per-second", true), "vsink push-per-second");
    CHECK_OK(zst_element_set_property_int(vsink, "log-period", 1), "vsink log-period");

    /* Configure Audio */
    CHECK_OK(zst_element_set_property_string(asrc, "protocol", "udp"), "asrc protocol");
    CHECK_OK(zst_element_set_property_string(asrc, "host", group), "asrc host");
    CHECK_OK(zst_element_set_property_int(asrc, "port", 5006), "asrc port");

    CHECK_OK(zst_element_set_property_bool(asink, "push-per-second", true), "asink push-per-second");
    CHECK_OK(zst_element_set_property_int(asink, "log-period", 1), "asink log-period");

    /* Build Pipeline */
    CHECK_OK(zst_pipeline_add(pipe, vsrc), "add vsrc");
    CHECK_OK(zst_pipeline_add(pipe, vdepay), "add vdepay");
    CHECK_OK(zst_pipeline_add(pipe, vsink), "add vsink");

    CHECK_OK(zst_pipeline_add(pipe, asrc), "add asrc");
    CHECK_OK(zst_pipeline_add(pipe, adepay), "add adepay");
    CHECK_OK(zst_pipeline_add(pipe, asink), "add asink");

    /* Link Video */
    CHECK_OK(zst_pad_link(zst_element_get_pad(vsrc, "src"), zst_element_get_pad(vdepay, "sink")), "link vsrc->vdepay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(vdepay, "src"), zst_element_get_pad(vsink, "sink")), "link vdepay->vsink");

    /* Link Audio */
    CHECK_OK(zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(adepay, "sink")), "link asrc->adepay");
    CHECK_OK(zst_pad_link(zst_element_get_pad(adepay, "src"), zst_element_get_pad(asink, "sink")), "link adepay->asink");

    /* Run */
    CHECK_OK(zst_scheduler_attach(sched, pipe), "scheduler attach");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "pipeline PLAYING");
    CHECK_OK(zst_scheduler_run(sched), "scheduler run");

    for (int i = 0; i < seconds && !g_stop; i++) {
        sleep(1);
    }

    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    zst_scheduler_stop(sched);
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "pipeline READY");
    zst_scheduler_stop(sched);
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_NULL), "pipeline NULL");

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
    return 0;

fail:
    if (sched) zst_scheduler_destroy(sched);
    if (pipe) zst_pipeline_destroy(pipe);
    return 1;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s send [multicast-group] [seconds]\n"
            "  %s recv [multicast-group] [seconds]\n"
            "\nDefaults: group=239.255.42.42 seconds=10\n",
            argv0, argv0);
}

int main(int argc, char** argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (zst_register_builtin_elements() != ZST_OK) {
        fprintf(stderr, "Failed to register built-in elements\n");
        return 1;
    }

    const char* mode = argc > 1 ? argv[1] : NULL;
    const char* group = argc > 2 ? argv[2] : "239.255.42.42";
    int seconds = argc > 3 ? atoi(argv[3]) : 10;
    if (seconds <= 0) seconds = 10;

    if (!mode) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(mode, "send") == 0) return run_sender(group, seconds);
    if (strcmp(mode, "recv") == 0) return run_receiver(group, seconds);

    usage(argv[0]);
    return 2;
}
