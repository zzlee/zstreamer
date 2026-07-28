/*=============================================================================
    test_st2110_redundancy.c — ST2110 Redundancy Phase 3 tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static int
test_st2110_redundancy_mux_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_redundancy_mux");
    if (!el) {
        fprintf(stderr, "SKIP: st2110_redundancy_mux not registered\n");
        return 0;
    }

    CHECK(zst_element_set_property(el, "fec-enabled", "true") == ZST_OK, "set fec-enabled failed");
    CHECK(zst_element_set_property(el, "primary-addr", "239.0.0.1") == ZST_OK, "set primary-addr failed");

    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_redundancy_demux_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_redundancy_demux");
    if (!el) return 0;

    CHECK(zst_element_set_property(el, "failover-detection-ms", "200") == ZST_OK, "set failover-detection-ms failed");
    CHECK(zst_element_set_property(el, "recovery-detection-ms", "1000") == ZST_OK, "set recovery-detection-ms failed");

    zst_element_destroy(el);
    return 0;
}

static uint16_t expected_sps_seq = 0;
static int sps_packets_received = 0;
static int sps_test_failed = 0;

static zst_pad_probe_return_t
st2022_7_probe(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    if (buf && buf->memory.size >= 12) {
        uint8_t* data = (uint8_t*)buf->memory.data;
        uint16_t seq = (data[2] << 8) | data[3];
        if (seq != expected_sps_seq) {
            fprintf(stderr, "ST2022-7 Fail: Expected seq %d, got %d\n", expected_sps_seq, seq);
            sps_test_failed = 1;
        }
        expected_sps_seq++;
        sps_packets_received++;
    }
    return ZST_PAD_PROBE_OK;
}

static zst_buffer_t*
create_dummy_rtp_packet(uint16_t seq)
{
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    buf->memory.size = 1200;
    buf->memory.data = calloc(1, buf->memory.size);
    uint8_t* data = (uint8_t*)buf->memory.data;
    data[0] = 0x80;
    data[1] = 96;
    data[2] = seq >> 8;
    data[3] = seq & 0xFF;
    return buf;
}

static int
test_st2022_7_seamless_switching(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_redundancy_demux");
    zst_element_t* sink = zst_element_factory_make("fakesink");
    if (!el || !sink) return 0;
    
    zst_pad_link(zst_element_get_pad(el, "src"), zst_element_get_pad(sink, "sink"));
    zst_pad_add_probe(zst_element_get_pad(el, "src"), ZST_PAD_PROBE_POST_BUFFER, st2022_7_probe, NULL);
    
    zst_element_set_state(sink, ZST_STATE_PLAYING);
    zst_element_set_state(el, ZST_STATE_PLAYING);
    
    zst_pad_t* pad_a = zst_element_get_pad(el, "sink_primary");
    zst_pad_t* pad_b = zst_element_get_pad(el, "sink_backup");
    
    if (!pad_a || !pad_b) {
        fprintf(stderr, "Failed to get redundancy pads\n");
        return 1;
    }
    
    expected_sps_seq = 1000;
    sps_packets_received = 0;
    sps_test_failed = 0;
    
    // Send seq 1000 to both
    pad_a->push(pad_a, create_dummy_rtp_packet(1000));
    pad_b->push(pad_b, create_dummy_rtp_packet(1000));
    
    // Seq 1001 only to A (B drops)
    pad_a->push(pad_a, create_dummy_rtp_packet(1001));
    
    // Seq 1002 only to B (A drops)
    pad_b->push(pad_b, create_dummy_rtp_packet(1002));
    
    // Seq 1003 to 1010 on A, 1005 to 1015 on B (A drops 1011-1015, B drops 1003-1004)
    for (int i = 1003; i <= 1010; i++) pad_a->push(pad_a, create_dummy_rtp_packet(i));
    for (int i = 1005; i <= 1015; i++) pad_b->push(pad_b, create_dummy_rtp_packet(i));
    
    // Wait out of order packets (skewed arrival)
    // A delivers 1016, 1017
    // B delivers 1016, 1017, but later
    pad_a->push(pad_a, create_dummy_rtp_packet(1016));
    pad_a->push(pad_a, create_dummy_rtp_packet(1017));
    pad_b->push(pad_b, create_dummy_rtp_packet(1016));
    pad_b->push(pad_b, create_dummy_rtp_packet(1017));
    
    CHECK(sps_test_failed == 0, "ST2022-7 Seamless Switching failed (corrupted sequence)");
    CHECK(sps_packets_received == 18, "ST2022-7 Seamless Switching failed (packet count mismatch)");
    
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_element_set_state(sink, ZST_STATE_NULL);
    zst_element_destroy(el);
    zst_element_destroy(sink);
    return 0;
}

int main(void)
{
    zst_register_builtin_elements();

    if (test_st2110_redundancy_mux_basic() != 0) return 1;
    if (test_st2110_redundancy_demux_basic() != 0) return 1;
    if (test_st2022_7_seamless_switching() != 0) return 1;

    printf("test_st2110_redundancy: PASS\n");
    return 0;
}
