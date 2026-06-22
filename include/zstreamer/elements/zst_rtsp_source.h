/*=============================================================================
    zst_rtsp_source.h — Rtsp Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SOURCE_FACTORY "rtspsrc"

#define ZST_RTSP_SOURCE_PROP_URL "url"
#define ZST_RTSP_SOURCE_PROP_RTSP_URL "rtsp_url"
#define ZST_RTSP_SOURCE_PROP_USERNAME "username"
#define ZST_RTSP_SOURCE_PROP_PASSWORD "password"
#define ZST_RTSP_SOURCE_PROP_TRANSPORT "transport"
#define ZST_RTSP_SOURCE_PROP_BUFFER_SIZE "buffer-size"
#define ZST_RTSP_SOURCE_PROP_RECONNECT "reconnect"
#define ZST_RTSP_SOURCE_PROP_RECONNECT_DELAY_MS "reconnect-delay-ms"
#define ZST_RTSP_SOURCE_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"
#define ZST_RTSP_SOURCE_PROP_KEEPALIVE_INTERVAL_SEC "keepalive-interval-sec"


typedef struct {
    size_t struct_size;
    const char* url;
    const char* rtsp_url;
    const char* username;
    const char* password;
    const char* transport;
    int buffer_size;
    bool reconnect;
    int reconnect_delay_ms;
    int max_reconnect_attempts;
    int keepalive_interval_sec;
} zst_rtsp_source_config_t;

zst_element_t* zst_rtsp_source_create(const char* url);
zst_element_t* zst_rtsp_source_create_with_config(const zst_rtsp_source_config_t* config);

#ifdef __cplusplus
}
#endif
