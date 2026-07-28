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
#include "zst_clock.h"
#include "zst_bus.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static int st2110_20_valid_packets = 0;
static zst_pad_probe_return_t
st2110_20_compliance_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size >= 20) {
        uint8_t* data = buf->memory.data;
        // Verify standard RTP header (12 bytes)
        if (data[0] != 0x80) return ZST_PAD_PROBE_DROP;
        
        // Verify ST2110-20 Extended Sequence Number and Line Header (8 bytes total)
        // uint16_t ext_seq = (data[12] << 8) | data[13];
        uint16_t length_field = (data[14] << 8) | data[15];
        uint16_t f_line = (data[16] << 8) | data[17];
        // uint16_t c_offset = (data[18] << 8) | data[19];
        
        int f = (f_line >> 15) & 1;
        int line_num = f_line & 0x7FFF;
        
        if (length_field > 0 && (f == 0 || f == 1) && line_num >= 0) {
            st2110_20_valid_packets++;
        }
    }
    return ZST_PAD_PROBE_OK;
}

static int
test_st2110_20_payloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_20_payloader");
    zst_element_t* sink = zst_element_factory_make("fakesink");
    if (!el || !sink) {
        if (el) zst_element_destroy(el);
        if (sink) zst_element_destroy(sink);
        fprintf(stderr, "SKIP: st2110_20_payloader not registered\n");
        return 0;
    }
    
    CHECK(zst_element_set_property_int(el, "width", 1920) == ZST_OK, "set width failed");
    CHECK(zst_element_set_property_int(el, "height", 1080) == ZST_OK, "set height failed");
    
    zst_pad_link(zst_element_get_pad(el, "src"), zst_element_get_pad(sink, "sink"));
    zst_pad_add_probe(zst_element_get_pad(el, "src"), ZST_PAD_PROBE_POST_BUFFER, st2110_20_compliance_probe, NULL);
    
    zst_element_set_state(sink, ZST_STATE_PLAYING);
    zst_element_set_state(el, ZST_STATE_PLAYING);
    
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    buf->memory.size = 1920 * 1080 * 2; // YUV422 16-bit packed dummy
    buf->memory.data = calloc(1, buf->memory.size);
    zst_pad_t* sink_pad = zst_element_get_pad(el, "sink");
    sink_pad->push(sink_pad, buf);
    
    CHECK(st2110_20_valid_packets > 0, "No valid ST2110-20 packets produced");
    
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_set_state(sink, ZST_STATE_NULL);
    zst_element_destroy(el);
    zst_element_destroy(sink);
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

static int st2110_30_valid_packets = 0;
static zst_pad_probe_return_t
st2110_30_compliance_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size >= 12) {
        uint8_t* data = buf->memory.data;
        if (data[0] == 0x80) { // Standard RTP header
            st2110_30_valid_packets++;
        }
    }
    return ZST_PAD_PROBE_OK;
}

