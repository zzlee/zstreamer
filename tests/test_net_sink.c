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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_pipeline.h"

/* Forward declaration from net_sink.c if linkable directly */
extern zst_element_t* zst_net_sink_create(void);

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
    usleep(100000);
    
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
    buf->memory.data = malloc(strlen(test_data));
    memcpy(buf->memory.data, test_data, strlen(test_data));
    buf->memory.size = strlen(test_data);
    buf->memory.release = free;
    buf->memory.priv = buf->memory.data;
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
    
    /* Get caps */
    zst_caps_t* caps = sink->ops->get_caps(sink, sink_pad, NULL);
    assert(caps != NULL);
    
    /* Verify caps structure */
    if (caps->structs) {
        zst_caps_struct_t* cap_struct = caps->structs;
        assert(strcmp(cap_struct->media_type, "application/octet-stream") == 0);
        printf("  Media type: %s\n", cap_struct->media_type);
    }
    
    zst_caps_destroy(caps);
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

int main(void)
{
    printf("=== net_sink comprehensive test suite ===\n");
    
    test_net_sink_properties();
    test_net_sink_caps();
    test_net_sink_state_transitions();
    test_net_sink_tcp_client();
    
    printf("\n✓✓✓ All net_sink tests passed ✓✓✓\n");
    return 0;
}
