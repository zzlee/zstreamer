/*=============================================================================
    test_net_sink.c — Comprehensive test suite for net_sink element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_pipeline.h"

/* Forward declarations */
extern zst_element_t* zst_net_sink_create(void);
extern zst_element_t* zst_net_source_create(void);

static void
sleep_ms(unsigned int ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000UL)
    };
    nanosleep(&ts, NULL);
}

/* TCP client test: connect to a listening socket and verify data reception */
static void*
tcp_server_thread(void* arg)
{
    uint16_t port = *(uint16_t*)arg;
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    assert(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) >= 0);
    assert(listen(listen_fd, 1) >= 0);
    
    struct sockaddr_in client_addr = {0};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    assert(client_fd >= 0);
    
    /* Receive data from net_sink */
    char buffer[256];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
    printf("tcp_server received %zd bytes\n", n);
    assert(n > 0);
    assert(memcmp(buffer, "test_data", 9) == 0);
    
    close(client_fd);
    close(listen_fd);
    return NULL;
}

static void
test_net_sink_tcp_client(void)
{
    printf("\n=== Test: net_sink TCP client mode ===\n");
    
    zst_element_t* sink = zst_net_sink_create();
    assert(sink != NULL);
    assert(strcmp(sink->ops->name, "netsink") == 0);
    
    uint16_t port = 15000;
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, tcp_server_thread, &port);
    
    /* Give server time to bind */
    sleep_ms(100);
    
    /* Configure sink for TCP client */
    assert(zst_element_set_property(sink, "protocol", "tcp-client") == ZST_OK);
    assert(zst_element_set_property(sink, "host", "127.0.0.1") == ZST_OK);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    assert(zst_element_set_property(sink, "port", port_str) == ZST_OK);
    
    /* Open sink */
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);
    
    /* Create and send a buffer */
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    assert(buf != NULL);
    
    const char* test_data = "test_data";
    buf->memory.data = (void*)test_data;
    buf->memory.size = strlen(test_data);
    buf->pts = 0;
    buf->duration = 0;
    
    /* Process the buffer through the sink */
    zst_buffer_t* out = NULL;
    assert(sink->ops->process(sink, buf, &out) == ZST_OK);
    
    zst_buffer_unref(buf);
    
    /* Close sink */
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    
    pthread_join(server_tid, NULL);
    zst_element_destroy(sink);
    printf("✓ TCP client test passed\n");
}

static void
test_net_sink_properties(void)
{
    printf("\n=== Test: net_sink property get/set ===\n");
    
    zst_element_t* sink = zst_net_sink_create();
    assert(sink != NULL);
    
    char value[128];
    
    /* Test protocol property */
    assert(zst_element_get_property(sink, "protocol", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "tcp-client") == 0);
    
    assert(zst_element_set_property(sink, "protocol", "tcp-server") == ZST_OK);
    assert(zst_element_get_property(sink, "protocol", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "tcp-server") == 0);
    
    assert(zst_element_set_property(sink, "protocol", "unix-client") == ZST_OK);
    assert(zst_element_get_property(sink, "protocol", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "unix-client") == 0);
    
    /* Test host property */
    assert(zst_element_set_property(sink, "host", "192.168.1.1") == ZST_OK);
    assert(zst_element_get_property(sink, "host", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "192.168.1.1") == 0);
    
    /* Test port property */
    assert(zst_element_set_property(sink, "port", "8080") == ZST_OK);
    assert(zst_element_get_property(sink, "port", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "8080") == 0);
    
    /* Test path property */
    assert(zst_element_set_property(sink, "path", "/tmp/test.sock") == ZST_OK);
    assert(zst_element_get_property(sink, "path", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "/tmp/test.sock") == 0);
    
    /* Test write-timeout property */
    assert(zst_element_set_property(sink, "write-timeout", "500") == ZST_OK);
    assert(zst_element_get_property(sink, "write-timeout", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "500") == 0);

    /* Test UDP timestamp pacing properties */
    assert(zst_element_get_property(sink, "timestamp-pacing", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "false") == 0);
    assert(zst_element_set_property(sink, "timestamp-pacing", "true") == ZST_OK);
    assert(zst_element_get_property(sink, "timestamp-pacing", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "true") == 0);

    assert(zst_element_set_property(sink, "pacing-tolerance-ms", "7") == ZST_OK);
    assert(zst_element_get_property(sink, "pacing-tolerance-ms", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "7") == 0);

    assert(zst_element_set_property(sink, "pacing-reset-threshold-ms", "3000") == ZST_OK);
    assert(zst_element_get_property(sink, "pacing-reset-threshold-ms", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "3000") == 0);

    assert(zst_element_set_property(sink, "max-lateness-ms", "25") == ZST_OK);
    assert(zst_element_get_property(sink, "max-lateness-ms", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "25") == 0);
    
    zst_element_destroy(sink);
    printf("✓ Property test passed\n");
}