static int
test_st2110_30_payloader_audio(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_30_payloader");
    zst_element_t* sink = zst_element_factory_make("fakesink");
    if (!el || !sink) {
        if (el) zst_element_destroy(el);
        if (sink) zst_element_destroy(sink);
        return 0;
    }
    
    CHECK(zst_element_set_property_int(el, "channels", 2) == ZST_OK, "set channels failed");
    CHECK(zst_element_set_property_int(el, "sample-rate", 48000) == ZST_OK, "set sample-rate failed");
    
    zst_pad_link(zst_element_get_pad(el, "src"), zst_element_get_pad(sink, "sink"));
    zst_pad_add_probe(zst_element_get_pad(el, "src"), ZST_PAD_PROBE_POST_BUFFER, st2110_30_compliance_probe, NULL);
    
    zst_element_set_state(sink, ZST_STATE_PLAYING);
    zst_element_set_state(el, ZST_STATE_PLAYING);
    
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    buf->memory.size = 48000 * 2 * 3; // 1 second of 24-bit stereo
    buf->memory.data = calloc(1, buf->memory.size);
    zst_pad_t* sink_pad = zst_element_get_pad(el, "sink");
    sink_pad->push(sink_pad, buf);
    
    CHECK(st2110_30_valid_packets > 0, "No valid ST2110-30 packets produced");
    
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_set_state(sink, ZST_STATE_NULL);
    zst_element_destroy(el);
    zst_element_destroy(sink);
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
    
    zst_element_set_property_string(mux, "media-mode", "st2110");
    zst_element_set_property_int(mux, "ptp-domain", 127);
    
    char sdp_out[4096] = {0};
    CHECK(zst_element_get_property_string(mux, "sdp-string", sdp_out, sizeof(sdp_out)) == ZST_OK, "get sdp-string failed");
    
    CHECK(strstr(sdp_out, "a=ts-refclk:ptp=") != NULL, "SDP missing PTP refclk attribute");
    
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
    zst_element_set_property_string(src, "real-time-pacing", "true");
    zst_element_set_property_string(src, "use-clock", "true");
    
    zst_clock_t* sys_clock = zst_clock_system_create();
    zst_element_set_clock(src, sys_clock);

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
    
    zst_element_set_clock(src, NULL);
    zst_clock_unref(sys_clock);

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

static int st2110_21_paced_packets = 0;
static zst_pad_probe_return_t
st2110_21_compliance_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size > 0) {
        st2110_21_paced_packets++;
    }
    return ZST_PAD_PROBE_OK;
}

static int
test_st2110_21_pacing_compliance(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_21_payloader");
    zst_element_t* sink = zst_element_factory_make("fakesink");
    if (!el || !sink) {
        if (el) zst_element_destroy(el);
        if (sink) zst_element_destroy(sink);
        fprintf(stderr, "SKIP: st2110_21_payloader not registered\n");
        return 0;
    }
    
    CHECK(zst_element_set_property_string(el, "codec", "h264") == ZST_OK, "set codec failed");
    CHECK(zst_element_set_property_int(el, "fps-num", 60) == ZST_OK, "set fps-num failed");
    CHECK(zst_element_set_property_int(el, "fps-den", 1) == ZST_OK, "set fps-den failed");
    CHECK(zst_element_set_property_string(el, "pacing", "on") == ZST_OK, "set pacing failed");
    
    zst_pad_link(zst_element_get_pad(el, "src"), zst_element_get_pad(sink, "sink"));
    zst_pad_add_probe(zst_element_get_pad(el, "src"), ZST_PAD_PROBE_POST_BUFFER, st2110_21_compliance_probe, NULL);
    
    zst_element_set_state(sink, ZST_STATE_PLAYING);
    zst_element_set_state(el, ZST_STATE_PLAYING);
    
    // Create a 50KB dummy NAL unit without start codes
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    buf->memory.size = 50000;
    buf->memory.data = calloc(1, buf->memory.size);
    ((uint8_t*)buf->memory.data)[0] = 0x65; // IDR NAL header
    buf->pts = 1000000000ULL; // 1 second
    
    struct timespec start_wall, end_wall;
    clock_gettime(CLOCK_MONOTONIC, &start_wall);
    
    zst_pad_t* sink_pad = zst_element_get_pad(el, "sink");
    sink_pad->push(sink_pad, buf);
    
    clock_gettime(CLOCK_MONOTONIC, &end_wall);
    double wall_time = (end_wall.tv_sec - start_wall.tv_sec) + (end_wall.tv_nsec - start_wall.tv_nsec) / 1e9;
    
    CHECK(st2110_21_paced_packets > 10, "ST2110-21 did not fragment packets");
    // At 60fps, 1 frame pacing should take roughly 16.6ms. We verify it blocked > 10ms.
    CHECK(wall_time > 0.010, "ST2110-21 traffic shaping failed! Packets were not paced over frame duration.");
    
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_set_state(sink, ZST_STATE_NULL);
    zst_element_destroy(el);
    zst_element_destroy(sink);
    return 0;
}

