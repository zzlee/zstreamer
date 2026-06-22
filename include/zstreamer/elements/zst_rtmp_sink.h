/*=============================================================================
    zst_rtmp_sink.h — Rtmp Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTMP_SINK_FACTORY "rtmpsink"
#define ZST_RTMP_SINK_PROP_URL "url"


typedef struct {
    size_t struct_size;
    const char* url;
} zst_rtmp_sink_config_t;

zst_element_t* zst_rtmp_sink_create(void);
zst_element_t* zst_rtmp_sink_create_with_config(const zst_rtmp_sink_config_t* config);

#ifdef __cplusplus
}
#endif
