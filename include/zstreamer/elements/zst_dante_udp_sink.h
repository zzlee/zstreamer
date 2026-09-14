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
#define ZST_DANTE_UDP_SINK_PROP_TIMING_OBSERVE "timing-observe"
#define ZST_DANTE_UDP_SINK_PROP_PACKETS_SENT "packets-sent"
#define ZST_DANTE_UDP_SINK_PROP_BYTES_SENT "bytes-sent"
#define ZST_DANTE_UDP_SINK_PROP_SEND_ERRORS "send-errors"
#define ZST_DANTE_UDP_SINK_PROP_SEND_ERRORS_EAGAIN "send-errors-eagain"
#define ZST_DANTE_UDP_SINK_PROP_SEND_ERRORS_EINTR "send-errors-eintr"
#define ZST_DANTE_UDP_SINK_PROP_SEND_ERRORS_OTHER "send-errors-other"
#define ZST_DANTE_UDP_SINK_PROP_SEND_LAST_ERRNO "send-last-errno"
#define ZST_DANTE_UDP_SINK_PROP_SEND_BUFFER_SIZE "send-buffer-size"
#define ZST_DANTE_UDP_SINK_PROP_LAST_PACKET_SIZE "last-packet-size"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_COUNT "send-gap-count"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_TOTAL_NS "send-gap-total-ns"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_MIN_NS "send-gap-min-ns"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_MAX_NS "send-gap-max-ns"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_LE_5US "send-gap-le-5us"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_LE_20US "send-gap-le-20us"
#define ZST_DANTE_UDP_SINK_PROP_SEND_GAP_LE_100US "send-gap-le-100us"
#define ZST_DANTE_UDP_SINK_PROP_SEND_DURATION_TOTAL_NS "send-duration-total-ns"
#define ZST_DANTE_UDP_SINK_PROP_SEND_DURATION_MIN_NS "send-duration-min-ns"
#define ZST_DANTE_UDP_SINK_PROP_SEND_DURATION_MAX_NS "send-duration-max-ns"
#define ZST_DANTE_UDP_SINK_PROP_EAGAIN_LAST_GAP_NS "eagain-last-gap-ns"
#define ZST_DANTE_UDP_SINK_PROP_EAGAIN_LAST_BURST_PACKETS "eagain-last-burst-packets"
#define ZST_DANTE_UDP_SINK_PROP_EAGAIN_MAX_BURST_PACKETS "eagain-max-burst-packets"

#define ZST_DANTE_UDP_SINK_PAD_SINK "sink"

zst_element_t* zst_dante_udp_sink_create(void);

#ifdef __cplusplus
}
#endif
