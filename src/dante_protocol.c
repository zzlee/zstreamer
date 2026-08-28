#define _POSIX_C_SOURCE 200809L

#include "dante_protocol.h"
#include <arpa/inet.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_error(char* out, size_t size, const char* format, ...)
{
    if (!out || size == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

static int
valid_utf8(const unsigned char* data, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char c = data[i++];
        if (c < 0x80) continue;
        unsigned int value;
        size_t continuation;
        if (c >= 0xc2 && c <= 0xdf) {
            value = c & 0x1f;
            continuation = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            value = c & 0x0f;
            continuation = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            value = c & 0x07;
            continuation = 3;
        } else {
            return 0;
        }
        if (continuation > length - i) return 0;
        for (size_t n = 0; n < continuation; n++) {
            unsigned char next = data[i++];
            if ((next & 0xc0) != 0x80) return 0;
            value = (value << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && value < 0x800) ||
            (continuation == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return 0;
        }
    }
    return 1;
}

static int
object_has_exact_keys(struct json_object* object, const char* const* keys, size_t count)
{
    if (!object || json_object_get_type(object) != json_type_object ||
        (size_t)json_object_object_length(object) != count) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        struct json_object* ignored;
        if (!json_object_object_get_ex(object, keys[i], &ignored)) return 0;
    }
    return 1;
}

static int
get_uint32(struct json_object* object, const char* key, uint32_t* out)
{
    struct json_object* value;
    if (!json_object_object_get_ex(object, key, &value) ||
        json_object_get_type(value) != json_type_int) {
        return 0;
    }
    int64_t number = json_object_get_int64(value);
    if (number < 0 || (uint64_t)number > UINT32_MAX) return 0;
    *out = (uint32_t)number;
    return 1;
}

static int
get_ipv4(struct json_object* object, const char* key, int multicast, char** out)
{
    struct json_object* value;
    struct in_addr address;
    if (!json_object_object_get_ex(object, key, &value) ||
        json_object_get_type(value) != json_type_string) {
        return 0;
    }
    const char* text = json_object_get_string(value);
    if (!text || inet_pton(AF_INET, text, &address) != 1) return 0;
    uint32_t host = ntohl(address.s_addr);
    int is_multicast = (host & 0xf0000000u) == 0xe0000000u;
    if (multicast != is_multicast) return 0;
    if (!multicast && (host == 0 || host == UINT32_MAX)) return 0;
    *out = strdup(text);
    return *out != NULL;
}

static int
parse_create(struct json_object* parameters, const char* action,
             dante_message_t* message, char* error, size_t error_size)
{
    int tx = strstr(action, "TxFlow") != NULL;
    int multicast = strstr(action, "Multicast") != NULL;
    const char* const unicast_keys[] = {
        "flowIndex", "channelIndex", "port", "receiverAddress", "transmitterAddress"
    };
    const char* const multicast_keys[] = {
        "flowIndex", "channelIndex", "port", "multicastAddress", "transmitterAddress"
    };
    const char* const* keys = multicast ? multicast_keys : unicast_keys;
    if (!object_has_exact_keys(parameters, keys, 5)) {
        set_error(error, error_size, "%s parameters have an invalid shape", action);
        return 0;
    }

    uint32_t port;
    message->type = DANTE_ACTION_CREATE_FLOW;
    message->flow.direction = tx ? ZST_DANTE_FLOW_TX : ZST_DANTE_FLOW_RX;
    message->flow.transport = multicast ? ZST_DANTE_FLOW_MULTICAST : ZST_DANTE_FLOW_UNICAST;
    if (!get_uint32(parameters, "flowIndex", &message->flow.flow_index) ||
        !get_uint32(parameters, "channelIndex", &message->flow.channel_index) ||
        !get_uint32(parameters, "port", &port) || port == 0 || port > UINT16_MAX ||
        !get_ipv4(parameters, "transmitterAddress", 0, &message->flow.transmitter_address) ||
        !(multicast
              ? get_ipv4(parameters, "multicastAddress", 1, &message->flow.multicast_address)
              : get_ipv4(parameters, "receiverAddress", 0, &message->flow.receiver_address))) {
        set_error(error, error_size, "%s parameters have invalid types, ranges, or addresses", action);
        return 0;
    }
    message->flow.port = (uint16_t)port;
    return 1;
}

static int
parse_delete(struct json_object* parameters, const char* action,
             dante_message_t* message, char* error, size_t error_size)
{
    const char* const keys[] = { "flowIndex" };
    if (!object_has_exact_keys(parameters, keys, 1) ||
        !get_uint32(parameters, "flowIndex", &message->delete_flow_index)) {
        set_error(error, error_size, "%s parameters have an invalid shape or flowIndex", action);
        return 0;
    }
    message->type = DANTE_ACTION_DELETE_FLOW;
    message->delete_direction = strcmp(action, "deleteTxFlow") == 0
        ? ZST_DANTE_FLOW_TX : ZST_DANTE_FLOW_RX;
    return 1;
}

dante_protocol_result_t
dante_protocol_parse_record(const void* data, size_t length,
                            dante_message_t* message_out,
                            char* error_out, size_t error_size)
{
    const char* const envelope_keys[] = { "action", "parameters" };
    const char* const no_keys[] = { NULL };
    struct json_tokener* tokener = NULL;
    struct json_object* root = NULL;
    struct json_object* action_object;
    struct json_object* parameters;
    dante_protocol_result_t result = DANTE_PROTOCOL_MALFORMED;

    if (!data || !message_out || length == 0 || length > INT_MAX) {
        set_error(error_out, error_size, "empty or oversized record");
        return result;
    }
    memset(message_out, 0, sizeof(*message_out));
    if (memchr(data, '\0', length)) {
        set_error(error_out, error_size, "record contains an embedded NUL");
        return result;
    }
    if (!valid_utf8(data, length)) {
        set_error(error_out, error_size, "record is not valid UTF-8");
        return result;
    }

    tokener = json_tokener_new();
    if (!tokener) {
        set_error(error_out, error_size, "JSON parser allocation failed");
        return result;
    }
    root = json_tokener_parse_ex(tokener, data, (int)length);
    enum json_tokener_error json_error = json_tokener_get_error(tokener);
    size_t parsed = json_tokener_get_parse_end(tokener);
    while (parsed < length && strchr(" \t\r\n", ((const char*)data)[parsed])) parsed++;
    if (json_error != json_tokener_success || !root || parsed != length ||
        !object_has_exact_keys(root, envelope_keys, 2) ||
        !json_object_object_get_ex(root, "action", &action_object) ||
        json_object_get_type(action_object) != json_type_string ||
        !json_object_object_get_ex(root, "parameters", &parameters) ||
        json_object_get_type(parameters) != json_type_object) {
        set_error(error_out, error_size, "record is not exactly one valid Dante JSON envelope");
        goto done;
    }

    const char* action = json_object_get_string(action_object);
    if (strcmp(action, "requestConfiguration") == 0) {
        if (!object_has_exact_keys(parameters, no_keys, 0)) {
            set_error(error_out, error_size, "requestConfiguration parameters must be empty");
            goto done;
        }
        message_out->type = DANTE_ACTION_REQUEST_CONFIGURATION;
    } else if (strcmp(action, "createVideoUnicastTxFlow") == 0 ||
               strcmp(action, "createVideoMulticastTxFlow") == 0 ||
               strcmp(action, "createVideoUnicastRxFlow") == 0 ||
               strcmp(action, "createVideoMulticastRxFlow") == 0) {
        if (!parse_create(parameters, action, message_out, error_out, error_size)) goto done;
    } else if (strcmp(action, "deleteTxFlow") == 0 || strcmp(action, "deleteRxFlow") == 0) {
        if (!parse_delete(parameters, action, message_out, error_out, error_size)) goto done;
    } else {
        set_error(error_out, error_size, "unsupported Dante action: %s", action);
        result = DANTE_PROTOCOL_UNKNOWN_ACTION;
        goto done;
    }
    result = DANTE_PROTOCOL_OK;

done:
    if (result != DANTE_PROTOCOL_OK) dante_protocol_message_clear(message_out);
    if (root) json_object_put(root);
    json_tokener_free(tokener);
    return result;
}

void
dante_protocol_message_clear(dante_message_t* message)
{
    if (!message) return;
    free(message->flow.receiver_address);
    free(message->flow.multicast_address);
    free(message->flow.transmitter_address);
    memset(message, 0, sizeof(*message));
}

static zst_result_t
serialize(struct json_object* root, char** data_out, size_t* length_out)
{
    if (!root || !data_out || !length_out) {
        if (root) json_object_put(root);
        return ZST_ERROR_INVALID_ARGUMENT;
    }
    const char* json = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    size_t length = strlen(json);
    char* copy = malloc(length);
    if (!copy) {
        json_object_put(root);
        return ZST_ERROR;
    }
    memcpy(copy, json, length);
    json_object_put(root);
    *data_out = copy;
    *length_out = length;
    return ZST_OK;
}

static struct json_object*
new_envelope(const char* action, struct json_object** parameters_out)
{
    struct json_object* root = json_object_new_object();
    struct json_object* parameters = json_object_new_object();
    struct json_object* action_value = json_object_new_string(action);
    if (!root || !parameters || !action_value) {
        if (root) json_object_put(root);
        if (parameters) json_object_put(parameters);
        if (action_value) json_object_put(action_value);
        return NULL;
    }
    json_object_object_add(root, "action", action_value);
    json_object_object_add(root, "parameters", parameters);
    *parameters_out = parameters;
    return root;
}

zst_result_t
dante_protocol_encode_start(uint32_t tx_channels, uint32_t rx_channels,
                            char** data_out, size_t* length_out)
{
    struct json_object* parameters;
    struct json_object* root = new_envelope("start", &parameters);
    struct json_object* tx = json_object_new_array();
    struct json_object* rx = json_object_new_array();
    if (!root || !tx || !rx) {
        if (root) json_object_put(root);
        if (tx) json_object_put(tx);
        if (rx) json_object_put(rx);
        return ZST_ERROR;
    }
    for (uint32_t direction = 0; direction < 2; direction++) {
        struct json_object* array = direction == 0 ? tx : rx;
        uint32_t count = direction == 0 ? tx_channels : rx_channels;
        for (uint32_t i = 0; i < count; i++) {
            struct json_object* channel = json_object_new_object();
            struct json_object* subtypes = json_object_new_array();
            if (!channel || !subtypes ||
                json_object_array_add(subtypes, json_object_new_string("H264")) != 0) {
                if (channel) json_object_put(channel);
                if (subtypes) json_object_put(subtypes);
                json_object_put(root);
                json_object_put(tx);
                json_object_put(rx);
                return ZST_ERROR;
            }
            json_object_object_add(channel, "subtypes", subtypes);
            json_object_array_add(array, channel);
        }
    }
    json_object_object_add(parameters, "txVideoChannels", tx);
    json_object_object_add(parameters, "rxVideoChannels", rx);
    return serialize(root, data_out, length_out);
}

zst_result_t
dante_protocol_encode_stop(char** data_out, size_t* length_out)
{
    struct json_object* parameters;
    return serialize(new_envelope("stop", &parameters), data_out, length_out);
}

static zst_result_t
encode_status(const char* action, const char* index_key, uint32_t index,
              const char* status_key, const char* status,
              char** data_out, size_t* length_out)
{
    struct json_object* parameters;
    struct json_object* root = new_envelope(action, &parameters);
    if (!root) return ZST_ERROR;
    json_object_object_add(parameters, index_key, json_object_new_int64(index));
    json_object_object_add(parameters, status_key, json_object_new_string(status));
    return serialize(root, data_out, length_out);
}

zst_result_t
dante_protocol_encode_rx_status(uint32_t flow_index, zst_dante_rx_flow_status_t status,
                                char** data_out, size_t* length_out)
{
    const char* text;
    if (status == ZST_DANTE_RX_STATUS_OK) text = "statusOK";
    else if (status == ZST_DANTE_RX_STATUS_NOT_RECEIVING_PACKETS) text = "statusNotReceivingPackets";
    else return ZST_ERROR_INVALID_ARGUMENT;
    return encode_status("reportRxFlowStatus", "flowIndex", flow_index,
                         "flowStatus", text, data_out, length_out);
}

zst_result_t
dante_protocol_encode_tx_status(uint32_t channel_index, zst_dante_tx_channel_status_t status,
                                char** data_out, size_t* length_out)
{
    const char* text;
    if (status == ZST_DANTE_TX_STATUS_OK) text = "statusOK";
    else if (status == ZST_DANTE_TX_STATUS_EXT_NOT_CONNECTED) text = "statusExtNotConnected";
    else return ZST_ERROR_INVALID_ARGUMENT;
    return encode_status("reportTxChannelStatus", "channelIndex", channel_index,
                         "channelStatus", text, data_out, length_out);
}
