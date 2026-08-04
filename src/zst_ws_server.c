/*=============================================================================
    zst_ws_server.c — Lightweight WebSocket signaling server implementation
=============================================================================*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>

#include "zst_ws_server.h"
#include "zstreamer/elements/zst_ws_server.h"
#include "zst_log.h"
#include "zst_plugin.h"

#define MAX_CLIENTS 32
#define INITIAL_RX_CAP 4096

/*──────────────────────────────────────────────────────────────────────────
  SHA-1 Implementation (FIPS 180-1)
──────────────────────────────────────────────────────────────────────────*/
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        uint32_t val = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (val << 1) | (val >> 31);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buffer[64];
    size_t i = 0;
    size_t bits = len * 8;
    
    while (len - i >= 64) {
        memcpy(buffer, data + i, 64);
        sha1_transform(state, buffer);
        i += 64;
    }
    
    uint8_t pad[64];
    memset(pad, 0, 64);
    size_t rem = len - i;
    memcpy(pad, data + i, rem);
    pad[rem] = 0x80;
    
    if (rem >= 56) {
        sha1_transform(state, pad);
        memset(pad, 0, 64);
    }
    
    pad[56] = (bits >> 56) & 0xFF;
    pad[57] = (bits >> 48) & 0xFF;
    pad[58] = (bits >> 40) & 0xFF;
    pad[59] = (bits >> 32) & 0xFF;
    pad[60] = (bits >> 24) & 0xFF;
    pad[61] = (bits >> 16) & 0xFF;
    pad[62] = (bits >> 8) & 0xFF;
    pad[63] = bits & 0xFF;
    
    sha1_transform(state, pad);
    
    for (int j = 0; j < 5; j++) {
        digest[j * 4]     = (state[j] >> 24) & 0xFF;
        digest[j * 4 + 1] = (state[j] >> 16) & 0xFF;
        digest[j * 4 + 2] = (state[j] >> 8) & 0xFF;
        digest[j * 4 + 3] = state[j] & 0xFF;
    }
}

/*──────────────────────────────────────────────────────────────────────────
  Base64 Implementation
──────────────────────────────────────────────────────────────────────────*/
static void base64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < in_len && j + 4 < out_cap) {
        uint32_t val = in[i++] << 16;
        int count = 1;
        if (i < in_len) {
            val |= in[i++] << 8;
            count++;
        }
        if (i < in_len) {
            val |= in[i++];
            count++;
        }
        out[j++] = b64[(val >> 18) & 0x3F];
        out[j++] = b64[(val >> 12) & 0x3F];
        out[j++] = (count >= 2) ? b64[(val >> 6) & 0x3F] : '=';
        out[j++] = (count == 3) ? b64[val & 0x3F] : '=';
    }
    out[j] = '\0';
}

/*──────────────────────────────────────────────────────────────────────────
  WebSocket Server Types & Implementation
──────────────────────────────────────────────────────────────────────────*/
typedef struct {
    int id;
    int fd;
    bool handshaked;
    uint8_t* rx_buf;
    size_t rx_len;
    size_t rx_cap;
} ws_client_t;

struct zst_ws_server_s {
    int port;
    int listen_fd;
    pthread_t thread;
    volatile bool running;
    pthread_mutex_t clients_lock;
    ws_client_t clients[MAX_CLIENTS];
    int next_client_id;
    
    void (*on_connect)(int client_id, void* user_data);
    void (*on_message)(int client_id, const char* msg, size_t len, void* user_data);
    void (*on_disconnect)(int client_id, void* user_data);
    void* cb_user_data;
};

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void disconnect_client_unlocked(zst_ws_server_t* srv, int idx) {
    if (srv->clients[idx].fd >= 0) {
        int fd = srv->clients[idx].fd;
        int client_id = srv->clients[idx].id;
        srv->clients[idx].fd = -1;
        srv->clients[idx].id = -1;
        srv->clients[idx].handshaked = false;
        free(srv->clients[idx].rx_buf);
        srv->clients[idx].rx_buf = NULL;
        srv->clients[idx].rx_len = 0;
        srv->clients[idx].rx_cap = 0;
        close(fd);
        ZST_LOG_INFO("ws_server", "Client %d disconnected (fd %d)", client_id, fd);
        
        if (srv->on_disconnect) {
            void (*on_disconnect_cb)(int, void*) = srv->on_disconnect;
            void* user_data = srv->cb_user_data;
            pthread_mutex_unlock(&srv->clients_lock);
            on_disconnect_cb(client_id, user_data);
            pthread_mutex_lock(&srv->clients_lock);
        }
    }
}