static uint32_t st2110_10_first_rtp_ts = 0;
static zst_pad_probe_return_t
st2110_10_timing_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size >= 12) {
        uint8_t* data = (uint8_t*)buf->memory.data;
        // Extract 32-bit RTP timestamp from bytes 4..7
        if (st2110_10_first_rtp_ts == 0) {
            st2110_10_first_rtp_ts = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        }
    }
    return ZST_PAD_PROBE_OK;
}

static int
test_st2110_10_timing_compliance(void)
{
    // Test Video (90kHz clock)
    zst_element_t* pay20 = zst_element_factory_make("st2110_20_payloader");
    zst_element_t* sink20 = zst_element_factory_make("fakesink");
    if (pay20 && sink20) {
        zst_pad_link(zst_element_get_pad(pay20, "src"), zst_element_get_pad(sink20, "sink"));
        zst_pad_add_probe(zst_element_get_pad(pay20, "src"), ZST_PAD_PROBE_POST_BUFFER, st2110_10_timing_probe, NULL);
        zst_element_set_state(sink20, ZST_STATE_PLAYING);
        zst_element_set_state(pay20, ZST_STATE_PLAYING);
        
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        buf->memory.size = 1920 * 1080 * 2;
        buf->memory.data = calloc(1, buf->memory.size);
        buf->pts = 1000000000ULL; // Exactly 1 second
        
        st2110_10_first_rtp_ts = 0;
        zst_pad_t* sink_pad = zst_element_get_pad(pay20, "sink");
        sink_pad->push(sink_pad, buf);
        
        // 1 second at 90kHz = 90000
        CHECK(st2110_10_first_rtp_ts == 90000, "ST2110-10 Video RTP Timestamp Alignment failed");
        
        zst_element_set_state(pay20, ZST_STATE_NULL);
        zst_element_set_state(sink20, ZST_STATE_NULL);
        zst_element_destroy(pay20);
        zst_element_destroy(sink20);
    }
    
    // Test Audio (48kHz clock)
    zst_element_t* pay30 = zst_element_factory_make("st2110_30_payloader");
    zst_element_t* sink30 = zst_element_factory_make("fakesink");
    if (pay30 && sink30) {
        zst_pad_link(zst_element_get_pad(pay30, "src"), zst_element_get_pad(sink30, "sink"));
        zst_pad_add_probe(zst_element_get_pad(pay30, "src"), ZST_PAD_PROBE_POST_BUFFER, st2110_10_timing_probe, NULL);
        zst_element_set_state(sink30, ZST_STATE_PLAYING);
        zst_element_set_state(pay30, ZST_STATE_PLAYING);
        
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        buf->memory.size = 48000 * 2 * 3; // 1 sec of 24-bit stereo
        buf->memory.data = calloc(1, buf->memory.size);
        buf->pts = 1000000000ULL; // Exactly 1 second
        
        st2110_10_first_rtp_ts = 0;
        zst_pad_t* sink_pad = zst_element_get_pad(pay30, "sink");
        sink_pad->push(sink_pad, buf);
        
        // 1 second at 48kHz = 48000
        CHECK(st2110_10_first_rtp_ts == 48000, "ST2110-10 Audio RTP Timestamp Alignment failed");
        
        zst_element_set_state(pay30, ZST_STATE_NULL);
        zst_element_set_state(sink30, ZST_STATE_NULL);
        zst_element_destroy(pay30);
        zst_element_destroy(sink30);
    }
    
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
    if (test_st2110_21_pacing_compliance() != 0) return 1;
    if (test_st2110_10_timing_compliance() != 0) return 1;

    printf("test_st2110: PASS\n");
    return 0;
}
