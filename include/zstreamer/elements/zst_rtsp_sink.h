/*=============================================================================
    zst_rtsp_sink.h — Rtsp Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SINK_FACTORY "rtspsink"

#define ZST_RTSP_SINK_PROP_URL "url"
#define ZST_RTSP_SINK_PROP_LISTEN_PORT "listen-port"
#define ZST_RTSP_SINK_PROP_MOUNT_POINT "mount-point"
#define ZST_RTSP_SINK_PROP_TRANSPORT "transport"
#define ZST_RTSP_SINK_PROP_MAX_CLIENTS "max-clients"
#define ZST_RTSP_SINK_PROP_RTCP_INTERVAL_MS "rtcp-interval-ms"


typedef struct {
    size_t struct_size;
    const char* url;
    int listen_port;
    const char* mount_point;
    const char* transport;
    int max_clients;
    int rtcp_interval_ms;
} zst_rtsp_sink_config_t;

zst_element_t* zst_rtsp_sink_create(void);
zst_element_t* zst_rtsp_sink_create_with_config(const zst_rtsp_sink_config_t* config);

#ifdef __cplusplus
}
#endif
