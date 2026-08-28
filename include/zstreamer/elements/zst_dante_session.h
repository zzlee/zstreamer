#pragma once

#include "zst_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_DANTE_SESSION_FACTORY "dantesession"

#define ZST_DANTE_SESSION_DEFAULT_SOCKET_PATH "/var/run/dante/dvr"
#define ZST_DANTE_SESSION_MAX_RECORD_LIMIT 65536u

#define ZST_DANTE_SESSION_PROP_SOCKET_PATH            "socket-path"
#define ZST_DANTE_SESSION_PROP_TX_VIDEO_CHANNELS      "tx-video-channels"
#define ZST_DANTE_SESSION_PROP_RX_VIDEO_CHANNELS      "rx-video-channels"
#define ZST_DANTE_SESSION_PROP_RECONNECT              "reconnect"
#define ZST_DANTE_SESSION_PROP_RECONNECT_DELAY_MS     "reconnect-delay-ms"
#define ZST_DANTE_SESSION_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"
#define ZST_DANTE_SESSION_PROP_MAX_RECORD_SIZE        "max-record-size"
#define ZST_DANTE_SESSION_PROP_CONNECTED              "connected"
#define ZST_DANTE_SESSION_PROP_SESSION_STATE          "session-state"
#define ZST_DANTE_SESSION_PROP_ACTIVE_TX_FLOWS        "active-tx-flows"
#define ZST_DANTE_SESSION_PROP_ACTIVE_RX_FLOWS        "active-rx-flows"

typedef enum {
    ZST_DANTE_SESSION_STOPPED,
    ZST_DANTE_SESSION_CONNECTING,
    ZST_DANTE_SESSION_CONNECTED,
    ZST_DANTE_SESSION_RECONNECT_WAIT
} zst_dante_session_state_t;

zst_element_t* zst_dante_session_create(const char* socket_path);

zst_result_t zst_dante_session_report_rx_flow_status(
    zst_element_t* session,
    uint32_t flow_index,
    zst_dante_rx_flow_status_t status);

zst_result_t zst_dante_session_report_tx_channel_status(
    zst_element_t* session,
    uint32_t channel_index,
    zst_dante_tx_channel_status_t status);

#ifdef __cplusplus
}
#endif
