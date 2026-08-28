/*=============================================================================
    zst_dante_udp_source.h - Dante IPv4 UDP media source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_DANTE_UDP_SOURCE_FACTORY "danteudpsrc"

#define ZST_DANTE_UDP_SOURCE_PROP_LOCAL_ADDRESS "local-address"
#define ZST_DANTE_UDP_SOURCE_PROP_PORT "port"
#define ZST_DANTE_UDP_SOURCE_PROP_MULTICAST_ADDRESS "multicast-address"
#define ZST_DANTE_UDP_SOURCE_PROP_MULTICAST_INTERFACE_ADDRESS "multicast-interface-address"
#define ZST_DANTE_UDP_SOURCE_PROP_TRANSMITTER_ADDRESS "transmitter-address"
#define ZST_DANTE_UDP_SOURCE_PROP_READ_TIMEOUT_MS "read-timeout-ms"
#define ZST_DANTE_UDP_SOURCE_PROP_MAX_DATAGRAM_SIZE "max-datagram-size"
#define ZST_DANTE_UDP_SOURCE_PROP_PACKETS_RECEIVED "packets-received"
#define ZST_DANTE_UDP_SOURCE_PROP_BYTES_RECEIVED "bytes-received"
#define ZST_DANTE_UDP_SOURCE_PROP_PACKETS_REJECTED "packets-rejected"
#define ZST_DANTE_UDP_SOURCE_PROP_PACKETS_TRUNCATED "packets-truncated"
#define ZST_DANTE_UDP_SOURCE_PROP_LAST_PACKET_ADDRESS "last-packet-address"
#define ZST_DANTE_UDP_SOURCE_PROP_LAST_PACKET_PORT "last-packet-port"
#define ZST_DANTE_UDP_SOURCE_PROP_LAST_PACKET_SIZE "last-packet-size"
#define ZST_DANTE_UDP_SOURCE_PROP_LAST_PACKET_TIME_NS "last-packet-time-ns"

#define ZST_DANTE_UDP_SOURCE_PAD_SRC "src"

zst_element_t* zst_dante_udp_source_create(void);

/* Monotonic CLOCK_MONOTONIC timestamp of the most recently accepted packet.
 * Returns zero before the first accepted packet or after the source is opened. */
uint64_t zst_dante_udp_source_get_last_packet_time_ns(zst_element_t* source);

#ifdef __cplusplus
}
#endif