static void process_client_rx(zst_ws_server_t* srv, int idx) {
    ws_client_t* c = &srv->clients[idx];
    
    while (c->fd >= 0) {
        if (!c->handshaked) {
            c->rx_buf[c->rx_len] = '\0';
            char* end = strstr((char*)c->rx_buf, "\r\n\r\n");
            if (!end) {
                if (c->rx_len >= c->rx_cap - 1) {
                    disconnect_client_unlocked(srv, idx);
                }
                return;
            }
            
            size_t header_bytes = (uint8_t*)end - c->rx_buf + 4;
            
            char* key_start = strstr((char*)c->rx_buf, "Sec-WebSocket-Key:");
            if (!key_start) {
                disconnect_client_unlocked(srv, idx);
                return;
            }
            key_start += 18;
            while (*key_start == ' ' || *key_start == '\t') key_start++;
            char* key_end = key_start;
            while (*key_end && *key_end != '\r' && *key_end != '\n') key_end++;
            
            char key[128];
            size_t key_len = key_end - key_start;
            if (key_len >= sizeof(key)) {
                disconnect_client_unlocked(srv, idx);
                return;
            }
            memcpy(key, key_start, key_len);
            key[key_len] = '\0';
            
            char concat[256];
            snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
            uint8_t digest[20];
            sha1((uint8_t*)concat, strlen(concat), digest);
            char accept_b64[64];
            base64_encode(digest, 20, accept_b64, sizeof(accept_b64));
            
            char resp[512];
            int resp_len = snprintf(resp, sizeof(resp),
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n\r\n",
                accept_b64);
            
            send(c->fd, resp, resp_len, 0);
            c->handshaked = true;
            ZST_LOG_INFO("ws_server", "Client %d handshaked (fd %d)", c->id, c->fd);
            
            memmove(c->rx_buf, c->rx_buf + header_bytes, c->rx_len - header_bytes);
            c->rx_len -= header_bytes;
            
            if (srv->on_connect) {
                void (*on_connect_cb)(int, void*) = srv->on_connect;
                int cid = c->id;
                void* user_data = srv->cb_user_data;
                pthread_mutex_unlock(&srv->clients_lock);
                on_connect_cb(cid, user_data);
                pthread_mutex_lock(&srv->clients_lock);
            }
        } else {
            if (c->rx_len < 2) return;
            
            uint8_t opcode = c->rx_buf[0] & 0x0F;
            bool masked = (c->rx_buf[1] & 0x80) != 0;
            uint8_t len7 = c->rx_buf[1] & 0x7F;
            
            size_t header_len = 2;
            uint64_t payload_len = 0;
            
            if (len7 <= 125) {
                payload_len = len7;
            } else if (len7 == 126) {
                if (c->rx_len < 4) return;
                payload_len = ((uint64_t)c->rx_buf[2] << 8) | c->rx_buf[3];
                header_len = 4;
            } else if (len7 == 127) {
                if (c->rx_len < 10) return;
                payload_len = 0;
                for (int k = 0; k < 8; k++) {
                    payload_len = (payload_len << 8) | c->rx_buf[2 + k];
                }
                header_len = 10;
            }
            
            size_t total_required = header_len + (masked ? 4 : 0) + payload_len;
            if (c->rx_len < total_required) return;
            
            uint8_t mask_key[4] = {0};
            if (masked) {
                memcpy(mask_key, c->rx_buf + header_len, 4);
                header_len += 4;
            }
            
            uint8_t* payload = c->rx_buf + header_len;
            if (masked) {
                for (uint64_t i = 0; i < payload_len; i++) {
                    payload[i] ^= mask_key[i % 4];
                }
            }
            
            if (opcode == 0x8) {
                disconnect_client_unlocked(srv, idx);
                return;
            } else if (opcode == 0x9) {
                uint8_t pong_hdr[2];
                pong_hdr[0] = 0x8A;
                pong_hdr[1] = 0;
                send(c->fd, pong_hdr, 2, 0);
            } else if (opcode == 0x1) {
                if (srv->on_message) {
                    uint8_t saved = payload[payload_len];
                    payload[payload_len] = '\0';
                    
                    void (*on_message_cb)(int, const char*, size_t, void*) = srv->on_message;
                    int cid = c->id;
                    void* user_data = srv->cb_user_data;
                    
                    pthread_mutex_unlock(&srv->clients_lock);
                    on_message_cb(cid, (const char*)payload, (size_t)payload_len, user_data);
                    pthread_mutex_lock(&srv->clients_lock);
                    
                    payload[payload_len] = saved;
                }
            }
            
            memmove(c->rx_buf, c->rx_buf + total_required, c->rx_len - total_required);
            c->rx_len -= total_required;
        }
    }
}

