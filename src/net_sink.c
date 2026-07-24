/*=============================================================================
    net_sink.c — Raw network byte stream sink (TCP/Unix)
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_timestamp_pacer.h"

typedef enum {
    NET_SINK_PROTOCOL_TCP_CLIENT,
    NET_SINK_PROTOCOL_TCP_SERVER,
    NET_SINK_PROTOCOL_UDP_CLIENT,
    NET_SINK_PROTOCOL_UNIX_CLIENT,
    NET_SINK_PROTOCOL_UNIX_SERVER,
    NET_SINK_PROTOCOL_UDP_SERVER
} net_sink_protocol_t;

typedef struct {
    int listen_fd;
    int conn_fd;
    char host[128];
    char path[256];
    uint16_t port;
    net_sink_protocol_t protocol;
    uint64_t reconnect_delay_ms;
    uint64_t next_reconnect_time_ms;
    uint64_t write_timeout_ms;
    int multicast_ttl;
    int multicast_loop;
    int timestamp_pacing;
    uint64_t pacing_tolerance_ms;
    uint64_t pacing_reset_threshold_ms;
    uint64_t max_lateness_ms;
    zst_timestamp_pacer_t pacer;
    bool pacer_initialized;
    struct sockaddr_in client_addr;
    bool client_registered;
} net_sink_t;

static uint64_t
net_sink_current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int
net_sink_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
net_sink_close_connection(net_sink_t* s)
{
    if (!s) return;
    if (s->conn_fd >= 0) {
        close(s->conn_fd);
        s->conn_fd = -1;
    }
}

static void
net_sink_close_listen(net_sink_t* s)
{
    if (!s) return;
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}

static zst_result_t
net_sink_create_listen_socket(net_sink_t* s)
{
    if (!s) return ZST_ERROR;

    net_sink_close_listen(s);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        ZST_LOG_ERROR("netsink", "socket() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(s->port);

    if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZST_LOG_ERROR("netsink", "bind() failed: %s", strerror(errno));
        net_sink_close_listen(s);
        return ZST_ERROR;
    }

    if (listen(s->listen_fd, 1) < 0) {
        ZST_LOG_ERROR("netsink", "listen() failed: %s", strerror(errno));
        net_sink_close_listen(s);
        return ZST_ERROR;
    }

    if (net_sink_set_nonblocking(s->listen_fd) < 0) {
        ZST_LOG_WARN("netsink", "failed to set listen socket non-blocking");
    }

    return ZST_OK;
}

static zst_result_t
net_sink_connect_socket(net_sink_t* s)
{
    if (!s) return ZST_ERROR;
    net_sink_close_connection(s);

    int fd = -1;
    if (s->protocol == NET_SINK_PROTOCOL_TCP_CLIENT) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
    }
    if (fd < 0) {
        ZST_LOG_ERROR("netsink", "socket() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    if (net_sink_set_nonblocking(fd) < 0) {
        ZST_LOG_WARN("netsink", "failed to set socket non-blocking");
    }

    int result = -1;
    if (s->protocol == NET_SINK_PROTOCOL_TCP_CLIENT) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->port);
        if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
            ZST_LOG_ERROR("netsink", "invalid host '%s'", s->host);
            close(fd);
            return ZST_ERROR;
        }
        result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    } else {
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s->path, sizeof(addr.sun_path) - 1);
        result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    }

    if (result == 0) {
        s->conn_fd = fd;
        return ZST_OK;
    }

    if (errno != EINPROGRESS && errno != EAGAIN) {
        ZST_LOG_WARN("netsink", "connect() failed: %s", strerror(errno));
        close(fd);
        return ZST_TIMEOUT;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (sel <= 0) {
        close(fd);
        return ZST_TIMEOUT;
    }

    int err = 0;
    socklen_t sz = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sz) < 0 || err != 0) {
        ZST_LOG_WARN("netsink", "connect failed after select: %s", strerror(err));
        close(fd);
        return ZST_TIMEOUT;
    }

    s->conn_fd = fd;
    return ZST_OK;
}

static zst_result_t
net_sink_accept_connection(net_sink_t* s)
{
    if (!s || s->listen_fd < 0) return ZST_ERROR;
    if (s->conn_fd >= 0) return ZST_OK;

    struct sockaddr_in peer = {0};
    socklen_t len = sizeof(peer);
    int fd = accept(s->listen_fd, (struct sockaddr*)&peer, &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ZST_TIMEOUT;
        }
        ZST_LOG_WARN("netsink", "accept() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    if (net_sink_set_nonblocking(fd) < 0) {
        ZST_LOG_WARN("netsink", "failed to set client socket non-blocking");
    }

    s->conn_fd = fd;
    return ZST_OK;
}

static zst_result_t
net_sink_ensure_connection(net_sink_t* s)
{
    if (!s) return ZST_ERROR;
    if (s->conn_fd >= 0) return ZST_OK;

    if (s->protocol == NET_SINK_PROTOCOL_TCP_SERVER || s->protocol == NET_SINK_PROTOCOL_UNIX_SERVER) {
        return net_sink_accept_connection(s);
    }

    uint64_t now = net_sink_current_time_ms();
    if (now < s->next_reconnect_time_ms) {
        return ZST_TIMEOUT;
    }

    zst_result_t ret = net_sink_connect_socket(s);
    if (ret == ZST_OK) {
        s->reconnect_delay_ms = 500;
        s->next_reconnect_time_ms = now;
        return ZST_OK;
    }

    if (ret == ZST_TIMEOUT) {
        if (s->reconnect_delay_ms == 0) {
            s->reconnect_delay_ms = 500;
        } else {
            s->reconnect_delay_ms = s->reconnect_delay_ms * 2;
            if (s->reconnect_delay_ms > 5000)
                s->reconnect_delay_ms = 5000;
        }
        s->next_reconnect_time_ms = now + s->reconnect_delay_ms;
        return ZST_TIMEOUT;
    }

    return ret;
}

static const char*
net_sink_protocol_to_string(net_sink_protocol_t protocol)
{
    switch (protocol) {
        case NET_SINK_PROTOCOL_TCP_CLIENT: return "tcp-client";
        case NET_SINK_PROTOCOL_TCP_SERVER: return "tcp-server";
        case NET_SINK_PROTOCOL_UDP_CLIENT: return "udp-client";
        case NET_SINK_PROTOCOL_UNIX_CLIENT: return "unix-client";
        case NET_SINK_PROTOCOL_UNIX_SERVER: return "unix-server";
        case NET_SINK_PROTOCOL_UDP_SERVER: return "udp-server";
    }
    return "tcp-client";
}

static bool
net_sink_string_to_protocol(const char* value, net_sink_protocol_t* out)
{
    if (!value || !out) return false;
    if (strcmp(value, "tcp-client") == 0 || strcmp(value, "tcp") == 0) {
        *out = NET_SINK_PROTOCOL_TCP_CLIENT;
    } else if (strcmp(value, "tcp-server") == 0) {
        *out = NET_SINK_PROTOCOL_TCP_SERVER;
    } else if (strcmp(value, "udp-client") == 0 || strcmp(value, "udp") == 0) {
        *out = NET_SINK_PROTOCOL_UDP_CLIENT;
    } else if (strcmp(value, "udp-server") == 0) {
        *out = NET_SINK_PROTOCOL_UDP_SERVER;
    } else if (strcmp(value, "unix-client") == 0) {
        *out = NET_SINK_PROTOCOL_UNIX_CLIENT;
    } else if (strcmp(value, "unix-server") == 0) {
        *out = NET_SINK_PROTOCOL_UNIX_SERVER;
    } else if (strcmp(value, "unix") == 0) {
        *out = NET_SINK_PROTOCOL_UNIX_CLIENT;
    } else {
        return false;
    }
    return true;
}

static bool
net_sink_parse_bool(const char* value)
{
    return value &&
        (strcmp(value, "1") == 0 ||
         strcmp(value, "true") == 0 ||
         strcmp(value, "yes") == 0 ||
         strcmp(value, "on") == 0);
}

static zst_time_t
net_sink_ms_to_ns(uint64_t value_ms)
{
    if (value_ms > UINT64_MAX / 1000000ULL) {
        return UINT64_MAX;
    }
    return (zst_time_t)(value_ms * 1000000ULL);
}

static void
net_sink_apply_pacer_config(net_sink_t* s)
{
    if (!s || !s->pacer_initialized) return;

    zst_timestamp_pacer_set_enabled(&s->pacer, s->timestamp_pacing);
    zst_timestamp_pacer_configure(&s->pacer,
                                  net_sink_ms_to_ns(s->pacing_tolerance_ms),
                                  net_sink_ms_to_ns(s->pacing_reset_threshold_ms),
                                  net_sink_ms_to_ns(s->max_lateness_ms));
}

static zst_result_t
net_sink_init_pacer(net_sink_t* s)
{
    if (!s) return ZST_ERROR;
    if (!s->pacer_initialized) {
        zst_timestamp_pacer_init(&s->pacer);
        s->pacer_initialized = true;
    }
    net_sink_apply_pacer_config(s);
    zst_timestamp_pacer_reset(&s->pacer);
    return ZST_OK;
}

static void
net_sink_deinit_pacer(net_sink_t* s)
{
    if (!s || !s->pacer_initialized) return;
    zst_timestamp_pacer_deinit(&s->pacer);
    memset(&s->pacer, 0, sizeof(s->pacer));
    s->pacer_initialized = false;
}

static zst_result_t
net_sink_pace_udp_buffer(zst_element_t* el, net_sink_t* s, zst_buffer_t* in)
{
    if (!el || !s || !in || !s->timestamp_pacing) {
        return ZST_OK;
    }
    if (!s->pacer_initialized) {
        zst_result_t init_res = net_sink_init_pacer(s);
        if (init_res != ZST_OK) return init_res;
    }

    zst_time_t timestamp = in->dts ? in->dts : in->pts;
    int dropped = 0;
    zst_result_t ret = zst_timestamp_pacer_wait(&s->pacer, el->clock, timestamp, &dropped);
    if (ret == ZST_AGAIN && dropped) {
        return ZST_AGAIN;
    }
    return ret;
}

static zst_result_t
net_sink_open(zst_element_t* el)
{
    net_sink_t* s = el->priv;
    s->conn_fd = -1;
    s->listen_fd = -1;
    s->reconnect_delay_ms = 500;
    s->next_reconnect_time_ms = 0;
    s->client_registered = false;
    memset(&s->client_addr, 0, sizeof(s->client_addr));

    if (s->protocol == NET_SINK_PROTOCOL_TCP_SERVER) {
        return net_sink_create_listen_socket(s);
    }

    if (s->protocol == NET_SINK_PROTOCOL_UDP_SERVER) {
        s->conn_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->conn_fd < 0) {
            ZST_LOG_ERROR("netsink", "UDP server socket() failed: %s", strerror(errno));
            return ZST_ERROR;
        }
        int opt = 1;
        setsockopt(s->conn_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(s->port);

        if (bind(s->conn_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ZST_LOG_ERROR("netsink", "UDP server bind() failed: %s", strerror(errno));
            close(s->conn_fd);
            s->conn_fd = -1;
            return ZST_ERROR;
        }

        if (net_sink_set_nonblocking(s->conn_fd) < 0) {
            ZST_LOG_WARN("netsink", "failed to set UDP server socket non-blocking");
        }

        ZST_LOG_INFO("netsink", "UDP server listening on port %u", s->port);
        return ZST_OK;
    }

    if (s->protocol == NET_SINK_PROTOCOL_UDP_CLIENT) {
        s->conn_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->conn_fd < 0) {
            ZST_LOG_ERROR("netsink", "UDP socket() failed: %s", strerror(errno));
            return ZST_ERROR;
        }
        setsockopt(s->conn_fd, IPPROTO_IP, IP_MULTICAST_TTL,
                   &s->multicast_ttl, sizeof(s->multicast_ttl));
        unsigned char loop = (unsigned char)(s->multicast_loop ? 1 : 0);
        setsockopt(s->conn_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        return ZST_OK;
    }

    if (s->protocol == NET_SINK_PROTOCOL_UNIX_SERVER) {
        net_sink_close_listen(s);
        s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->listen_fd < 0) {
            ZST_LOG_ERROR("netsink", "socket() failed: %s", strerror(errno));
            return ZST_ERROR;
        }

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s->path, sizeof(addr.sun_path) - 1);
        unlink(s->path);

        if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ZST_LOG_ERROR("netsink", "bind() failed: %s", strerror(errno));
            net_sink_close_listen(s);
            return ZST_ERROR;
        }
        if (listen(s->listen_fd, 1) < 0) {
            ZST_LOG_ERROR("netsink", "listen() failed: %s", strerror(errno));
            net_sink_close_listen(s);
            return ZST_ERROR;
        }
        if (net_sink_set_nonblocking(s->listen_fd) < 0) {
            ZST_LOG_WARN("netsink", "failed to set listen socket non-blocking");
        }
        return ZST_OK;
    }

    return ZST_OK;
}

static zst_result_t
net_sink_close(zst_element_t* el)
{
    net_sink_t* s = el->priv;
    net_sink_close_connection(s);
    net_sink_close_listen(s);
    net_sink_deinit_pacer(s);
    return ZST_OK;
}

static zst_result_t
net_sink_start(zst_element_t* el)
{
    net_sink_t* s = el->priv;
    s->client_registered = false;
    memset(&s->client_addr, 0, sizeof(s->client_addr));
    if (s->pacer_initialized) {
        net_sink_apply_pacer_config(s);
        zst_timestamp_pacer_reset(&s->pacer);
    }
    if (s->protocol == NET_SINK_PROTOCOL_TCP_CLIENT || s->protocol == NET_SINK_PROTOCOL_UNIX_CLIENT) {
        s->next_reconnect_time_ms = 0;
    }
    return ZST_OK;
}

static zst_result_t
net_sink_stop(zst_element_t* el)
{
    net_sink_t* s = el->priv;
    net_sink_close_connection(s);
    if (s->pacer_initialized) {
        zst_timestamp_pacer_reset(&s->pacer);
    }
    return ZST_OK;
}

static zst_result_t
net_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    net_sink_t* s = el->priv;

    if (!in) {
        return ZST_ERROR;
    }

    /* For EOS buffer, flush connection and close */
    if (in->memory.size == 0 && in->memory.data == NULL) {
        net_sink_close_connection(s);
        return ZST_OK;
    }

    if (s->protocol == NET_SINK_PROTOCOL_UDP_SERVER) {
        if (s->conn_fd < 0) {
            return ZST_ERROR;
        }
        if (!s->client_registered) {
            char reg_buf[16];
            struct sockaddr_in sender_addr = {0};
            socklen_t addr_len = sizeof(sender_addr);
            ssize_t n_rec = recvfrom(s->conn_fd, reg_buf, sizeof(reg_buf), 0,
                                     (struct sockaddr*)&sender_addr, &addr_len);
            if (n_rec < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    return ZST_TIMEOUT;
                }
                ZST_LOG_WARN("netsink", "UDP server recvfrom() failed: %s", strerror(errno));
                return ZST_TIMEOUT;
            }
            s->client_addr = sender_addr;
            s->client_registered = true;
            char ip_str[64];
            inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));
            ZST_LOG_INFO("netsink", "UDP server registered client from %s:%u",
                         ip_str, ntohs(sender_addr.sin_port));
        }

        zst_result_t pace_res = net_sink_pace_udp_buffer(el, s, in);
        if (pace_res == ZST_AGAIN) return ZST_OK;
        if (pace_res != ZST_OK) return pace_res;

        ssize_t n = sendto(s->conn_fd, in->memory.data, in->memory.size, 0,
                           (struct sockaddr*)&s->client_addr, sizeof(s->client_addr));
        if (n >= 0 && (size_t)n == in->memory.size) return ZST_OK;
        if (n >= 0) return ZST_TIMEOUT;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return ZST_TIMEOUT;

        ZST_LOG_WARN("netsink", "UDP server sendto() failed: %s, resetting client registration", strerror(errno));
        s->client_registered = false;
        return ZST_TIMEOUT;
    }

    if (s->protocol == NET_SINK_PROTOCOL_UDP_CLIENT) {
        if (s->conn_fd < 0) {
            s->conn_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (s->conn_fd < 0) return ZST_ERROR;
        }
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->port);
        if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
            ZST_LOG_WARN("netsink", "invalid UDP host '%s'", s->host);
            return ZST_ERROR;
        }
        zst_result_t pace_res = net_sink_pace_udp_buffer(el, s, in);
        if (pace_res == ZST_AGAIN) return ZST_OK;
        if (pace_res != ZST_OK) return pace_res;

        ssize_t n = sendto(s->conn_fd, in->memory.data, in->memory.size, 0,
                           (struct sockaddr*)&addr, sizeof(addr));
        if (n >= 0 && (size_t)n == in->memory.size) return ZST_OK;
        if (n >= 0) return ZST_TIMEOUT;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return ZST_TIMEOUT;
        ZST_LOG_WARN("netsink", "UDP sendto() failed: %s", strerror(errno));
        return ZST_TIMEOUT;
    }

    zst_result_t connected = net_sink_ensure_connection(s);
    if (connected != ZST_OK) {
        return connected;
    }

    if (s->conn_fd < 0) {
        return ZST_ERROR;
    }

    if (s->write_timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = (long)(s->write_timeout_ms / 1000ULL);
        tv.tv_usec = (long)((s->write_timeout_ms % 1000ULL) * 1000ULL);
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s->conn_fd, &wfds);

        int sel = select(s->conn_fd + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                return ZST_TIMEOUT;
            }
            ZST_LOG_WARN("netsink", "select() failed: %s", strerror(errno));
            net_sink_close_connection(s);
            return ZST_TIMEOUT;
        }
        if (sel == 0) {
            return ZST_TIMEOUT;
        }
    }

    ssize_t n = send(s->conn_fd, in->memory.data, in->memory.size, 0);
    if (n > 0 && (size_t)n == in->memory.size) {
        return ZST_OK;
    }

    if (n > 0 && (size_t)n < in->memory.size) {
        ZST_LOG_WARN("netsink", "partial write: %zd/%zu bytes", n, in->memory.size);
        return ZST_TIMEOUT;
    }

    if (n == 0) {
        net_sink_close_connection(s);
        return ZST_ERROR;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return ZST_TIMEOUT;
    }

    ZST_LOG_WARN("netsink", "send() failed: %s", strerror(errno));
    net_sink_close_connection(s);
    return ZST_TIMEOUT;
}

