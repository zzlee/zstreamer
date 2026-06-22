/*=============================================================================
    zst_net_sink.h — Net Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NET_SINK_FACTORY "netsink"

#define ZST_NET_SINK_PROP_HOST "host"
#define ZST_NET_SINK_PROP_PORT "port"
#define ZST_NET_SINK_PROP_PROTOCOL "protocol"
#define ZST_NET_SINK_PROP_PATH "path"
#define ZST_NET_SINK_PROP_WRITE_TIMEOUT "write-timeout"

#define ZST_NET_SINK_PROTOCOL_TCP_CLIENT "tcp-client"
#define ZST_NET_SINK_PROTOCOL_TCP_SERVER "tcp-server"
#define ZST_NET_SINK_PROTOCOL_UNIX_CLIENT "unix-client"
#define ZST_NET_SINK_PROTOCOL_UNIX_SERVER "unix-server"

#define ZST_NET_SINK_PAD_SINK "sink"


typedef struct {
    size_t struct_size;
    const char* host;
    int port;
    const char* protocol;
    const char* path;
    int write_timeout;
} zst_net_sink_config_t;

zst_element_t* zst_net_sink_create(void);
zst_element_t* zst_net_sink_create_with_config(const zst_net_sink_config_t* config);

#ifdef __cplusplus
}
#endif
