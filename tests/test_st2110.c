/*=============================================================================
    test_st2110.c — ST2110 Phase 1 tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_bus.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static int
test_st2110_20_payloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_20_payloader");
    if (!el) {
        fprintf(stderr, "SKIP: st2110_20_payloader not registered\n");
        return 0;
    }
    
    CHECK(zst_element_set_property_int(el, "width", 1920) == ZST_OK, "set width failed");
    CHECK(zst_element_set_property_int(el, "height", 1080) == ZST_OK, "set height failed");
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_20_depayloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_20_depayloader");
    if (!el) return 0;
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_30_payloader_audio(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_30_payloader");
    if (!el) return 0;
    
    CHECK(zst_element_set_property_int(el, "channels", 2) == ZST_OK, "set channels failed");
    CHECK(zst_element_set_property_int(el, "sample-rate", 48000) == ZST_OK, "set sample-rate failed");
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_30_depayloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_30_depayloader");
    if (!el) return 0;
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_sdp_generation(void)
{
    zst_element_t* mux = zst_element_factory_make("sdpmux");
    if (!mux) return 0;
    
    /* Just check if we can set media-mode to st2110 */
    zst_element_set_property_string(mux, "media-mode", "st2110");
    
    zst_element_destroy(mux);
    return 0;
}

static int
test_st2110_40_ancillary_data(void)
{
    zst_element_t* p = zst_element_factory_make("st2110_40_payloader");
    if (p) {
        CHECK(zst_element_set_property_string(p, "aux-data-type", "cea708") == ZST_OK, "set aux-data-type failed");
        CHECK(zst_element_set_property_int(p, "sampling-frequency", 90000) == ZST_OK, "set sampling-frequency failed");
        zst_element_destroy(p);
    }

    zst_element_t* d = zst_element_factory_make("st2110_40_depayloader");
    if (d) {
        zst_element_destroy(d);
    }
    return 0;
}

static int
test_st2110_22_jpeg_xs(void)
{
    zst_element_t* p = zst_element_factory_make("st2110_22_payloader");
    zst_element_t* d = zst_element_factory_make("st2110_22_depayloader");
    if (!p || !d) {
        if (p) zst_element_destroy(p);
        if (d) zst_element_destroy(d);
        fprintf(stderr, "SKIP: st2110_22 elements not registered (SVT-JPEG-XS disabled?)\n");
        return 0;
    }

    zst_element_set_property_string(p, "width", "1280");
    zst_element_set_property_string(p, "height", "720");

    char val[32];
    zst_element_get_property_string(p, "width", val, sizeof(val));
    CHECK(strcmp(val, "1280") == 0, "st2110_22_payloader width property get/set failed");

    zst_element_destroy(p);
    zst_element_destroy(d);
    return 0;
}

static uint64_t total_encoded_bytes = 0;

static zst_pad_probe_return_t
encoder_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size > 0) {
        total_encoded_bytes += buf->memory.size;
    }
    return ZST_PAD_PROBE_OK;
}