static zst_caps_t*
net_sink_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    return NULL;
}

static zst_result_t
net_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    net_sink_t* s = el->priv;
    if (strcmp(name, "host") == 0) {
        strncpy(s->host, value, sizeof(s->host) - 1);
        s->host[sizeof(s->host) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "port") == 0) {
        s->port = (uint16_t)atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "protocol") == 0) {
        if (!net_sink_string_to_protocol(value, &s->protocol)) return ZST_ERROR;
        return ZST_OK;
    }
    if (strcmp(name, "path") == 0) {
        strncpy(s->path, value, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "write-timeout") == 0) {
        s->write_timeout_ms = (uint64_t)atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "ttl") == 0 || strcmp(name, "multicast-ttl") == 0) {
        int ttl = atoi(value);
        if (ttl < 0 || ttl > 255) return ZST_ERROR;
        s->multicast_ttl = ttl;
        return ZST_OK;
    }
    if (strcmp(name, "loop") == 0 || strcmp(name, "multicast-loop") == 0) {
        s->multicast_loop = net_sink_parse_bool(value);
        return ZST_OK;
    }
    if (strcmp(name, "timestamp-pacing") == 0) {
        s->timestamp_pacing = net_sink_parse_bool(value);
        net_sink_apply_pacer_config(s);
        return ZST_OK;
    }
    if (strcmp(name, "pacing-tolerance-ms") == 0) {
        long long v = atoll(value);
        if (v < 0) return ZST_ERROR;
        s->pacing_tolerance_ms = (uint64_t)v;
        net_sink_apply_pacer_config(s);
        return ZST_OK;
    }
    if (strcmp(name, "pacing-reset-threshold-ms") == 0) {
        long long v = atoll(value);
        if (v < 0) return ZST_ERROR;
        s->pacing_reset_threshold_ms = (uint64_t)v;
        net_sink_apply_pacer_config(s);
        return ZST_OK;
    }
    if (strcmp(name, "max-lateness-ms") == 0) {
        long long v = atoll(value);
        if (v < 0) return ZST_ERROR;
        s->max_lateness_ms = (uint64_t)v;
        net_sink_apply_pacer_config(s);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
net_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    net_sink_t* s = el->priv;
    if (strcmp(name, "host") == 0) {
        strncpy(value_out, s->host, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "port") == 0) {
        snprintf(value_out, max_len, "%u", s->port);
        return ZST_OK;
    }
    if (strcmp(name, "protocol") == 0) {
        strncpy(value_out, net_sink_protocol_to_string(s->protocol), max_len);
        return ZST_OK;
    }
    if (strcmp(name, "path") == 0) {
        strncpy(value_out, s->path, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "write-timeout") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->write_timeout_ms);
        return ZST_OK;
    }
    if (strcmp(name, "ttl") == 0 || strcmp(name, "multicast-ttl") == 0) {
        snprintf(value_out, max_len, "%d", s->multicast_ttl);
        return ZST_OK;
    }
    if (strcmp(name, "loop") == 0 || strcmp(name, "multicast-loop") == 0) {
        snprintf(value_out, max_len, "%s", s->multicast_loop ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "timestamp-pacing") == 0) {
        snprintf(value_out, max_len, "%s", s->timestamp_pacing ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "pacing-tolerance-ms") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->pacing_tolerance_ms);
        return ZST_OK;
    }
    if (strcmp(name, "pacing-reset-threshold-ms") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->pacing_reset_threshold_ms);
        return ZST_OK;
    }
    if (strcmp(name, "max-lateness-ms") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->max_lateness_ms);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "netsink",
    .open = net_sink_open,
    .close = net_sink_close,
    .start = net_sink_start,
    .stop = net_sink_stop,
    .process = net_sink_process,
    .get_caps = net_sink_get_caps,
    .set_property = net_sink_set_property,
    .get_property = net_sink_get_property,
};

