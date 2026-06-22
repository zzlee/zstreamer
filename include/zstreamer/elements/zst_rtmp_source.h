/*=============================================================================
    zst_rtmp_source.h — Rtmp Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTMP_SOURCE_FACTORY "rtmpsrc"
#define ZST_RTMP_SOURCE_PROP_URL "url"
#define ZST_RTMP_SOURCE_PROP_RTMP_URL "rtmp_url"
#define ZST_RTMP_SOURCE_PROP_LIVE "live"
#define ZST_RTMP_SOURCE_PROP_BUFFER_TIME "buffer-time"
#define ZST_RTMP_SOURCE_PROP_SWF_URL "swf-url"
#define ZST_RTMP_SOURCE_PROP_RECONNECT "reconnect"
#define ZST_RTMP_SOURCE_PROP_RECONNECT_DELAY_MS "reconnect-delay-ms"
#define ZST_RTMP_SOURCE_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"


typedef struct {
    size_t struct_size;
    const char* url;
    const char* rtmp_url;
    bool live;
    int buffer_time;
    const char* swf_url;
    bool reconnect;
    int reconnect_delay_ms;
    int max_reconnect_attempts;
} zst_rtmp_source_config_t;

zst_element_t* zst_rtmp_source_create(const char* url);
zst_element_t* zst_rtmp_source_create_with_config(const zst_rtmp_source_config_t* config);

#ifdef __cplusplus
}
#endif
