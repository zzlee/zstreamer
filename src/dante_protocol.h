#pragma once

#include "zst_bus.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DANTE_ACTION_REQUEST_CONFIGURATION,
    DANTE_ACTION_CREATE_FLOW,
    DANTE_ACTION_DELETE_FLOW
} dante_action_type_t;

typedef struct {
    dante_action_type_t type;
    zst_dante_flow_t flow;
    zst_dante_flow_direction_t delete_direction;
    uint32_t delete_flow_index;
} dante_message_t;

typedef enum {
    DANTE_PROTOCOL_OK,
    DANTE_PROTOCOL_MALFORMED,
    DANTE_PROTOCOL_UNKNOWN_ACTION
} dante_protocol_result_t;

dante_protocol_result_t dante_protocol_parse_record(
    const void* data,
    size_t length,
    dante_message_t* message_out,
    char* error_out,
    size_t error_size);

void dante_protocol_message_clear(dante_message_t* message);

zst_result_t dante_protocol_encode_start(
    uint32_t tx_channels,
    uint32_t rx_channels,
    char** data_out,
    size_t* length_out);

zst_result_t dante_protocol_encode_stop(char** data_out, size_t* length_out);

zst_result_t dante_protocol_encode_rx_status(
    uint32_t flow_index,
    zst_dante_rx_flow_status_t status,
    char** data_out,
    size_t* length_out);

zst_result_t dante_protocol_encode_tx_status(
    uint32_t channel_index,
    zst_dante_tx_channel_status_t status,
    char** data_out,
    size_t* length_out);
