/*=============================================================================
    srt_sink.c — SRT (Secure Reliable Transport) Sink Element
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <srt/srt.h>
#include "srt_common.h"

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_clock.h"

typedef struct {
    char uri[512];
    char host[128];
    int port;
    char mode[32]; // "caller", "listener", "rendezvous"
    int latency;   // in ms
    char passphrase[128];
    int pbkeylen;  // 16, 24, 32
    char streamid[512];
    int payload_size;
    bool tlpktdrop;
    int64_t maxbw;
    int rcvbuf;
    int sndbuf;

    SRTSOCKET listen_sock;
    SRTSOCKET conn_sock;
    bool connecting;

    uint64_t reconnect_delay_ms;
    uint64_t next_reconnect_time_ms;

    // Timestamp mapping
    uint64_t base_srt_time;
    uint64_t base_pts;
} srt_sink_t;

static uint64_t
srt_sink_current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static uint64_t
srt_sink_time_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void
srt_sink_apply_socket_opts(SRTSOCKET sock, srt_sink_t* s)
{
    int latency = s->latency;
    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency));

    int drop = s->tlpktdrop ? 1 : 0;
    srt_setsockopt(sock, 0, SRTO_TLPKTDROP, &drop, sizeof(drop));

    if (s->maxbw >= 0) {
        int64_t bw = s->maxbw;
        srt_setsockopt(sock, 0, SRTO_MAXBW, &bw, sizeof(bw));
    }

    if (s->rcvbuf > 0) {
        int buf = s->rcvbuf;
        srt_setsockopt(sock, 0, SRTO_RCVBUF, &buf, sizeof(buf));
    }

    if (s->sndbuf > 0) {
        int buf = s->sndbuf;
        srt_setsockopt(sock, 0, SRTO_SNDBUF, &buf, sizeof(buf));
    }

    if (strlen(s->passphrase) > 0) {
        srt_setsockopt(sock, 0, SRTO_PASSPHRASE, s->passphrase, strlen(s->passphrase));
        int keylen = s->pbkeylen;
        srt_setsockopt(sock, 0, SRTO_PBKEYLEN, &keylen, sizeof(keylen));
    }

    if (strlen(s->streamid) > 0) {
        srt_setsockopt(sock, 0, SRTO_STREAMID, s->streamid, strlen(s->streamid));
    }
}

static zst_result_t
srt_sink_open(zst_element_t* el)
{
    srt_sink_t* s = el->priv;
    srt_global_init();

    s->listen_sock = SRT_INVALID_SOCK;
    s->conn_sock = SRT_INVALID_SOCK;
    s->connecting = false;
    s->reconnect_delay_ms = 500;
    s->next_reconnect_time_ms = 0;
    s->base_srt_time = 0;
    s->base_pts = 0;

    if (s->payload_size <= 0) {
        s->payload_size = 1316;
    }

    if (strcmp(s->mode, "listener") == 0) {
        s->listen_sock = srt_create_socket();
        if (s->listen_sock == SRT_INVALID_SOCK) {
            ZST_LOG_ERROR("srtsink", "Failed to create listener socket: %s", srt_getlasterror_str());
            srt_global_cleanup();
            return ZST_ERROR;
        }

        int syn = 0;
        srt_setsockopt(s->listen_sock, 0, SRTO_RCVSYN, &syn, sizeof(syn));

        int reuse = 1;
        srt_setsockopt(s->listen_sock, 0, SRTO_REUSEADDR, &reuse, sizeof(reuse));

        srt_sink_apply_socket_opts(s->listen_sock, s);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->port);
        if (strlen(s->host) > 0 && strcmp(s->host, "127.0.0.1") != 0 && strcmp(s->host, "0.0.0.0") != 0) {
            inet_pton(AF_INET, s->host, &addr.sin_addr);
        } else {
            addr.sin_addr.s_addr = INADDR_ANY;
        }

        if (srt_bind(s->listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SRT_ERROR) {
            ZST_LOG_ERROR("srtsink", "srt_bind failed: %s", srt_getlasterror_str());
            srt_close(s->listen_sock);
            s->listen_sock = SRT_INVALID_SOCK;
            srt_global_cleanup();
            return ZST_ERROR;
        }

        if (srt_listen(s->listen_sock, 1) == SRT_ERROR) {
            ZST_LOG_ERROR("srtsink", "srt_listen failed: %s", srt_getlasterror_str());
            srt_close(s->listen_sock);
            s->listen_sock = SRT_INVALID_SOCK;
            srt_global_cleanup();
            return ZST_ERROR;
        }
        ZST_LOG_INFO("srtsink", "Listening on port %d", s->port);
    }

    return ZST_OK;
}

static zst_result_t
srt_sink_close(zst_element_t* el)
{
    srt_sink_t* s = el->priv;
    if (s->conn_sock != SRT_INVALID_SOCK) {
        srt_close(s->conn_sock);
        s->conn_sock = SRT_INVALID_SOCK;
    }
    if (s->listen_sock != SRT_INVALID_SOCK) {
        srt_close(s->listen_sock);
        s->listen_sock = SRT_INVALID_SOCK;
    }
    srt_global_cleanup();
    return ZST_OK;
}

static zst_result_t
srt_sink_start(zst_element_t* el)
{
    srt_sink_t* s = el->priv;
    s->reconnect_delay_ms = 500;
    s->next_reconnect_time_ms = 0;
    s->base_srt_time = 0;
    s->base_pts = 0;
    s->connecting = false;
    return ZST_OK;
}

static zst_result_t
srt_sink_stop(zst_element_t* el)
{
    srt_sink_t* s = el->priv;
    if (s->conn_sock != SRT_INVALID_SOCK) {
        srt_close(s->conn_sock);
        s->conn_sock = SRT_INVALID_SOCK;
    }
    s->connecting = false;
    return ZST_OK;
}

static zst_result_t
srt_sink_ensure_connection(zst_element_t* el, srt_sink_t* s)
{
    if (s->conn_sock != SRT_INVALID_SOCK && !s->connecting) {
        return ZST_OK;
    }

    if (strcmp(s->mode, "listener") == 0) {
        struct sockaddr_in peer;
        int peer_len = sizeof(peer);
        SRTSOCKET client = srt_accept(s->listen_sock, (struct sockaddr*)&peer, &peer_len);
        if (client == SRT_INVALID_SOCK) {
            int err = srt_getlasterror(NULL);
            if (err == SRT_EASYNCRCV) {
                return ZST_TIMEOUT;
            }
            ZST_LOG_ERROR("srtsink", "srt_accept failed: %s", srt_getlasterror_str());
            return ZST_ERROR;
        }
        s->conn_sock = client;
        int syn = 0;
        srt_setsockopt(s->conn_sock, 0, SRTO_RCVSYN, &syn, sizeof(syn));
        srt_setsockopt(s->conn_sock, 0, SRTO_SNDSYN, &syn, sizeof(syn));
        srt_sink_apply_socket_opts(s->conn_sock, s);
        ZST_LOG_INFO("srtsink", "Accepted SRT connection");
        return ZST_OK;
    }

    // Caller or Rendezvous mode
    uint64_t now = srt_sink_current_time_ms();
    if (!s->connecting) {
        if (now < s->next_reconnect_time_ms) {
            return ZST_TIMEOUT;
        }

        s->conn_sock = srt_create_socket();
        if (s->conn_sock == SRT_INVALID_SOCK) {
            ZST_LOG_ERROR("srtsink", "Failed to create socket: %s", srt_getlasterror_str());
            return ZST_ERROR;
        }

        int syn = 0;
        srt_setsockopt(s->conn_sock, 0, SRTO_RCVSYN, &syn, sizeof(syn));
        srt_setsockopt(s->conn_sock, 0, SRTO_SNDSYN, &syn, sizeof(syn));

        if (strcmp(s->mode, "rendezvous") == 0) {
            int rend = 1;
            srt_setsockopt(s->conn_sock, 0, SRTO_RENDEZVOUS, &rend, sizeof(rend));

            /* Rendezvous requires binding to the local port before connecting.
             * Without this, srt_connect fails with SRT_ERDVUNBOUND. */
            struct sockaddr_in local = {0};
            local.sin_family = AF_INET;
            local.sin_port = htons(s->port);
            local.sin_addr.s_addr = INADDR_ANY;
            if (srt_bind(s->conn_sock, (struct sockaddr*)&local, sizeof(local)) == SRT_ERROR) {
                ZST_LOG_ERROR("srtsink", "srt_bind failed (rendezvous): %s", srt_getlasterror_str());
                srt_close(s->conn_sock);
                s->conn_sock = SRT_INVALID_SOCK;
                return ZST_ERROR;
            }
        }

        srt_sink_apply_socket_opts(s->conn_sock, s);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->port);
        if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
            ZST_LOG_ERROR("srtsink", "Invalid host address '%s'", s->host);
            srt_close(s->conn_sock);
            s->conn_sock = SRT_INVALID_SOCK;
            return ZST_ERROR;
        }

        ZST_LOG_INFO("srtsink", "Connecting to %s:%d (mode: %s)", s->host, s->port, s->mode);
        int res = srt_connect(s->conn_sock, (struct sockaddr*)&addr, sizeof(addr));
        if (res == SRT_ERROR) {
            int err = srt_getlasterror(NULL);
            if (err != SRT_EASYNCSND) {
                ZST_LOG_WARN("srtsink", "srt_connect failed immediately: %s", srt_getlasterror_str());
                srt_close(s->conn_sock);
                s->conn_sock = SRT_INVALID_SOCK;
                s->next_reconnect_time_ms = now + s->reconnect_delay_ms;
                s->reconnect_delay_ms = (s->reconnect_delay_ms * 2 > 5000) ? 5000 : s->reconnect_delay_ms * 2;
                return ZST_TIMEOUT;
            }
        }
        s->connecting = true;
        return ZST_TIMEOUT;
    }

    // We are connecting
    SRT_SOCKSTATUS status = srt_getsockstate(s->conn_sock);
    if (status == SRTS_CONNECTED) {
        s->connecting = false;
        s->reconnect_delay_ms = 500;
        ZST_LOG_INFO("srtsink", "SRT connected successfully");
        return ZST_OK;
    } else if (status == SRTS_CONNECTING) {
        return ZST_TIMEOUT;
    } else {
        ZST_LOG_WARN("srtsink", "SRT connection failed, socket status: %d", status);
        srt_close(s->conn_sock);
        s->conn_sock = SRT_INVALID_SOCK;
        s->connecting = false;
        s->next_reconnect_time_ms = now + s->reconnect_delay_ms;
        s->reconnect_delay_ms = (s->reconnect_delay_ms * 2 > 5000) ? 5000 : s->reconnect_delay_ms * 2;
        return ZST_TIMEOUT;
    }
}