static void* ws_server_thread(void* arg) {
    zst_ws_server_t* srv = arg;
    ZST_LOG_INFO("ws_server", "Thread started");
    
    struct pollfd fds[MAX_CLIENTS + 1];
    
    while (srv->running) {
        pthread_mutex_lock(&srv->clients_lock);
        
        int count = 0;
        fds[count].fd = srv->listen_fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        count++;
        
        int client_map[MAX_CLIENTS];
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (srv->clients[i].fd >= 0) {
                fds[count].fd = srv->clients[i].fd;
                fds[count].events = POLLIN;
                fds[count].revents = 0;
                client_map[count] = i;
                count++;
            }
        }
        
        pthread_mutex_unlock(&srv->clients_lock);
        
        int ret = poll(fds, count, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ZST_LOG_ERROR("ws_server", "poll failed: %s", strerror(errno));
            break;
        }
        if (ret == 0) continue;
        
        pthread_mutex_lock(&srv->clients_lock);
        
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int client_fd = accept(srv->listen_fd, (struct sockaddr*)&addr, &addr_len);
            if (client_fd >= 0) {
                set_nonblocking(client_fd);
                
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (srv->clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                
                if (slot >= 0) {
                    srv->clients[slot].id = srv->next_client_id++;
                    srv->clients[slot].fd = client_fd;
                    srv->clients[slot].handshaked = false;
                    srv->clients[slot].rx_cap = INITIAL_RX_CAP;
                    srv->clients[slot].rx_buf = malloc(INITIAL_RX_CAP + 1);
                    srv->clients[slot].rx_len = 0;
                    
                    ZST_LOG_INFO("ws_server", "Accepted client %d (fd %d) from %s:%hu",
                        srv->clients[slot].id, client_fd,
                        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                } else {
                    ZST_LOG_WARN("ws_server", "Max clients reached, dropping connection");
                    close(client_fd);
                }
            }
        }
        
        for (int i = 1; i < count; i++) {
            int slot = client_map[i];
            if (srv->clients[slot].fd < 0) continue;
            
            if (fds[i].revents & (POLLHUP | POLLERR)) {
                disconnect_client_unlocked(srv, slot);
                continue;
            }
            
            if (fds[i].revents & POLLIN) {
                ws_client_t* c = &srv->clients[slot];
                
                if (c->rx_len + 4096 >= c->rx_cap) {
                    size_t need = c->rx_len + 4096;
                    size_t new_cap = need - 1;
                    new_cap |= new_cap >> 1;
                    new_cap |= new_cap >> 2;
                    new_cap |= new_cap >> 4;
                    new_cap |= new_cap >> 8;
                    new_cap |= new_cap >> 16;
#if SIZE_MAX > 0xFFFFFFFF
                    new_cap |= new_cap >> 32;
#endif
                    new_cap++;
                    if (new_cap < INITIAL_RX_CAP) new_cap = INITIAL_RX_CAP;
                    uint8_t* new_buf = realloc(c->rx_buf, new_cap + 1);
                    if (!new_buf) {
                        disconnect_client_unlocked(srv, slot);
                        continue;
                    }
                    c->rx_buf = new_buf;
                    c->rx_cap = new_cap;
                }
                
                ssize_t n = recv(c->fd, c->rx_buf + c->rx_len, c->rx_cap - c->rx_len, 0);
                if (n > 0) {
                    c->rx_len += n;
                    process_client_rx(srv, slot);
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    disconnect_client_unlocked(srv, slot);
                }
            }
        }
        
        pthread_mutex_unlock(&srv->clients_lock);
    }
    
    ZST_LOG_INFO("ws_server", "Thread exiting");
    return NULL;
}

