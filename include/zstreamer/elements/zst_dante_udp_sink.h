/*=============================================================================
    zst_dante_udp_sink.h - Dante IPv4 UDP media sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_DANTE_UDP_SINK_FACTORY "danteudpsink"

#define ZST_DANTE_UDP_SINK_PROP_DESTINATION_ADDRESS "destination-address"
#define ZST_DANTE_UDP_SINK_PROP_PORT "port"
#define ZST_DANTE_UDP_SINK_PROP_TRANSMITTER_ADDRESS "transmitter-address"
#define ZST_DANTE_UDP_SINK_PROP_MULTICAST_INTERFACE_ADDRESS "multicast-interface-address"
#define ZST_DANTE_UDP_SINK_PROP_TTL "ttl"
#define ZST_DANTE_UDP_SINK_PROP_LOOP "loop"
#define ZST_DANTE_UDP_SINK_PROP_TIMESTAMP_PACING "timestamp-pacing"
#define ZST_DANTE_UDP_SINK_PROP_PACKETS_SENT "packets-sent"
#define ZST_DANTE_UDP_SINK_PROP_BYTES_SENT "bytes-sent"
#define ZST_DANTE_UDP_SINK_PROP_SEND_ERRORS "send-errors"
#define ZST_DANTE_UDP_SINK_PROP_LAST_PACKET_SIZE "last-packet-size"

#define ZST_DANTE_UDP_SINK_PAD_SINK "sink"

zst_element_t* zst_dante_udp_sink_create(void);

#ifdef __cplusplus
}
#endif