static void
test_net_sink_caps(void)
{
    printf("\n=== Test: net_sink caps negotiation ===\n");
    
    zst_element_t* sink = zst_net_sink_create();
    assert(sink != NULL);
    
    /* Get sink pad */
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    assert(sink_pad != NULL);
    
    zst_caps_t* caps = sink->ops->get_caps(sink, sink_pad, NULL);
    assert(caps == NULL);
    
    zst_element_destroy(sink);
    printf("✓ Caps negotiation test passed\n");
}

static void
test_net_sink_state_transitions(void)
{
    printf("\n=== Test: net_sink state transitions ===\n");
    
    zst_element_t* sink = zst_net_sink_create();
    assert(sink != NULL);
    
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
test_net_udp_push(void)
{
    printf("\n=== Test: net_sink/net_source UDP push mode ===\n");

    zst_element_t* src = zst_net_source_create();
    zst_element_t* sink = zst_net_sink_create();
    assert(src != NULL && sink != NULL);

    assert(zst_element_set_property(src, "protocol", "udp") == ZST_OK);
    assert(zst_element_set_property(src, "port", "16001") == ZST_OK);
    assert(zst_element_set_property(src, "read-timeout", "500") == ZST_OK);

    assert(zst_element_set_property(sink, "protocol", "udp-client") == ZST_OK);
    assert(zst_element_set_property(sink, "host", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(sink, "port", "16001") == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);

    /* Create input buffer */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_USER);
    assert(in_buf != NULL);
    const char* payload = "udp_push_test_data";
    in_buf->memory.data = (void*)payload;
    in_buf->memory.size = strlen(payload);

    /* Send buffer through sink */
    zst_buffer_t* temp = NULL;
    assert(sink->ops->process(sink, in_buf, &temp) == ZST_OK);

    /* Receive buffer from src */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = ZST_TIMEOUT;
    for (int i = 0; i < 5 && res == ZST_TIMEOUT; i++) {
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

    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);

    zst_element_destroy(sink);
    zst_element_destroy(src);
    printf("✓ UDP push test passed\n");
}

static void
test_net_udp_pull(void)
{
    printf("\n=== Test: net_sink/net_source UDP pull mode ===\n");

    zst_element_t* src = zst_net_source_create();
    zst_element_t* sink = zst_net_sink_create();
    assert(src != NULL && sink != NULL);

    assert(zst_element_set_property(sink, "protocol", "udp-server") == ZST_OK);
    assert(zst_element_set_property(sink, "port", "16002") == ZST_OK);

    assert(zst_element_set_property(src, "protocol", "udp-client") == ZST_OK);
    assert(zst_element_set_property(src, "host", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(src, "port", "16002") == ZST_OK);
    assert(zst_element_set_property(src, "read-timeout", "500") == ZST_OK);

    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);

    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    /* 1. Call src process to connect and send the hole-punch/registration packet */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = src->ops->process(src, NULL, &out_buf);
    /* It should return ZST_TIMEOUT as no media has been pushed yet */
    assert(res == ZST_TIMEOUT);
    assert(out_buf == NULL);

    /* Give OS loopback a tiny moment */
    sleep_ms(10);

    /* 2. Push media through sink. It will read registration packet, register client, and send media */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_USER);
    assert(in_buf != NULL);
    const char* payload = "udp_pull_test_data";
    in_buf->memory.data = (void*)payload;
    in_buf->memory.size = strlen(payload);

    zst_buffer_t* temp = NULL;
    assert(sink->ops->process(sink, in_buf, &temp) == ZST_OK);

    /* 3. Receive buffer from src */
    res = ZST_TIMEOUT;
    for (int i = 0; i < 5 && res == ZST_TIMEOUT; i++) {
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

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);

    zst_element_destroy(src);
    zst_element_destroy(sink);
    printf("✓ UDP pull test passed\n");
}