zst_ws_server_t* zst_ws_server_create(int port) {
    zst_ws_server_t* srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    
    srv->port = port;
    srv->listen_fd = -1;
    srv->next_client_id = 1;
    pthread_mutex_init(&srv->clients_lock, NULL);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        srv->clients[i].fd = -1;
        srv->clients[i].id = -1;
    }
    
    return srv;
}

zst_result_t zst_ws_server_start(zst_ws_server_t* srv) {
    if (!srv) return ZST_ERROR;
    if (srv->running) return ZST_OK;
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ZST_LOG_ERROR("ws_server", "socket creation failed: %s", strerror(errno));
        return ZST_ERROR;
    }
    
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(srv->port);
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZST_LOG_ERROR("ws_server", "bind to port %d failed: %s", srv->port, strerror(errno));
        close(fd);
        return ZST_ERROR;
    }
    
    if (listen(fd, 10) < 0) {
        ZST_LOG_ERROR("ws_server", "listen failed: %s", strerror(errno));
        close(fd);
        return ZST_ERROR;
    }
    
    srv->listen_fd = fd;
    set_nonblocking(fd);
    
    srv->running = true;
    if (pthread_create(&srv->thread, NULL, ws_server_thread, srv) != 0) {
        ZST_LOG_ERROR("ws_server", "pthread_create failed");
        close(fd);
        srv->listen_fd = -1;
        srv->running = false;
        return ZST_ERROR;
    }
    
    ZST_LOG_INFO("ws_server", "Server started on port %d", srv->port);
    return ZST_OK;
}

zst_result_t zst_ws_server_stop(zst_ws_server_t* srv) {
    if (!srv) return ZST_ERROR;
    if (!srv->running) return ZST_OK;
    
    srv->running = false;
    pthread_join(srv->thread, NULL);
    
    pthread_mutex_lock(&srv->clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        disconnect_client_unlocked(srv, i);
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    pthread_mutex_unlock(&srv->clients_lock);
    
    return ZST_OK;
}

void zst_ws_server_free(zst_ws_server_t* srv) {
    if (!srv) return;
    zst_ws_server_stop(srv);
    pthread_mutex_destroy(&srv->clients_lock);
    free(srv);
}

zst_result_t zst_ws_send(zst_ws_server_t* srv, int client_id, const char* data, size_t len) {
    if (!srv) return ZST_ERROR;
    
    pthread_mutex_lock(&srv->clients_lock);
    
    int fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (srv->clients[i].id == client_id && srv->clients[i].fd >= 0) {
            fd = srv->clients[i].fd;
            break;
        }
    }
    
    if (fd < 0) {
        pthread_mutex_unlock(&srv->clients_lock);
        return ZST_ERROR;
    }
    
    uint8_t header[10];
    size_t header_len = 0;
    header[0] = 0x81;
    
    if (len <= 125) {
        header[1] = (uint8_t)len;
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (len >> ((7 - i) * 8)) & 0xFF;
        }
        header_len = 10;
    }
    
    ssize_t sent_hdr = send(fd, header, header_len, 0);
    ssize_t sent_data = send(fd, data, len, 0);
    
    pthread_mutex_unlock(&srv->clients_lock);
    
    if (sent_hdr != (ssize_t)header_len || sent_data != (ssize_t)len) {
        return ZST_ERROR;
    }
    return ZST_OK;
}

