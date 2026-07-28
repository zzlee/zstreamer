/*=============================================================================
    test_st2110_21.c — ST2110-21 CPU Performance Test (H.264/H.265)
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_log.h"
#include "zst_clock.h"
#include <time.h>

typedef struct {
    uint64_t total_bytes;
} bit_counter_t;

static zst_pad_probe_return_t bit_counter_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data) {
    if (buf && (type & ZST_PAD_PROBE_PRE_BUFFER)) {
        bit_counter_t* counter = (bit_counter_t*)user_data;
        counter->total_bytes += buf->memory.size;
    }
    return ZST_PAD_PROBE_OK;
}

int run_test(const char* codec_enc, const char* codec_dec, int is_h265) {
    printf("=== Starting ST2110-21 Loopback Test: %s ===\n", codec_enc);
    
    zst_pipeline_t* tx = zst_pipeline_create();
    zst_pipeline_t* rx = zst_pipeline_create();

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* enc = zst_element_factory_make(codec_enc);
    zst_element_t* pay = zst_element_factory_make("st2110_21_payloader");
    zst_element_t* tx_sink = zst_element_factory_make("netsink");

    zst_element_t* rx_src = zst_element_factory_make("netsrc");
    zst_element_t* depay = zst_element_factory_make("st2110_21_depayloader");
    zst_element_t* dec = zst_element_factory_make(codec_dec);
    zst_element_t* sink = zst_element_factory_make("fakesink");

    if (!src || !enc || !pay || !tx_sink || !rx_src || !depay || !dec || !sink) {
        printf("Failed to create elements\n");
        return -1;
    }

    zst_element_set_property_int(src, "num-buffers", 300);
    zst_element_set_property_int(src, "width", 1280);
    zst_element_set_property_int(src, "height", 720);
    zst_element_set_property_int(src, "fps", 60);

    zst_element_set_property_string(pay, "codec", is_h265 ? "h265" : "h264");
    zst_element_set_property_string(tx_sink, "host", "127.0.0.1");
    zst_element_set_property_int(tx_sink, "port", 5004);
    zst_element_set_property_string(tx_sink, "protocol", "udp-client");

    zst_element_set_property_string(rx_src, "host", "127.0.0.1");
    zst_element_set_property_int(rx_src, "port", 5004);
    zst_element_set_property_string(rx_src, "protocol", "udp");
    zst_element_set_property_string(depay, "codec", is_h265 ? "h265" : "h264");

    zst_pipeline_add(tx, src);
    zst_pipeline_add(tx, enc);
    zst_pipeline_add(tx, pay);
    zst_pipeline_add(tx, tx_sink);

    zst_pipeline_add(rx, rx_src);
    zst_pipeline_add(rx, depay);
    zst_pipeline_add(rx, dec);
    zst_pipeline_add(rx, sink);

    zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(enc, "sink"));
    zst_pad_link(zst_element_get_pad(enc, "src"), zst_element_get_pad(pay, "sink"));
    zst_pad_link(zst_element_get_pad(pay, "src"), zst_element_get_pad(tx_sink, "sink"));

    zst_pad_link(zst_element_get_pad(rx_src, "src"), zst_element_get_pad(depay, "sink"));
    zst_pad_link(zst_element_get_pad(depay, "src"), zst_element_get_pad(dec, "sink"));
    zst_pad_link(zst_element_get_pad(dec, "src"), zst_element_get_pad(sink, "sink"));

    zst_scheduler_config_t cfg_rx = {.mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 2};
    zst_scheduler_t* sched_rx = zst_scheduler_create(&cfg_rx);
    zst_scheduler_attach(sched_rx, rx);
    zst_pipeline_set_state(rx, ZST_STATE_PLAYING);
    zst_scheduler_run(sched_rx);

    usleep(50000); // let RX start listening

    zst_scheduler_config_t cfg_tx = {.mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 2};
    zst_scheduler_t* sched_tx = zst_scheduler_create(&cfg_tx);
    zst_scheduler_attach(sched_tx, tx);
    
    bit_counter_t net_counter = {0};
    zst_pad_add_probe(zst_element_get_pad(tx_sink, "sink"), ZST_PAD_PROBE_PRE_BUFFER, bit_counter_probe, &net_counter);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t start = ts_start.tv_sec * 1000000ULL + ts_start.tv_nsec / 1000ULL;
    zst_pipeline_set_state(tx, ZST_STATE_PLAYING);
    zst_scheduler_run(sched_tx);

    usleep(6000000); // Wait 6 seconds for 300 frames at 60fps to process (5 seconds + 1 sec buffer)
    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t end = ts_end.tv_sec * 1000000ULL + ts_end.tv_nsec / 1000ULL;

    usleep(200000); // wait for RX to flush

    zst_scheduler_stop(sched_tx);
    zst_pipeline_set_state(tx, ZST_STATE_NULL);

    zst_scheduler_stop(sched_rx);
    zst_pipeline_set_state(rx, ZST_STATE_NULL);

    zst_scheduler_destroy(sched_tx);
    zst_scheduler_destroy(sched_rx);

    double elapsed = (double)(end - start) / 1000000.0;
    double fps = 300.0 / elapsed;
    double mbps = (net_counter.total_bytes * 8.0) / (elapsed * 1000000.0);

    printf("--- Performance Metrics (%s) ---\n", codec_enc);
    printf("Total Frames: 300\n");
    printf("Resolution  : 1280x720\n");
    printf("Wall Time   : %.3f seconds\n", elapsed);
    printf("Overall FPS : %.2f fps\n", fps);
    printf("ST2110-21 TX: %.2f Mbps\n", mbps);
    printf("=====================================================\n\n");

    zst_pipeline_destroy(tx);
    zst_pipeline_destroy(rx);
    return 0;
}

int main() {
    zst_register_builtin_elements();
    
    int err = run_test("x264enc", "h264dec", 0);
    if (err) return err;
    
    // H.265 test commented out or optional depending on ffmpeg
    err = run_test("x265enc", "h265dec", 1);
    
    return err;
}