static uint64_t
get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void
test_net_udp_pacing(void)
{
    printf("\n=== Test: net_sink/net_source UDP pacing ===\n");

    zst_element_t* src = zst_net_source_create();
    zst_element_t* sink = zst_net_sink_create();
    assert(src != NULL && sink != NULL);

    assert(zst_element_set_property(src, "protocol", "udp") == ZST_OK);
    assert(zst_element_set_property(src, "port", "16005") == ZST_OK);
    assert(zst_element_set_property(src, "read-timeout", "500") == ZST_OK);

    assert(zst_element_set_property(sink, "protocol", "udp-client") == ZST_OK);
    assert(zst_element_set_property(sink, "host", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(sink, "port", "16005") == ZST_OK);
    assert(zst_element_set_property(sink, "timestamp-pacing", "true") == ZST_OK);
    assert(zst_element_set_property(sink, "pacing-tolerance-ms", "2") == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);

    uint64_t recv_times[4];

    for (int i = 0; i < 4; i++) {
        zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_USER);
        assert(in_buf != NULL);
        char payload[32];
        snprintf(payload, sizeof(payload), "pacing_data_%d", i);
        in_buf->memory.data = (void*)payload;
        in_buf->memory.size = strlen(payload);
        in_buf->pts = i * 20ULL * 1000000ULL; // 0, 20ms, 40ms, 60ms

        zst_buffer_t* temp = NULL;
        assert(sink->ops->process(sink, in_buf, &temp) == ZST_OK);

        /* Receive buffer from src */
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = ZST_TIMEOUT;
        for (int k = 0; k < 10 && res == ZST_TIMEOUT; k++) {
            res = src->ops->process(src, NULL, &out_buf);
            if (res == ZST_TIMEOUT) {
                sleep_ms(5);
            }
        }
        recv_times[i] = get_time_ms();
        assert(res == ZST_OK);
        assert(out_buf != NULL);

        zst_buffer_unref(in_buf);
        zst_buffer_unref(out_buf);
    }

    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);

    zst_element_destroy(sink);
    zst_element_destroy(src);

    uint64_t diff1 = recv_times[1] - recv_times[0];
    uint64_t diff2 = recv_times[2] - recv_times[1];
    uint64_t diff3 = recv_times[3] - recv_times[2];

    printf("  UDP pacing recv times diff: %d ms, %d ms, %d ms\n", (int)diff1, (int)diff2, (int)diff3);
    // Since pacing interval is 20ms, each diff should be >= 12 ms.
    assert(diff1 >= 12);
    assert(diff2 >= 12);
    assert(diff3 >= 12);

    printf("✓ UDP pacing test passed\n");
}

static void
test_net_udp_no_pacing(void)
{
    printf("\n=== Test: net_sink/net_source UDP no pacing ===\n");

    zst_element_t* src = zst_net_source_create();
    zst_element_t* sink = zst_net_sink_create();
    assert(src != NULL && sink != NULL);

    assert(zst_element_set_property(src, "protocol", "udp") == ZST_OK);
    assert(zst_element_set_property(src, "port", "16006") == ZST_OK);
    assert(zst_element_set_property(src, "read-timeout", "500") == ZST_OK);

    assert(zst_element_set_property(sink, "protocol", "udp-client") == ZST_OK);
    assert(zst_element_set_property(sink, "host", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(sink, "port", "16006") == ZST_OK);
    assert(zst_element_set_property(sink, "timestamp-pacing", "false") == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);

    uint64_t recv_times[4];

    for (int i = 0; i < 4; i++) {
        zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_USER);
        assert(in_buf != NULL);
        char payload[32];
        snprintf(payload, sizeof(payload), "pacing_data_%d", i);
        in_buf->memory.data = (void*)payload;
        in_buf->memory.size = strlen(payload);
        in_buf->pts = i * 20ULL * 1000000ULL; // 0, 20ms, 40ms, 60ms

        zst_buffer_t* temp = NULL;
        assert(sink->ops->process(sink, in_buf, &temp) == ZST_OK);

        /* Receive buffer from src */
        zst_buffer_t* out_buf = NULL;
        zst_result_t res = ZST_TIMEOUT;
        for (int k = 0; k < 10 && res == ZST_TIMEOUT; k++) {
            res = src->ops->process(src, NULL, &out_buf);
            if (res == ZST_TIMEOUT) {
                sleep_ms(5);
            }
        }
        recv_times[i] = get_time_ms();
        assert(res == ZST_OK);
        assert(out_buf != NULL);

        zst_buffer_unref(in_buf);
        zst_buffer_unref(out_buf);
    }

    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);

    zst_element_destroy(sink);
    zst_element_destroy(src);

    uint64_t total_duration = recv_times[3] - recv_times[0];

    printf("  UDP no pacing total duration for 4 buffers: %d ms\n", (int)total_duration);
    // Without pacing, they should arrive in a burst, total duration should be very small (typically < 15 ms).
    assert(total_duration < 15);

    printf("✓ UDP no pacing test passed\n");
}

int main(void)
{
    printf("=== net_sink comprehensive test suite ===\n");

    test_net_sink_properties();
    test_net_sink_caps();
    test_net_sink_state_transitions();
    test_net_sink_tcp_client();
    test_net_udp_push();
    test_net_udp_pull();
    test_net_udp_pacing();
    test_net_udp_no_pacing();

    printf("\n✓✓✓ All net_sink tests passed ✓✓✓\n");
    return 0;
}