static zst_result_t
srt_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    srt_sink_t* s = el->priv;

    if (!in) {
        return ZST_ERROR;
    }

    // Graceful EOS handling
    if ((in->memory.size == 0 && in->memory.data == NULL) || (in->flags & ZST_BUFFER_FLAG_EOS)) {
        ZST_LOG_INFO("srtsink", "Received EOS buffer. Closing connection.");
        if (s->conn_sock != SRT_INVALID_SOCK) {
            srt_close(s->conn_sock);
            s->conn_sock = SRT_INVALID_SOCK;
        }
        return ZST_OK;
    }

    zst_result_t conn_res = srt_sink_ensure_connection(el, s);
    if (conn_res != ZST_OK) {
        return conn_res;
    }

    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    if (in->pts != 0) {
        if (s->base_pts == 0) {
            s->base_pts = in->pts;
            s->base_srt_time = srt_sink_time_now_us();
        }
        ctrl.srctime = s->base_srt_time + (in->pts - s->base_pts) / 1000ULL;
    }

    int n = srt_sendmsg2(s->conn_sock, (const char*)in->memory.data, (int)in->memory.size, &ctrl);
    if (n > 0 && (size_t)n == in->memory.size) {
        return ZST_OK;
    }

    if (n > 0 && (size_t)n < in->memory.size) {
        ZST_LOG_WARN("srtsink", "Partial write: %d/%zu bytes", n, in->memory.size);
        return ZST_TIMEOUT;
    }

    int err = srt_getlasterror(NULL);
    if (err == SRT_EASYNCSND) {
        return ZST_TIMEOUT;
    }

    ZST_LOG_WARN("srtsink", "srt_sendmsg2 error: %s (error code: %d)", srt_getlasterror_str(), err);
    srt_close(s->conn_sock);
    s->conn_sock = SRT_INVALID_SOCK;
    s->base_srt_time = 0;

    return ZST_TIMEOUT;
}