zst_element_t*
zst_net_sink_create(void)
{
    zst_element_t* el;
    net_sink_t* priv;
    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));
    priv->port = 5000;
    strcpy(priv->host, "127.0.0.1");
    strcpy(priv->path, "/tmp/zst_net_sink.sock");
    priv->protocol = NET_SINK_PROTOCOL_TCP_CLIENT;
    priv->listen_fd = -1;
    priv->conn_fd = -1;
    priv->reconnect_delay_ms = 500;
    priv->next_reconnect_time_ms = 0;
    priv->write_timeout_ms = 100;
    priv->multicast_ttl = 16;
    priv->multicast_loop = 1;
    priv->timestamp_pacing = 0;
    priv->pacing_tolerance_ms = 5;
    priv->pacing_reset_threshold_ms = 2000;
    priv->max_lateness_ms = 0;
    priv->pacer_initialized = false;

    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "netsink") == 0) {
        return zst_net_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_netsink_properties[] = {
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "Network host to connect to or listen on" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "Network port" },
    { "protocol", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Network protocol (tcp, udp, udp-client, udp-server, unix, tcp-server, unix-server)" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Unix domain socket path" },
    { "write-timeout", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1000", "Write timeout in milliseconds" },
    { "ttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "Multicast TTL for UDP" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Enable UDP multicast loopback" },
    { "timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Pace UDP sends according to buffer timestamps" },
    { "pacing-tolerance-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Timestamp pacing tolerance in milliseconds" },
    { "pacing-reset-threshold-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Timestamp discontinuity threshold before resetting pacing" },
    { "max-lateness-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Drop UDP buffers later than this many milliseconds; 0 disables dropping" }
};

static const zst_pad_template_t g_netsink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "application/octet-stream" }
};

static const zst_element_desc_t g_netsink_elements[] = {
    {
        .name = "netsink",
        .long_name = "Network Sink",
        .category = "Sink/Network",
        .description = "Sends buffers to TCP/UDP or Unix sockets",
        .author = "zstreamer",
        .properties = g_netsink_properties,
        .nb_properties = sizeof(g_netsink_properties) / sizeof(g_netsink_properties[0]),
        .pads = g_netsink_pads,
        .nb_pads = sizeof(g_netsink_pads) / sizeof(g_netsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "netsink_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_netsink_elements) / sizeof(g_netsink_elements[0]);
    }
    return g_netsink_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
