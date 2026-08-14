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
    int mss;
    int fc;
    int sndtimeo;
    int rcvtimeo;
    int tsbpdmode;
    int rcvlatency;
    int peerlatency;
    int inputbw;
    int oheadbw;
    int ipttl;
    int iptos;
    int snddropdelay;
    int nakreport;
    int conntimeo;
    int drifttracer;
    int mininputbw;
    int lossmaxttl;
    char congestion[128];
    int messageapi;
    int kmrefreshrate;
    int kmpreannounce;
    int enforcedencryption;
    int ipv6only;
    int peeridletimeo;
    char bindtodevice[128];
    char packetfilter[128];
    int retransmitalgo;
    int cryptomode;
    int64_t maxrexmitbw;
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

    if (s->payload_size > 0) {
        int payload_size = s->payload_size;
        srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload_size, sizeof(payload_size));
    }

    if (s->mss != -1) {
        int val = s->mss;
        srt_setsockopt(sock, 0, SRTO_MSS, &val, sizeof(val));
    }
    if (s->fc != -1) {
        int val = s->fc;
        srt_setsockopt(sock, 0, SRTO_FC, &val, sizeof(val));
    }
    if (s->sndtimeo != -1) {
        int val = s->sndtimeo;
        srt_setsockopt(sock, 0, SRTO_SNDTIMEO, &val, sizeof(val));
    }
    if (s->rcvtimeo != -1) {
        int val = s->rcvtimeo;
        srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &val, sizeof(val));
    }
    if (s->tsbpdmode != -1) {
        int val = s->tsbpdmode;
        srt_setsockopt(sock, 0, SRTO_TSBPDMODE, &val, sizeof(val));
    }
    if (s->rcvlatency != -1) {
        int val = s->rcvlatency;
        srt_setsockopt(sock, 0, SRTO_RCVLATENCY, &val, sizeof(val));
    }
    if (s->peerlatency != -1) {
        int val = s->peerlatency;
        srt_setsockopt(sock, 0, SRTO_PEERLATENCY, &val, sizeof(val));
    }
    if (s->inputbw != -1) {
        int val = s->inputbw;
        srt_setsockopt(sock, 0, SRTO_INPUTBW, &val, sizeof(val));
    }
    if (s->oheadbw != -1) {
        int val = s->oheadbw;
        srt_setsockopt(sock, 0, SRTO_OHEADBW, &val, sizeof(val));
    }
    if (s->ipttl != -1) {
        int val = s->ipttl;
        srt_setsockopt(sock, 0, SRTO_IPTTL, &val, sizeof(val));
    }
    if (s->iptos != -1) {
        int val = s->iptos;
        srt_setsockopt(sock, 0, SRTO_IPTOS, &val, sizeof(val));
    }
    if (s->snddropdelay != -2) {
        int val = s->snddropdelay;
        srt_setsockopt(sock, 0, SRTO_SNDDROPDELAY, &val, sizeof(val));
    }
    if (s->nakreport != -1) {
        int val = s->nakreport;
        srt_setsockopt(sock, 0, SRTO_NAKREPORT, &val, sizeof(val));
    }
    if (s->conntimeo != -1) {
        int val = s->conntimeo;
        srt_setsockopt(sock, 0, SRTO_CONNTIMEO, &val, sizeof(val));
    }
    if (s->drifttracer != -1) {
        int val = s->drifttracer;
        srt_setsockopt(sock, 0, SRTO_DRIFTTRACER, &val, sizeof(val));
    }
    if (s->mininputbw != -1) {
        int val = s->mininputbw;
        srt_setsockopt(sock, 0, SRTO_MININPUTBW, &val, sizeof(val));
    }
    if (s->lossmaxttl != -1) {
        int val = s->lossmaxttl;
        srt_setsockopt(sock, 0, SRTO_LOSSMAXTTL, &val, sizeof(val));
    }
    if (strlen(s->congestion) > 0) {
        srt_setsockopt(sock, 0, SRTO_CONGESTION, s->congestion, strlen(s->congestion));
    }
    if (s->messageapi != -1) {
        int val = s->messageapi;
        srt_setsockopt(sock, 0, SRTO_MESSAGEAPI, &val, sizeof(val));
    }
    if (s->kmrefreshrate != -1) {
        int val = s->kmrefreshrate;
        srt_setsockopt(sock, 0, SRTO_KMREFRESHRATE, &val, sizeof(val));
    }
    if (s->kmpreannounce != -1) {
        int val = s->kmpreannounce;
        srt_setsockopt(sock, 0, SRTO_KMPREANNOUNCE, &val, sizeof(val));
    }
    if (s->enforcedencryption != -1) {
        int val = s->enforcedencryption;
        srt_setsockopt(sock, 0, SRTO_ENFORCEDENCRYPTION, &val, sizeof(val));
    }
    if (s->ipv6only != -1) {
        int val = s->ipv6only;
        srt_setsockopt(sock, 0, SRTO_IPV6ONLY, &val, sizeof(val));
    }
    if (s->peeridletimeo != -1) {
        int val = s->peeridletimeo;
        srt_setsockopt(sock, 0, SRTO_PEERIDLETIMEO, &val, sizeof(val));
    }
    if (strlen(s->bindtodevice) > 0) {
        srt_setsockopt(sock, 0, SRTO_BINDTODEVICE, s->bindtodevice, strlen(s->bindtodevice));
    }
    if (strlen(s->packetfilter) > 0) {
        srt_setsockopt(sock, 0, SRTO_PACKETFILTER, s->packetfilter, strlen(s->packetfilter));
    }
    if (s->retransmitalgo != -1) {
        int val = s->retransmitalgo;
        srt_setsockopt(sock, 0, SRTO_RETRANSMITALGO, &val, sizeof(val));
    }
    if (s->cryptomode != -1) {
        int val = s->cryptomode;
        srt_setsockopt(sock, 0, SRTO_CRYPTOMODE, &val, sizeof(val));
    }
    if (s->maxrexmitbw >= 0) {
        int64_t val = s->maxrexmitbw;
        srt_setsockopt(sock, 0, SRTO_MAXREXMITBW, &val, sizeof(val));
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

    if (strcmp(name, "mss") == 0) {
        if (value[0] == '\0') s->mss = -1;
        else s->mss = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "fc") == 0) {
        if (value[0] == '\0') s->fc = -1;
        else s->fc = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "sndtimeo") == 0) {
        if (value[0] == '\0') s->sndtimeo = -1;
        else s->sndtimeo = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "rcvtimeo") == 0) {
        if (value[0] == '\0') s->rcvtimeo = -1;
        else s->rcvtimeo = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "tsbpdmode") == 0) {
        if (value[0] == '\0') s->tsbpdmode = -1;
        else s->tsbpdmode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "rcvlatency") == 0) {
        if (value[0] == '\0') s->rcvlatency = -1;
        else s->rcvlatency = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "peerlatency") == 0) {
        if (value[0] == '\0') s->peerlatency = -1;
        else s->peerlatency = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "inputbw") == 0) {
        if (value[0] == '\0') s->inputbw = -1;
        else s->inputbw = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "oheadbw") == 0) {
        if (value[0] == '\0') s->oheadbw = -1;
        else s->oheadbw = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "ipttl") == 0) {
        if (value[0] == '\0') s->ipttl = -1;
        else s->ipttl = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "iptos") == 0) {
        if (value[0] == '\0') s->iptos = -1;
        else s->iptos = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "snddropdelay") == 0) {
        if (value[0] == '\0') s->snddropdelay = -2;
        else s->snddropdelay = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "nakreport") == 0) {
        if (value[0] == '\0') s->nakreport = -1;
        else s->nakreport = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "conntimeo") == 0) {
        if (value[0] == '\0') s->conntimeo = -1;
        else s->conntimeo = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "drifttracer") == 0) {
        if (value[0] == '\0') s->drifttracer = -1;
        else s->drifttracer = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "mininputbw") == 0) {
        if (value[0] == '\0') s->mininputbw = -1;
        else s->mininputbw = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "lossmaxttl") == 0) {
        if (value[0] == '\0') s->lossmaxttl = -1;
        else s->lossmaxttl = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "congestion") == 0) {
        strncpy(s->congestion, value, sizeof(s->congestion) - 1);
        s->congestion[sizeof(s->congestion) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "messageapi") == 0) {
        if (value[0] == '\0') s->messageapi = -1;
        else s->messageapi = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "kmrefreshrate") == 0) {
        if (value[0] == '\0') s->kmrefreshrate = -1;
        else s->kmrefreshrate = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "kmpreannounce") == 0) {
        if (value[0] == '\0') s->kmpreannounce = -1;
        else s->kmpreannounce = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "enforcedencryption") == 0) {
        if (value[0] == '\0') s->enforcedencryption = -1;
        else s->enforcedencryption = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "ipv6only") == 0) {
        if (value[0] == '\0') s->ipv6only = -1;
        else s->ipv6only = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) ? 1 : 0;
        return ZST_OK;
    }
    if (strcmp(name, "peeridletimeo") == 0) {
        if (value[0] == '\0') s->peeridletimeo = -1;
        else s->peeridletimeo = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "bindtodevice") == 0) {
        strncpy(s->bindtodevice, value, sizeof(s->bindtodevice) - 1);
        s->bindtodevice[sizeof(s->bindtodevice) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "packetfilter") == 0) {
        strncpy(s->packetfilter, value, sizeof(s->packetfilter) - 1);
        s->packetfilter[sizeof(s->packetfilter) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "retransmitalgo") == 0) {
        if (value[0] == '\0') s->retransmitalgo = -1;
        else s->retransmitalgo = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "cryptomode") == 0) {
        if (value[0] == '\0') s->cryptomode = -1;
        else s->cryptomode = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "maxrexmitbw") == 0) {
        if (value[0] == '\0') s->maxrexmitbw = -1;
        else s->maxrexmitbw = atoll(value);
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

    if (strcmp(name, "mss") == 0) {
        if (s->mss == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->mss);
        return ZST_OK;
    }
    if (strcmp(name, "fc") == 0) {
        if (s->fc == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->fc);
        return ZST_OK;
    }
    if (strcmp(name, "sndtimeo") == 0) {
        if (s->sndtimeo == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->sndtimeo);
        return ZST_OK;
    }
    if (strcmp(name, "rcvtimeo") == 0) {
        if (s->rcvtimeo == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->rcvtimeo);
        return ZST_OK;
    }
    if (strcmp(name, "tsbpdmode") == 0) {
        if (s->tsbpdmode == -1) value_out[0] = '\0';
        else strncpy(value_out, s->tsbpdmode ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "rcvlatency") == 0) {
        if (s->rcvlatency == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->rcvlatency);
        return ZST_OK;
    }
    if (strcmp(name, "peerlatency") == 0) {
        if (s->peerlatency == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->peerlatency);
        return ZST_OK;
    }
    if (strcmp(name, "inputbw") == 0) {
        if (s->inputbw == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->inputbw);
        return ZST_OK;
    }
    if (strcmp(name, "oheadbw") == 0) {
        if (s->oheadbw == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->oheadbw);
        return ZST_OK;
    }
    if (strcmp(name, "ipttl") == 0) {
        if (s->ipttl == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->ipttl);
        return ZST_OK;
    }
    if (strcmp(name, "iptos") == 0) {
        if (s->iptos == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->iptos);
        return ZST_OK;
    }
    if (strcmp(name, "snddropdelay") == 0) {
        if (s->snddropdelay == -2) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->snddropdelay);
        return ZST_OK;
    }
    if (strcmp(name, "nakreport") == 0) {
        if (s->nakreport == -1) value_out[0] = '\0';
        else strncpy(value_out, s->nakreport ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "conntimeo") == 0) {
        if (s->conntimeo == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->conntimeo);
        return ZST_OK;
    }
    if (strcmp(name, "drifttracer") == 0) {
        if (s->drifttracer == -1) value_out[0] = '\0';
        else strncpy(value_out, s->drifttracer ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "mininputbw") == 0) {
        if (s->mininputbw == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->mininputbw);
        return ZST_OK;
    }
    if (strcmp(name, "lossmaxttl") == 0) {
        if (s->lossmaxttl == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->lossmaxttl);
        return ZST_OK;
    }
    if (strcmp(name, "congestion") == 0) {
        strncpy(value_out, s->congestion, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "messageapi") == 0) {
        if (s->messageapi == -1) value_out[0] = '\0';
        else strncpy(value_out, s->messageapi ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "kmrefreshrate") == 0) {
        if (s->kmrefreshrate == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->kmrefreshrate);
        return ZST_OK;
    }
    if (strcmp(name, "kmpreannounce") == 0) {
        if (s->kmpreannounce == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->kmpreannounce);
        return ZST_OK;
    }
    if (strcmp(name, "enforcedencryption") == 0) {
        if (s->enforcedencryption == -1) value_out[0] = '\0';
        else strncpy(value_out, s->enforcedencryption ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "ipv6only") == 0) {
        if (s->ipv6only == -1) value_out[0] = '\0';
        else strncpy(value_out, s->ipv6only ? "true" : "false", max_len);
        return ZST_OK;
    }
    if (strcmp(name, "peeridletimeo") == 0) {
        if (s->peeridletimeo == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->peeridletimeo);
        return ZST_OK;
    }
    if (strcmp(name, "bindtodevice") == 0) {
        strncpy(value_out, s->bindtodevice, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "packetfilter") == 0) {
        strncpy(value_out, s->packetfilter, max_len);
        return ZST_OK;
    }
    if (strcmp(name, "retransmitalgo") == 0) {
        if (s->retransmitalgo == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->retransmitalgo);
        return ZST_OK;
    }
    if (strcmp(name, "cryptomode") == 0) {
        if (s->cryptomode == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%d", s->cryptomode);
        return ZST_OK;
    }
    if (strcmp(name, "maxrexmitbw") == 0) {
        if (s->maxrexmitbw == -1) value_out[0] = '\0';
        else snprintf(value_out, max_len, "%lld", (long long)s->maxrexmitbw);
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

    priv->mss = -1;
    priv->fc = -1;
    priv->sndtimeo = -1;
    priv->rcvtimeo = -1;
    priv->tsbpdmode = -1;
    priv->rcvlatency = -1;
    priv->peerlatency = -1;
    priv->inputbw = -1;
    priv->oheadbw = -1;
    priv->ipttl = -1;
    priv->iptos = -1;
    priv->snddropdelay = -2;
    priv->nakreport = -1;
    priv->conntimeo = -1;
    priv->drifttracer = -1;
    priv->mininputbw = -1;
    priv->lossmaxttl = -1;
    priv->messageapi = -1;
    priv->kmrefreshrate = -1;
    priv->kmpreannounce = -1;
    priv->enforcedencryption = -1;
    priv->ipv6only = -1;
    priv->peeridletimeo = -1;
    priv->retransmitalgo = -1;
    priv->cryptomode = -1;
    priv->maxrexmitbw = -1;

    el = zst_element_create(&g_ops, priv);
    sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    return el;
}