static zst_caps_t*
srt_sink_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_struct_t* caps_mp2t = calloc(1, sizeof(*caps_mp2t));
    if (caps_mp2t) {
        strncpy(caps_mp2t->media_type, "video/mp2t", sizeof(caps_mp2t->media_type) - 1);
        caps_mp2t->type = ZST_CAPS_ANY;
        caps_mp2t->next = NULL;
        zst_caps_append(caps, caps_mp2t);
    }

    zst_caps_struct_t* caps_raw = calloc(1, sizeof(*caps_raw));
    if (caps_raw) {
        strncpy(caps_raw->media_type, "application/octet-stream", sizeof(caps_raw->media_type) - 1);
        caps_raw->type = ZST_CAPS_ANY;
        caps_raw->next = NULL;
        zst_caps_append(caps, caps_raw);
    }

    return caps;
}

static zst_result_t
srt_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    srt_sink_t* s = el->priv;
    if (strcmp(name, "uri") == 0) {
        strncpy(s->uri, value, sizeof(s->uri) - 1);
        s->uri[sizeof(s->uri) - 1] = '\0';
        srt_parse_uri_ext(s->uri, s->host, sizeof(s->host), &s->port,
                          s->mode, sizeof(s->mode), &s->latency,
                          s->passphrase, sizeof(s->passphrase), &s->pbkeylen,
                          s->streamid, sizeof(s->streamid), &s->payload_size,
                          &s->tlpktdrop, &s->maxbw, &s->rcvbuf, &s->sndbuf);
        return ZST_OK;
    }
    if (strcmp(name, "host") == 0) {
        strncpy(s->host, value, sizeof(s->host) - 1);
        s->host[sizeof(s->host) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "port") == 0) {
        s->port = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "mode") == 0) {
        strncpy(s->mode, value, sizeof(s->mode) - 1);
        s->mode[sizeof(s->mode) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "latency") == 0) {
        s->latency = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "passphrase") == 0) {
        strncpy(s->passphrase, value, sizeof(s->passphrase) - 1);
        s->passphrase[sizeof(s->passphrase) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "pbkeylen") == 0) {
        s->pbkeylen = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "streamid") == 0) {
        strncpy(s->streamid, value, sizeof(s->streamid) - 1);
        s->streamid[sizeof(s->streamid) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "payload-size") == 0) {
        s->payload_size = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "tlpktdrop") == 0) {
        s->tlpktdrop = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "maxbw") == 0) {
        s->maxbw = atoll(value);
        return ZST_OK;
    }
    if (strcmp(name, "rcvbuf") == 0) {
        s->rcvbuf = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "sndbuf") == 0) {
        s->sndbuf = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
srt_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    srt_sink_t* s = el->priv;
    if (strcmp(name, "uri") == 0) {
        strncpy(value_out, s->uri, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "host") == 0) {
        strncpy(value_out, s->host, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "port") == 0) {
        snprintf(value_out, max_len, "%d", s->port);
        return ZST_OK;
    }
    if (strcmp(name, "mode") == 0) {
        strncpy(value_out, s->mode, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "latency") == 0) {
        snprintf(value_out, max_len, "%d", s->latency);
        return ZST_OK;
    }
    if (strcmp(name, "passphrase") == 0) {
        strncpy(value_out, s->passphrase, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "pbkeylen") == 0) {
        snprintf(value_out, max_len, "%d", s->pbkeylen);
        return ZST_OK;
    }
    if (strcmp(name, "streamid") == 0) {
        strncpy(value_out, s->streamid, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "payload-size") == 0) {
        snprintf(value_out, max_len, "%d", s->payload_size);
        return ZST_OK;
    }
    if (strcmp(name, "tlpktdrop") == 0) {
        strncpy(value_out, s->tlpktdrop ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "maxbw") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->maxbw);
        return ZST_OK;
    }
    if (strcmp(name, "rcvbuf") == 0) {
        snprintf(value_out, max_len, "%d", s->rcvbuf);
        return ZST_OK;
    }
    if (strcmp(name, "sndbuf") == 0) {
        snprintf(value_out, max_len, "%d", s->sndbuf);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name = "srtsink",
    .open = srt_sink_open,
    .close = srt_sink_close,
    .start = srt_sink_start,
    .stop = srt_sink_stop,
    .process = srt_sink_process,
    .get_caps = srt_sink_get_caps,
    .set_property = srt_sink_set_property,
    .get_property = srt_sink_get_property,
    .get_pool = NULL
};

zst_element_t*
zst_srt_sink_create(void)
{
    zst_element_t* el;
    srt_sink_t* priv;
    zst_pad_t* sink;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->port = 9000;
    strcpy(priv->host, "127.0.0.1");
    strcpy(priv->mode, "caller");
    priv->latency = 120;
    priv->pbkeylen = 16;
    priv->payload_size = 1316;
    priv->tlpktdrop = true;
    priv->maxbw = -1;
    priv->rcvbuf = 0;
    priv->sndbuf = 0;
    priv->listen_sock = SRT_INVALID_SOCK;
    priv->conn_sock = SRT_INVALID_SOCK;
    priv->connecting = false;

    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    return el;
}
