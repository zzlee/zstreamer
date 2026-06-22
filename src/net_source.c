/*=============================================================================
    net_source.c — Raw network byte stream source (TCP/Unix)
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
#include "zstreamer/elements/zst_net_source.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_log.h"
#include "zst_clock.h"

typedef enum {
    NET_SOURCE_PROTOCOL_TCP_CLIENT,
    NET_SOURCE_PROTOCOL_TCP_SERVER,
    NET_SOURCE_PROTOCOL_UNIX_CLIENT,
    NET_SOURCE_PROTOCOL_UNIX_SERVER
} net_source_protocol_t;

typedef struct {
    int listen_fd;
    int conn_fd;
    char host[128];
    char path[256];
    uint16_t port;
    int chunk_size;
    net_source_protocol_t protocol;
    zst_buffer_pool_t* pool;
    uint64_t reconnect_delay_ms;
    uint64_t next_reconnect_time_ms;
    uint64_t read_timeout_ms;
} net_source_t;

static uint64_t
net_source_current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int
net_source_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
net_source_close_connection(net_source_t* s)
{
    if (!s) return;
    if (s->conn_fd >= 0) {
        close(s->conn_fd);
        s->conn_fd = -1;
    }
}

static void
net_source_close_listen(net_source_t* s)
{
    if (!s) return;
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}

static zst_result_t
net_source_create_listen_socket(net_source_t* s)
{
    if (!s) return ZST_ERROR;

    net_source_close_listen(s);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        ZST_LOG_ERROR("netsrc", "socket() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(s->port);

    if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZST_LOG_ERROR("netsrc", "bind() failed: %s", strerror(errno));
        net_source_close_listen(s);
        return ZST_ERROR;
    }

    if (listen(s->listen_fd, 1) < 0) {
        ZST_LOG_ERROR("netsrc", "listen() failed: %s", strerror(errno));
        net_source_close_listen(s);
        return ZST_ERROR;
    }

    if (net_source_set_nonblocking(s->listen_fd) < 0) {
        ZST_LOG_WARN("netsrc", "failed to set listen socket non-blocking");
    }

    return ZST_OK;
}

static zst_result_t
net_source_connect_socket(net_source_t* s)
{
    if (!s) return ZST_ERROR;
    net_source_close_connection(s);

    int fd = -1;
    if (s->protocol == NET_SOURCE_PROTOCOL_TCP_CLIENT) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
    }
    if (fd < 0) {
        ZST_LOG_ERROR("netsrc", "socket() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    if (net_source_set_nonblocking(fd) < 0) {
        ZST_LOG_WARN("netsrc", "failed to set socket non-blocking");
    }

    int result = -1;
    if (s->protocol == NET_SOURCE_PROTOCOL_TCP_CLIENT) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->port);
        if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
            ZST_LOG_ERROR("netsrc", "invalid host '%s'", s->host);
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
        ZST_LOG_WARN("netsrc", "connect() failed: %s", strerror(errno));
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
        ZST_LOG_WARN("netsrc", "connect failed after select: %s", strerror(err));
        close(fd);
        return ZST_TIMEOUT;
    }

    s->conn_fd = fd;
    return ZST_OK;
}

static zst_result_t
net_source_accept_connection(net_source_t* s)
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
        ZST_LOG_WARN("netsrc", "accept() failed: %s", strerror(errno));
        return ZST_ERROR;
    }

    if (net_source_set_nonblocking(fd) < 0) {
        ZST_LOG_WARN("netsrc", "failed to set client socket non-blocking");
    }

    s->conn_fd = fd;
    return ZST_OK;
}

static zst_result_t
net_source_ensure_connection(net_source_t* s)
{
    if (!s) return ZST_ERROR;
    if (s->conn_fd >= 0) return ZST_OK;

    if (s->protocol == NET_SOURCE_PROTOCOL_TCP_SERVER || s->protocol == NET_SOURCE_PROTOCOL_UNIX_SERVER) {
        return net_source_accept_connection(s);
    }

    uint64_t now = net_source_current_time_ms();
    if (now < s->next_reconnect_time_ms) {
        return ZST_TIMEOUT;
    }

    zst_result_t ret = net_source_connect_socket(s);
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
net_source_protocol_to_string(net_source_protocol_t protocol)
{
    switch (protocol) {
        case NET_SOURCE_PROTOCOL_TCP_CLIENT: return "tcp-client";
        case NET_SOURCE_PROTOCOL_TCP_SERVER: return "tcp-server";
        case NET_SOURCE_PROTOCOL_UNIX_CLIENT: return "unix-client";
        case NET_SOURCE_PROTOCOL_UNIX_SERVER: return "unix-server";
    }
    return "tcp-client";
}

static bool
net_source_string_to_protocol(const char* value, net_source_protocol_t* out)
{
    if (!value || !out) return false;
    if (strcmp(value, "tcp-client") == 0) {
        *out = NET_SOURCE_PROTOCOL_TCP_CLIENT;
    } else if (strcmp(value, "tcp-server") == 0) {
        *out = NET_SOURCE_PROTOCOL_TCP_SERVER;
    } else if (strcmp(value, "unix-client") == 0) {
        *out = NET_SOURCE_PROTOCOL_UNIX_CLIENT;
    } else if (strcmp(value, "unix-server") == 0) {
        *out = NET_SOURCE_PROTOCOL_UNIX_SERVER;
    } else if (strcmp(value, "unix") == 0) {
        *out = NET_SOURCE_PROTOCOL_UNIX_CLIENT;
    } else {
        return false;
    }
    return true;
}

static zst_result_t
net_source_open(zst_element_t* el)
{
    net_source_t* s = el->priv;
    s->conn_fd = -1;
    s->listen_fd = -1;
    s->reconnect_delay_ms = 500;
    s->next_reconnect_time_ms = 0;

    if (s->chunk_size <= 0) {
        s->chunk_size = 4096;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = (size_t)s->chunk_size,
        .buffer_type = ZST_BUFFER_USER
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        ZST_LOG_ERROR("netsrc", "failed to create buffer pool");
        return ZST_ERROR;
    }

    if (s->protocol == NET_SOURCE_PROTOCOL_TCP_SERVER) {
        return net_source_create_listen_socket(s);
    }

    if (s->protocol == NET_SOURCE_PROTOCOL_UNIX_SERVER) {
        net_source_close_listen(s);
        s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->listen_fd < 0) {
            ZST_LOG_ERROR("netsrc", "socket() failed: %s", strerror(errno));
            return ZST_ERROR;
        }

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, s->path, sizeof(addr.sun_path) - 1);
        unlink(s->path);

        if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ZST_LOG_ERROR("netsrc", "bind() failed: %s", strerror(errno));
            net_source_close_listen(s);
            return ZST_ERROR;
        }
        if (listen(s->listen_fd, 1) < 0) {
            ZST_LOG_ERROR("netsrc", "listen() failed: %s", strerror(errno));
            net_source_close_listen(s);
            return ZST_ERROR;
        }
        if (net_source_set_nonblocking(s->listen_fd) < 0) {
            ZST_LOG_WARN("netsrc", "failed to set listen socket non-blocking");
        }
        return ZST_OK;
    }

    return ZST_OK;
}

static zst_result_t
net_source_close(zst_element_t* el)
{
    net_source_t* s = el->priv;
    net_source_close_connection(s);
    net_source_close_listen(s);
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    return ZST_OK;
}

static zst_result_t
net_source_start(zst_element_t* el)
{
    net_source_t* s = el->priv;
    if (s->protocol == NET_SOURCE_PROTOCOL_TCP_CLIENT || s->protocol == NET_SOURCE_PROTOCOL_UNIX_CLIENT) {
        s->next_reconnect_time_ms = 0;
    }
    return ZST_OK;
}

static zst_result_t
net_source_stop(zst_element_t* el)
{
    net_source_t* s = el->priv;
    net_source_close_connection(s);
    return ZST_OK;
}

static zst_result_t
net_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    net_source_t* s = el->priv;

    zst_result_t connected = net_source_ensure_connection(s);
    if (connected != ZST_OK) {
        return connected;
    }

    if (s->conn_fd < 0) {
        return ZST_ERROR;
    }

    if (s->read_timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = (long)(s->read_timeout_ms / 1000ULL);
        tv.tv_usec = (long)((s->read_timeout_ms % 1000ULL) * 1000ULL);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s->conn_fd, &rfds);

        int sel = select(s->conn_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                return ZST_TIMEOUT;
            }
            ZST_LOG_WARN("netsrc", "select() failed: %s", strerror(errno));
            net_source_close_connection(s);
            return ZST_TIMEOUT;
        }
        if (sel == 0) {
            return ZST_TIMEOUT;
        }
    }

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        return ZST_ERROR;
    }

    ssize_t n = recv(s->conn_fd, buf->memory.data, s->chunk_size, 0);
    if (n > 0) {
        buf->memory.size = (size_t)n;
        if (el->clock) {
            buf->pts = zst_clock_get_time(el->clock);
        } else {
            buf->pts = 0;
        }
        buf->duration = 0;
        *out = buf;
        return ZST_OK;
    }

    zst_buffer_unref(buf);

    if (n == 0) {
        net_source_close_connection(s);
        return ZST_EOF;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return ZST_TIMEOUT;
    }

    ZST_LOG_WARN("netsrc", "recv() failed: %s", strerror(errno));
    net_source_close_connection(s);
    return ZST_TIMEOUT;
}

static zst_caps_t*
net_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_struct_t* caps_struct = calloc(1, sizeof(*caps_struct));
    if (!caps_struct) {
        zst_caps_destroy(caps);
        return NULL;
    }

    strncpy(caps_struct->media_type, "application/octet-stream", sizeof(caps_struct->media_type) - 1);
    caps_struct->type = ZST_CAPS_ANY;
    caps_struct->next = NULL;
    zst_caps_append(caps, caps_struct);
    return caps;
}

static zst_result_t
net_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    net_source_t* s = el->priv;
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
        if (!net_source_string_to_protocol(value, &s->protocol)) return ZST_ERROR;
        return ZST_OK;
    }
    if (strcmp(name, "path") == 0) {
        strncpy(s->path, value, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "chunk-size") == 0) {
        s->chunk_size = atoi(value);
        if (s->chunk_size <= 0) s->chunk_size = 4096;
        return ZST_OK;
    }
    if (strcmp(name, "read-timeout") == 0) {
        s->read_timeout_ms = (uint64_t)atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
net_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    net_source_t* s = el->priv;
    if (strcmp(name, "host") == 0) {
        strncpy(value_out, s->host, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "port") == 0) {
        snprintf(value_out, max_len, "%u", s->port);
        return ZST_OK;
    }
    if (strcmp(name, "protocol") == 0) {
        strncpy(value_out, net_source_protocol_to_string(s->protocol), max_len);
        return ZST_OK;
    }
    if (strcmp(name, "path") == 0) {
        strncpy(value_out, s->path, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "chunk-size") == 0) {
        snprintf(value_out, max_len, "%d", s->chunk_size);
        return ZST_OK;
    }
    if (strcmp(name, "read-timeout") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->read_timeout_ms);
        return ZST_OK;
    }
    return ZST_ERROR;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    net_source_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "netsrc",
    .open = net_source_open,
    .close = net_source_close,
    .start = net_source_start,
    .stop = net_source_stop,
    .process = net_source_process,
    .get_caps = net_source_get_caps,
    .set_property = net_source_set_property,
    .get_property = net_source_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_net_source_create(void)
{
    zst_element_t* el;
    net_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->port = 5000;
    strcpy(priv->host, "127.0.0.1");
    strcpy(priv->path, "/tmp/zst_net_source.sock");
    priv->chunk_size = 4096;
    priv->protocol = NET_SOURCE_PROTOCOL_TCP_CLIENT;
    priv->listen_fd = -1;
    priv->conn_fd = -1;
    priv->reconnect_delay_ms = 500;
    priv->next_reconnect_time_ms = 0;
    priv->read_timeout_ms = 100;

    el = zst_element_create(&g_ops, priv);
    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}



zst_element_t*
zst_net_source_create_with_config(const zst_net_source_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_net_source_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("netsrc");
    if (!el) return NULL;

    if (config->host) {
        zst_element_set_property_string(el, "host", config->host);
    }
    if (config->port > 0) {
        zst_element_set_property_int(el, "port", config->port);
    }
    if (config->protocol) {
        zst_element_set_property_string(el, "protocol", config->protocol);
    }
    if (config->path) {
        zst_element_set_property_string(el, "path", config->path);
    }
    if (config->chunk_size > 0) {
        zst_element_set_property_uint(el, "chunk-size", config->chunk_size);
    }
    if (config->read_timeout > 0) {
        zst_element_set_property_int(el, "read-timeout", config->read_timeout);
    }

    return el;
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "netsrc") == 0) {
        return zst_net_source_create();
    }
    return NULL;
}

static const zst_property_spec_t g_netsrc_properties[] = {
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "Network host to bind or connect to" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "Network port" },
    { "protocol", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Network protocol (tcp, udp, unix, tcp-server, unix-server)" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Unix domain socket path" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4096", "Chunk size in bytes to read at a time" },
    { "read-timeout", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1000", "Read timeout in milliseconds" }
};

static const zst_pad_template_t g_netsrc_pads[] = {
    { "src", ZST_PAD_SRC, "application/octet-stream" }
};

static const zst_element_desc_t g_netsrc_elements[] = {
    {
        .name = "netsrc",
        .long_name = "Network Source",
        .category = "Source/Network",
        .description = "Receives buffers from TCP/UDP or Unix sockets",
        .author = "zstreamer",
        .properties = g_netsrc_properties,
        .nb_properties = sizeof(g_netsrc_properties) / sizeof(g_netsrc_properties[0]),
        .pads = g_netsrc_pads,
        .nb_pads = sizeof(g_netsrc_pads) / sizeof(g_netsrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "netsrc_plugin",
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
        *nb_elements_out = sizeof(g_netsrc_elements) / sizeof(g_netsrc_elements[0]);
    }
    return g_netsrc_elements;
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
