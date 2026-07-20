/*=============================================================================
    test_ws_server.c — Unit tests for the WebSocket signaling server
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "zst_ws_server.h"
#include "zst_log.h"

static volatile int g_connected_cid = -1;
static volatile int g_disconnected_cid = -1;
static char g_last_msg[256];
static size_t g_last_msg_len = 0;
static pthread_mutex_t g_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_test_cond = PTHREAD_COND_INITIALIZER;

static void on_connect(int client_id, void* user_data) {
    pthread_mutex_lock(&g_test_lock);
    g_connected_cid = client_id;
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_lock);
}

static void on_message(int client_id, const char* msg, size_t len, void* user_data) {
    pthread_mutex_lock(&g_test_lock);
    if (len < sizeof(g_last_msg) - 1) {
        memcpy(g_last_msg, msg, len);
        g_last_msg[len] = '\0';
        g_last_msg_len = len;
    }
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_lock);
}

static void on_disconnect(int client_id, void* user_data) {
    pthread_mutex_lock(&g_test_lock);
    g_disconnected_cid = client_id;
    pthread_cond_signal(&g_test_cond);
    pthread_mutex_unlock(&g_test_lock);
}

static int connect_to_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    zst_log_set_level(ZST_LOG_LEVEL_INFO);
    printf("Starting WebSocket signaling server unit tests...\n");
    
    int port = 9002;
    zst_ws_server_t* srv = zst_ws_server_create(port);
    assert(srv != NULL);
    
    zst_ws_server_set_callbacks(srv, on_connect, on_message, on_disconnect, NULL);
    
    zst_result_t ret = zst_ws_server_start(srv);
    assert(ret == ZST_OK);
    
    // Connect client
    int client_fd = connect_to_server(port);
    assert(client_fd >= 0);
    
    // Send RFC 6455 standard handshake request
    const char* handshake =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Origin: http://example.com\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    
    ssize_t sent = send(client_fd, handshake, strlen(handshake), 0);
    assert(sent == (ssize_t)strlen(handshake));
    
    // Read response
    char rx_buf[1024];
    ssize_t n = recv(client_fd, rx_buf, sizeof(rx_buf) - 1, 0);
    assert(n > 0);
    rx_buf[n] = '\0';
    
    // RFC 6455 response key for "dGhlIHNhbXBsZSBub25jZQ==" is "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    printf("Received handshake response:\n%s\n", rx_buf);
    assert(strstr(rx_buf, "101 Switching Protocols") != NULL);
    assert(strstr(rx_buf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    
    // Wait for on_connect callback
    pthread_mutex_lock(&g_test_lock);
    while (g_connected_cid == -1) {
        pthread_cond_wait(&g_test_cond, &g_test_lock);
    }
    int client_id = g_connected_cid;
    pthread_mutex_unlock(&g_test_lock);
    printf("Server accepted client ID: %d\n", client_id);
    
    // Send a masked text frame containing "ping"
    // FIN=1, Opcode=1, Mask=1, len=4, mask_key={0x12, 0x34, 0x56, 0x78}
    // "ping" masked:
    // 'p' (0x70) ^ 0x12 = 0x62
    // 'i' (0x69) ^ 0x34 = 0x5D
    // 'n' (0x6E) ^ 0x56 = 0x38
    // 'g' (0x67) ^ 0x78 = 0x1F
    uint8_t ping_frame[] = {
        0x81, 0x84,
        0x12, 0x34, 0x56, 0x78,
        0x62, 0x5D, 0x38, 0x1F
    };
    sent = send(client_fd, ping_frame, sizeof(ping_frame), 0);
    assert(sent == sizeof(ping_frame));
    
    // Wait for on_message callback
    pthread_mutex_lock(&g_test_lock);
    while (g_last_msg_len == 0) {
        pthread_cond_wait(&g_test_cond, &g_test_lock);
    }
    printf("Server received message: %s (len=%zu)\n", g_last_msg, g_last_msg_len);
    assert(strcmp(g_last_msg, "ping") == 0);
    pthread_mutex_unlock(&g_test_lock);
    
    // Server replies with "pong"
    ret = zst_ws_send(srv, client_id, "pong", 4);
    assert(ret == ZST_OK);
    
    // Read reply frame
    uint8_t reply_hdr[2];
    n = recv(client_fd, reply_hdr, 2, 0);
    assert(n == 2);
    assert(reply_hdr[0] == 0x81); // FIN=1, Opcode=1 (Text)
    assert((reply_hdr[1] & 0x80) == 0); // Mask=0 (Server-to-client frames must not be masked)
    assert((reply_hdr[1] & 0x7F) == 4); // len = 4
    
    char reply_payload[5];
    n = recv(client_fd, reply_payload, 4, 0);
    assert(n == 4);
    reply_payload[4] = '\0';
    printf("Client received reply payload: %s\n", reply_payload);
    assert(strcmp(reply_payload, "pong") == 0);
    
    // Close client connection
    close(client_fd);
    
    // Wait for on_disconnect callback
    pthread_mutex_lock(&g_test_lock);
    while (g_disconnected_cid == -1) {
        pthread_cond_wait(&g_test_cond, &g_test_lock);
    }
    assert(g_disconnected_cid == client_id);
    pthread_mutex_unlock(&g_test_lock);
    printf("Client %d disconnected callback fired successfully.\n", client_id);
    
    // Stop and free server
    zst_ws_server_free(srv);
    
    printf("WebSocket signaling server tests passed successfully!\n");
    return 0;
}
