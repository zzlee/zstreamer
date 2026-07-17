/*=============================================================================
    test_srt_sink.c — Comprehensive test suite for srt_sink element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_pipeline.h"

/* Forward declarations */
extern zst_element_t* zst_srt_sink_create(void);
extern zst_element_t* zst_srt_source_create(void);

static void
sleep_ms(unsigned int ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000UL)
    };
    nanosleep(&ts, NULL);
}

static void
test_srt_sink_properties(void)
{
    printf("\n=== Test: srt_sink property get/set ===\n");

    zst_element_t* sink = zst_srt_sink_create();
    assert(sink != NULL);

    char value[128];

    /* Test uri property */
    assert(zst_element_set_property(sink, "uri", "srt://192.168.1.1:9001?mode=listener&latency=200&passphrase=dummyphrase&pbkeylen=32") == ZST_OK);
    assert(zst_element_get_property(sink, "uri", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "srt://192.168.1.1:9001?mode=listener&latency=200&passphrase=dummyphrase&pbkeylen=32") == 0);

    /* URI parsing should have populated host, port, mode, latency, password, pbkeylen */
    assert(zst_element_get_property(sink, "host", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "192.168.1.1") == 0);

    assert(zst_element_get_property(sink, "port", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "9001") == 0);

    assert(zst_element_get_property(sink, "mode", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "listener") == 0);

    assert(zst_element_get_property(sink, "latency", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "200") == 0);

    assert(zst_element_get_property(sink, "passphrase", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "dummyphrase") == 0);

    assert(zst_element_get_property(sink, "pbkeylen", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "32") == 0);

    /* Test manual property overrides */
    assert(zst_element_set_property(sink, "host", "127.0.0.1") == ZST_OK);
    assert(zst_element_get_property(sink, "host", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "127.0.0.1") == 0);

    assert(zst_element_set_property(sink, "port", "9002") == ZST_OK);
    assert(zst_element_get_property(sink, "port", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "9002") == 0);

    assert(zst_element_set_property(sink, "mode", "caller") == ZST_OK);
    assert(zst_element_get_property(sink, "mode", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "caller") == 0);

    assert(zst_element_set_property(sink, "streamid", "mystream") == ZST_OK);
    assert(zst_element_get_property(sink, "streamid", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "mystream") == 0);

    assert(zst_element_set_property(sink, "payload-size", "1400") == ZST_OK);
    assert(zst_element_get_property(sink, "payload-size", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "1400") == 0);

    zst_element_destroy(sink);
    printf("✓ Property test passed\n");
}

static void
test_srt_sink_caps(void)
{
    printf("\n=== Test: srt_sink caps negotiation ===\n");

    zst_element_t* sink = zst_srt_sink_create();
    assert(sink != NULL);

    /* Get sink pad */
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    assert(sink_pad != NULL);

    /* Get caps */
    zst_caps_t* caps = sink->ops->get_caps(sink, sink_pad, NULL);
    assert(caps != NULL);

    /* Verify caps structure */
    if (caps->structs) {
        zst_caps_struct_t* cap_struct = caps->structs;
        assert(strcmp(cap_struct->media_type, "video/mp2t") == 0);

        cap_struct = cap_struct->next;
        assert(cap_struct != NULL);
        assert(strcmp(cap_struct->media_type, "application/octet-stream") == 0);
    }

    zst_caps_destroy(caps);
    zst_element_destroy(sink);
    printf("✓ Caps negotiation test passed\n");
}

static void
test_srt_sink_state_transitions(void)
{
    printf("\n=== Test: srt_sink state transitions ===\n");

    zst_element_t* sink = zst_srt_sink_create();
    assert(sink != NULL);

    /* Use a random port to avoid conflicts */
    assert(zst_element_set_property(sink, "uri", "srt://127.0.0.1:9091?mode=listener") == ZST_OK);

    /* Verify initial state */
    assert(sink->state == ZST_STATE_NULL);

    /* NULL -> READY */
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);
    assert(sink->state == ZST_STATE_READY);

    /* READY -> PLAYING */
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    assert(sink->state == ZST_STATE_PLAYING);

    /* PLAYING -> NULL */
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    assert(sink->state == ZST_STATE_NULL);

    zst_element_destroy(sink);
    printf("✓ State transition test passed\n");
}

static void
test_srt_loopback(void)
{
    printf("\n=== Test: srt_sink -> srt_source loopback ===\n");

    zst_element_t* sink = zst_srt_sink_create();
    zst_element_t* src = zst_srt_source_create();
    assert(sink != NULL && src != NULL);

    /* Configure sink as listener */
    assert(zst_element_set_property(sink, "uri", "srt://127.0.0.1:9192?mode=listener") == ZST_OK);

    /* Configure source as caller */
    assert(zst_element_set_property(src, "uri", "srt://127.0.0.1:9192?mode=caller") == ZST_OK);

    /* Transition to READY */
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);
    /* Small delay to let listener bind */
    sleep_ms(100);
    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);

    /* Transition to PLAYING */
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    /* In caller/listener loopback, caller will initiate the connection */
    /* Give it time to connect */
    zst_result_t res = ZST_TIMEOUT;
    for (int i = 0; i < 20 && res == ZST_TIMEOUT; i++) {
        zst_buffer_t* out_buf = NULL;
        res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_TIMEOUT) {
            sleep_ms(50);
        } else if (res == ZST_OK) {
             /* Received unexpected data early? */
             if (out_buf) zst_buffer_unref(out_buf);
        }
    }

    /* Create input buffer */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_USER);
    assert(in_buf != NULL);
    const char* payload = "srt_loopback_test_data";
    in_buf->memory.data = (void*)payload;
    in_buf->memory.size = strlen(payload);

    /* Send buffer through sink */
    /* Due to SRT connection setup, we might need a few attempts if TIMEOUT occurs */
    res = ZST_TIMEOUT;
    for (int i = 0; i < 20 && res == ZST_TIMEOUT; i++) {
        zst_buffer_t* temp = NULL;
        res = sink->ops->process(sink, in_buf, &temp);
        if (res == ZST_TIMEOUT) {
            sleep_ms(50);
        }
    }
    assert(res == ZST_OK);

    /* Receive buffer from src */
    zst_buffer_t* out_buf = NULL;
    res = ZST_TIMEOUT;
    for (int i = 0; i < 20 && res == ZST_TIMEOUT; i++) {
        res = src->ops->process(src, NULL, &out_buf);
        if (res == ZST_TIMEOUT) {
            sleep_ms(50);
        }
    }
    assert(res == ZST_OK);
    assert(out_buf != NULL);
    assert(out_buf->memory.size == strlen(payload));
    assert(memcmp(out_buf->memory.data, payload, strlen(payload)) == 0);

    zst_buffer_unref(in_buf);
    zst_buffer_unref(out_buf);

    /* Stop elements */
    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);

    zst_element_destroy(src);
    zst_element_destroy(sink);
    printf("✓ SRT loopback test passed\n");
}

int main(void)
{
    printf("=== srt_sink comprehensive test suite ===\n");

    test_srt_sink_properties();
    test_srt_sink_caps();
    test_srt_sink_state_transitions();
    test_srt_loopback();

    printf("\n✓✓✓ All srt_sink tests passed ✓✓✓\n");
    return 0;
}