void zst_ws_server_set_callbacks(zst_ws_server_t* srv,
    void (*on_connect)(int client_id, void* user_data),
    void (*on_message)(int client_id, const char* msg, size_t len, void* user_data),
    void (*on_disconnect)(int client_id, void* user_data),
    void* user_data) {
    if (!srv) return;
    pthread_mutex_lock(&srv->clients_lock);
    srv->on_connect = on_connect;
    srv->on_message = on_message;
    srv->on_disconnect = on_disconnect;
    srv->cb_user_data = user_data;
    pthread_mutex_unlock(&srv->clients_lock);
}

/*──────────────────────────────────────────────────────────────────────────
  zstreamer Element Wrapper (ws_server)
──────────────────────────────────────────────────────────────────────────*/
typedef struct {
    zst_element_t* self;
    int port;
    zst_ws_server_t* srv;
} ws_server_priv_t;

static zst_result_t el_open(zst_element_t* el) {
    ws_server_priv_t* priv = el->priv;
    priv->srv = zst_ws_server_create(priv->port);
    if (!priv->srv) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t el_close(zst_element_t* el) {
    ws_server_priv_t* priv = el->priv;
    if (priv->srv) {
        zst_ws_server_free(priv->srv);
        priv->srv = NULL;
    }
    return ZST_OK;
}

static zst_result_t el_start(zst_element_t* el) {
    ws_server_priv_t* priv = el->priv;
    if (!priv->srv) return ZST_ERROR;
    return zst_ws_server_start(priv->srv);
}

static zst_result_t el_stop(zst_element_t* el) {
    ws_server_priv_t* priv = el->priv;
    if (!priv->srv) return ZST_ERROR;
    return zst_ws_server_stop(priv->srv);
}

static zst_result_t el_set_prop(zst_element_t* el, const char* name, const char* value) {
    ws_server_priv_t* priv = el->priv;
    if (strcmp(name, "port") == 0) {
        priv->port = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t el_get_prop(zst_element_t* el, const char* name, char* out, size_t max) {
    ws_server_priv_t* priv = el->priv;
    if (strcmp(name, "port") == 0) {
        snprintf(out, max, "%d", priv->port);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static const zst_element_ops_t g_ops = {
    .name          = "ws_server",
    .open          = el_open,
    .close         = el_close,
    .start         = el_start,
    .stop          = el_stop,
    .set_property  = el_set_prop,
    .get_property  = el_get_prop,
};

zst_element_t* zst_ws_server_element_create(void) {
    ws_server_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    priv->port = 8000;
    
    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }
    priv->self = el;
    return el;
}

static const zst_property_spec_t g_wsserver_properties[] = {
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8000", "WebSocket server listen port" }
};

static const zst_element_desc_t g_wsserver_elements[] = {
    {
        .name = "ws_server",
        .long_name = "WebSocket Server",
        .category = "Network/Signaling",
        .description = "Lightweight WebSocket server for WebRTC signaling",
        .author = "zstreamer",
        .properties = g_wsserver_properties,
        .nb_properties = sizeof(g_wsserver_properties) / sizeof(g_wsserver_properties[0]),
        .pads = NULL,
        .nb_pads = 0,
        .create = NULL
    }
};

static zst_element_t* plugin_create(const char* name) {
    if (strcmp(name, "ws_server") == 0) {
        return zst_ws_server_element_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name    = "ws_server_plugin",
        .author  = "zstreamer",
        .version = "1.0.0",
        .init    = NULL,
        .deinit  = NULL
    },
    .create_element = plugin_create
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_wsserver_elements) / sizeof(g_wsserver_elements[0]);
    }
    return g_wsserver_elements;
}

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void) {
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}

