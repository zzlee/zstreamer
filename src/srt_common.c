#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <srt/srt.h>
#include "zst_log.h"
#include "srt_common.h"

pthread_mutex_t g_srt_init_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_srt_init_count = 0;

void srt_global_init(void) {
    pthread_mutex_lock(&g_srt_init_mutex);
    if (g_srt_init_count == 0) {
        srt_startup();
    }
    g_srt_init_count++;
    pthread_mutex_unlock(&g_srt_init_mutex);
}

void srt_global_cleanup(void) {
    pthread_mutex_lock(&g_srt_init_mutex);
    g_srt_init_count--;
    // srt_cleanup() is intentionally omitted to prevent AddressSanitizer/TSD crashes
    // on thread exit and glibc double-free/corruption crashes.
    pthread_mutex_unlock(&g_srt_init_mutex);
}

void srt_parse_uri_ext(const char* uri, char* host, size_t host_len, int* port,
                       char* mode, size_t mode_len, int* latency,
                       char* passphrase, size_t passphrase_len, int* pbkeylen,
                       char* streamid, size_t streamid_len, int* payload_size,
                       bool* tlpktdrop, int64_t* maxbw, int* rcvbuf, int* sndbuf,
                       int* peeridle, int* conntimeo, int* oheadbw,
                       int* ipttl, int* iptos, int* fc,
                       int* lossmaxttl, int64_t* mininputbw, int* snddropdelay,
                       char* bindtodevice, size_t bindtodevice_len,
                       char* congestion, size_t congestion_len,
                       char* packetfilter, size_t packetfilter_len,
                       int* sndtimeo, int* rcvtimeo, int* ipv6only) {
    if (!uri || strncmp(uri, "srt://", 6) != 0) return;
    const char* p = uri + 6;
    const char* col = strchr(p, ':');
    const char* q = strchr(p, '?');

    if (col && (!q || col < q)) {
        size_t len = col - p;
        if (len >= host_len) len = host_len - 1;
        strncpy(host, p, len);
        host[len] = '\0';
        p = col + 1;
        q = strchr(p, '?');
        if (q) {
            if (port) *port = atoi(p);
            p = q + 1;
        } else {
            if (port) *port = atoi(p);
            return;
        }
    } else if (q) {
        size_t len = q - p;
        if (len >= host_len) len = host_len - 1;
        strncpy(host, p, len);
        host[len] = '\0';
        p = q + 1;
    } else {
        strncpy(host, p, host_len - 1);
        host[host_len - 1] = '\0';
        return;
    }

    while (p && *p) {
        const char* next = strchr(p, '&');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        char pair[256];
        if (len >= sizeof(pair)) len = sizeof(pair) - 1;
        strncpy(pair, p, len);
        pair[len] = '\0';

        char* eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char* key = pair;
            char* val = eq + 1;
            if (strcmp(key, "mode") == 0 && mode) {
                strncpy(mode, val, mode_len - 1);
                mode[mode_len - 1] = '\0';
            } else if (strcmp(key, "latency") == 0 && latency) {
                *latency = atoi(val);
            } else if (strcmp(key, "passphrase") == 0 && passphrase) {
                strncpy(passphrase, val, passphrase_len - 1);
                passphrase[passphrase_len - 1] = '\0';
            } else if (strcmp(key, "pbkeylen") == 0 && pbkeylen) {
                *pbkeylen = atoi(val);
            } else if (strcmp(key, "streamid") == 0 && streamid) {
                strncpy(streamid, val, streamid_len - 1);
                streamid[streamid_len - 1] = '\0';
            } else if ((strcmp(key, "payloadsize") == 0 || strcmp(key, "pkt_size") == 0) && payload_size) {
                *payload_size = atoi(val);
            } else if (strcmp(key, "tlpktdrop") == 0 && tlpktdrop) {
                *tlpktdrop = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0);
            } else if (strcmp(key, "maxbw") == 0 && maxbw) {
                *maxbw = atoll(val);
            } else if (strcmp(key, "rcvbuf") == 0 && rcvbuf) {
                *rcvbuf = atoi(val);
            } else if (strcmp(key, "sndbuf") == 0 && sndbuf) {
                *sndbuf = atoi(val);
            } else if (strcmp(key, "peeridle") == 0 && peeridle) {
                *peeridle = atoi(val);
            } else if (strcmp(key, "conntimeo") == 0 && conntimeo) {
                *conntimeo = atoi(val);
            } else if (strcmp(key, "oheadbw") == 0 && oheadbw) {
                *oheadbw = atoi(val);
            } else if (strcmp(key, "ipttl") == 0 && ipttl) {
                *ipttl = atoi(val);
            } else if (strcmp(key, "iptos") == 0 && iptos) {
                *iptos = atoi(val);
            } else if (strcmp(key, "fc") == 0 && fc) {
                *fc = atoi(val);
            } else if (strcmp(key, "lossmaxttl") == 0 && lossmaxttl) {
                *lossmaxttl = atoi(val);
            } else if (strcmp(key, "mininputbw") == 0 && mininputbw) {
                *mininputbw = atoll(val);
            } else if (strcmp(key, "snddropdelay") == 0 && snddropdelay) {
                *snddropdelay = atoi(val);
            } else if (strcmp(key, "bindtodevice") == 0 && bindtodevice) {
                strncpy(bindtodevice, val, bindtodevice_len - 1);
                bindtodevice[bindtodevice_len - 1] = '\0';
            } else if (strcmp(key, "congestion") == 0 && congestion) {
                strncpy(congestion, val, congestion_len - 1);
                congestion[congestion_len - 1] = '\0';
            } else if (strcmp(key, "packetfilter") == 0 && packetfilter) {
                strncpy(packetfilter, val, packetfilter_len - 1);
                packetfilter[packetfilter_len - 1] = '\0';
            } else if (strcmp(key, "sndtimeo") == 0 && sndtimeo) {
                *sndtimeo = atoi(val);
            } else if (strcmp(key, "rcvtimeo") == 0 && rcvtimeo) {
                *rcvtimeo = atoi(val);
            } else if (strcmp(key, "ipv6only") == 0 && ipv6only) {
                *ipv6only = atoi(val);
            }
        }
        p = next ? next + 1 : NULL;
    }
}

void srt_parse_uri(const char* uri, char* host, size_t host_len, int* port,
                   char* mode, size_t mode_len, int* latency,
                   char* passphrase, size_t passphrase_len, int* pbkeylen,
                   char* streamid, size_t streamid_len, int* payload_size) {
    srt_parse_uri_ext(uri, host, host_len, port, mode, mode_len, latency,
                      passphrase, passphrase_len, pbkeylen, streamid, streamid_len, payload_size,
                      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);
}