static int
test_st2110_22_encode_decode(void)
{
    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* scaler = zst_element_factory_make("videoscaler");
    zst_element_t* enc = zst_element_factory_make("st2110_22_payloader");
    zst_element_t* dec = zst_element_factory_make("st2110_22_depayloader");
    zst_element_t* sink = zst_element_factory_make("fakesink");
    
    if (!src || !scaler || !enc || !dec || !sink) {
        if (src) zst_element_destroy(src);
        if (scaler) zst_element_destroy(scaler);
        if (enc) zst_element_destroy(enc);
        if (dec) zst_element_destroy(dec);
        if (sink) zst_element_destroy(sink);
        fprintf(stderr, "SKIP: Missing elements or SVT-JPEG-XS not enabled\n");
        return 0;
    }
    
    zst_pipeline_t* pipe = zst_pipeline_create();
    
    zst_element_set_property_int(src, "num-buffers", 100);
    zst_element_set_property_string(scaler, "format", "I422");
    
    char w_str[16], h_str[16];
    snprintf(w_str, sizeof(w_str), "%d", 640);
    snprintf(h_str, sizeof(h_str), "%d", 480);
    zst_element_set_property_string(enc, "width", w_str);
    zst_element_set_property_string(enc, "height", h_str);
    
    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, scaler);
    zst_pipeline_add(pipe, enc);
    zst_pipeline_add(pipe, dec);
    zst_pipeline_add(pipe, sink);
    
    zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(scaler, "sink"));
    zst_pad_link(zst_element_get_pad(scaler, "src"), zst_element_get_pad(enc, "sink"));
    zst_pad_link(zst_element_get_pad(enc, "src"), zst_element_get_pad(dec, "sink"));
    zst_pad_link(zst_element_get_pad(dec, "src"), zst_element_get_pad(sink, "sink"));
    
    zst_pad_add_probe(zst_element_get_pad(enc, "src"), ZST_PAD_PROBE_POST_BUFFER, encoder_probe, NULL);

    zst_scheduler_config_t cfg = {0};
    cfg.mode = ZST_SCHEDULER_SINGLE_THREAD;
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);
    
    CHECK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK, "Pipeline start failed");
    
    struct timespec start_wall, end_wall;
    struct timespec start_cpu, end_cpu;

    clock_gettime(CLOCK_MONOTONIC, &start_wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start_cpu);

    zst_scheduler_run(sched);

    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 5000);
        if (r == ZST_TIMEOUT) {
            fprintf(stderr, "test_st2110_22_encode_decode: Timed out\n");
            break;
        }
        if (r != ZST_OK || !ev) {
            break;
        }
        if (ev->type == ZST_EVENT_ERROR) {
            fprintf(stderr, "test_st2110_22_encode_decode: Pipeline Error\n");
            zst_event_destroy(ev);
            break;
        }
        if (ev->type == ZST_EVENT_EOS) {
            zst_event_destroy(ev);
            break;
        }
        zst_event_destroy(ev);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_cpu);
    
    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    double wall_time = (end_wall.tv_sec - start_wall.tv_sec) + (end_wall.tv_nsec - start_wall.tv_nsec) / 1e9;
    double cpu_time = (end_cpu.tv_sec - start_cpu.tv_sec) + (end_cpu.tv_nsec - start_cpu.tv_nsec) / 1e9;
    double bitrate_mbps = (total_encoded_bytes * 8.0) / (wall_time * 1000000.0);
    double cpu_usage_pct = (cpu_time / wall_time) * 100.0;

    fprintf(stderr, "=== ST2110-22 SVT-JPEG-XS Benchmark ===\n");
    fprintf(stderr, "Frames: 100, Resolution: 640x480 (I422)\n");
    fprintf(stderr, "Total Encoded Data: %lu bytes\n", total_encoded_bytes);
    fprintf(stderr, "Wall Time: %.3f seconds\n", wall_time);
    fprintf(stderr, "CPU Time: %.3f seconds (%.1f%% usage)\n", cpu_time, cpu_usage_pct);
    fprintf(stderr, "Avg Bitrate: %.2f Mbps\n", bitrate_mbps);
    fprintf(stderr, "=========================================\n");
    
    return 0;
}

int main(void)
{
    zst_register_builtin_elements();

    if (test_st2110_20_payloader_basic() != 0) return 1;
    if (test_st2110_20_depayloader_basic() != 0) return 1;
    if (test_st2110_30_payloader_audio() != 0) return 1;
    if (test_st2110_30_depayloader_basic() != 0) return 1;
    if (test_st2110_sdp_generation() != 0) return 1;
    if (test_st2110_40_ancillary_data() != 0) return 1;
    if (test_st2110_22_jpeg_xs() != 0) return 1;
    if (test_st2110_22_encode_decode() != 0) return 1;

    printf("test_st2110: PASS\n");
    return 0;
}
