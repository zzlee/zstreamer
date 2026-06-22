/*=============================================================================
    zst_http_source.h — HTTP/HTTPS Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_HTTP_SOURCE_FACTORY "httpsrc"
#define ZST_HTTP_SOURCE_PROP_URL "url"
#define ZST_HTTP_SOURCE_PROP_URI "uri"
#define ZST_HTTP_SOURCE_PROP_USER_AGENT "user-agent"
#define ZST_HTTP_SOURCE_PROP_HEADERS "headers"
#define ZST_HTTP_SOURCE_PROP_TIMEOUT "timeout"
#define ZST_HTTP_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_HTTP_SOURCE_PROP_RECONNECT "reconnect"
#define ZST_HTTP_SOURCE_PROP_RECONNECT_DELAY "reconnect-delay-ms"
#define ZST_HTTP_SOURCE_PROP_MAX_RECONNECT "max-reconnect-attempts"


typedef struct {
    size_t struct_size;
    const char* url;
    const char* uri;
    const char* user_agent;
    const char* headers;
    int timeout;
    uint32_t chunk_size;
    bool reconnect;
    int reconnect_delay_ms;
    int max_reconnect_attempts;
} zst_http_source_config_t;

zst_element_t* zst_http_source_create(const char* url);
zst_element_t* zst_http_source_create_with_config(const zst_http_source_config_t* config);

#ifdef __cplusplus
}
#endif
