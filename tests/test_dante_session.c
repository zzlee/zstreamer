#define _POSIX_C_SOURCE 200809L

#include "zstreamer/elements/zst_dante_session.h"
#include "dante_protocol.h"
#include <assert.h>
#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WAIT_MS 3000

typedef struct {
    int listener;
    int failed;
    int saw_stop;
} fake_dvr_t;

static int
wait_fd(int fd, short events)
{
    struct pollfd poll_fd = { .fd = fd, .events = events };
    int result;
    do result = poll(&poll_fd, 1, WAIT_MS); while (result < 0 && errno == EINTR);
    return result == 1 && (poll_fd.revents & events) != 0;
}

static int
receive_record(int fd, char* buffer, size_t size)
{
    if (!wait_fd(fd, POLLIN)) return -1;
    ssize_t length = recv(fd, buffer, size, 0);
    return length > 0 ? (int)length : -1;
}

static int
send_text(int fd, const char* text)
{
    size_t length = strlen(text);
    return send(fd, text, length, MSG_NOSIGNAL) == (ssize_t)length;
}

static int
record_has_action(const char* data, int length, const char* action)
{
    struct json_tokener* tokener = json_tokener_new();
    struct json_object* root = json_tokener_parse_ex(tokener, data, length);
    struct json_object* value = NULL;
    int matches = root && json_tokener_get_error(tokener) == json_tokener_success &&
                  json_object_object_get_ex(root, "action", &value) &&
                  strcmp(json_object_get_string(value), action) == 0;
    if (root) json_object_put(root);
    json_tokener_free(tokener);
    return matches;
}

static int
validate_start(const char* data, int length)
{
    struct json_tokener* tokener = json_tokener_new();
    struct json_object* root = json_tokener_parse_ex(tokener, data, length);
    struct json_object* parameters = NULL;
    struct json_object* tx = NULL;
    struct json_object* rx = NULL;
    int valid = root && json_tokener_get_error(tokener) == json_tokener_success &&
                record_has_action(data, length, "start") &&
                json_object_object_get_ex(root, "parameters", &parameters) &&
                json_object_object_get_ex(parameters, "txVideoChannels", &tx) &&
                json_object_object_get_ex(parameters, "rxVideoChannels", &rx) &&
                json_object_array_length(tx) == 2 && json_object_array_length(rx) == 1;
    for (size_t i = 0; valid && i < 3; i++) {
        struct json_object* channel = i < 2 ? json_object_array_get_idx(tx, i)
                                            : json_object_array_get_idx(rx, 0);
        struct json_object* subtypes = NULL;
        valid = json_object_object_get_ex(channel, "subtypes", &subtypes) &&
                json_object_array_length(subtypes) == 1 &&
                strcmp(json_object_get_string(json_object_array_get_idx(subtypes, 0)), "H264") == 0;
    }
    if (root) json_object_put(root);
    json_tokener_free(tokener);
    return valid;
}

static void*
fake_dvr_thread(void* argument)
{
    fake_dvr_t* dvr = argument;
    char buffer[4096];
    if (!wait_fd(dvr->listener, POLLIN)) { dvr->failed = 1; return NULL; }
    int client = accept(dvr->listener, NULL, NULL);
    int length = client >= 0 ? receive_record(client, buffer, sizeof(buffer)) : -1;
    if (length < 0 || !validate_start(buffer, length)) { dvr->failed = 2; goto first_done; }

    const char embedded_nul[] = "{\"action\":\"requestConfiguration\",\0\"parameters\":{}}";
    if (send(client, embedded_nul, sizeof(embedded_nul) - 1, MSG_NOSIGNAL) < 0 ||
        !send_text(client, "{\"action\":\"futureAction\",\"parameters\":{}}") ||
        !send_text(client, "{\"action\":\"createVideoUnicastTxFlow\",\"parameters\":{\"flowIndex\":0,\"channelIndex\":0,\"port\":5004,\"receiverAddress\":\"192.0.2.20\",\"transmitterAddress\":\"192.0.2.10\"}}") ||
        !send_text(client, "{\"action\":\"createVideoMulticastTxFlow\",\"parameters\":{\"flowIndex\":1,\"channelIndex\":1,\"port\":5006,\"multicastAddress\":\"239.192.0.20\",\"transmitterAddress\":\"192.0.2.10\"}}") ||
        !send_text(client, "{\"action\":\"createVideoUnicastRxFlow\",\"parameters\":{\"flowIndex\":2,\"channelIndex\":0,\"port\":5008,\"receiverAddress\":\"192.0.2.20\",\"transmitterAddress\":\"192.0.2.10\"}}") ||
        !send_text(client, "{\"action\":\"createVideoMulticastRxFlow\",\"parameters\":{\"flowIndex\":3,\"channelIndex\":0,\"port\":5010,\"multicastAddress\":\"239.192.0.21\",\"transmitterAddress\":\"192.0.2.10\"}}") ||
        !send_text(client, "{\"action\":\"createVideoUnicastTxFlow\",\"parameters\":{\"flowIndex\":0,\"channelIndex\":0,\"port\":5004,\"receiverAddress\":\"192.0.2.20\",\"transmitterAddress\":\"192.0.2.10\"}}") ||
        !send_text(client, "{\"action\":\"deleteRxFlow\",\"parameters\":{\"flowIndex\":99}}") ||
        !send_text(client, "{\"action\":\"requestConfiguration\",\"parameters\":{}}")) {
        dvr->failed = 3;
        goto first_done;
    }
    /*
     * requestConfiguration must be skipped (matching the reference DVR
     * implementations): no start is re-sent, and the only records that follow
     * are the two status reports produced by the test's report_* calls.
     */
    for (int i = 0; i < 2; i++) {
        length = receive_record(client, buffer, sizeof(buffer));
        if (length < 0 || record_has_action(buffer, length, "start") ||
            (!record_has_action(buffer, length, "reportRxFlowStatus") &&
             !record_has_action(buffer, length, "reportTxChannelStatus"))) {
            dvr->failed = 4;
            goto first_done;
        }
    }
    if (!send_text(client, "{\"action\":\"deleteTxFlow\",\"parameters\":{\"flowIndex\":0}}") ||
        !send_text(client, "{\"action\":\"deleteRxFlow\",\"parameters\":{\"flowIndex\":2}}"))
        dvr->failed = 6;
first_done:
    if (client >= 0) close(client);
    if (dvr->failed) return NULL;

    if (!wait_fd(dvr->listener, POLLIN)) { dvr->failed = 7; return NULL; }
    client = accept(dvr->listener, NULL, NULL);
    length = client >= 0 ? receive_record(client, buffer, sizeof(buffer)) : -1;
    if (length < 0 || !validate_start(buffer, length)) dvr->failed = 8;
    if (!dvr->failed) {
        length = receive_record(client, buffer, sizeof(buffer));
        dvr->saw_stop = length > 0 && record_has_action(buffer, length, "stop");
        if (!dvr->saw_stop) dvr->failed = 9;
    }
    if (client >= 0) close(client);
    return NULL;
}

static void
test_codec(void)
{
    dante_message_t message;
    char error[128];
    const char valid[] = "{\"action\":\"createVideoMulticastRxFlow\",\"parameters\":{\"flowIndex\":7,\"channelIndex\":1,\"port\":5004,\"multicastAddress\":\"239.1.2.3\",\"transmitterAddress\":\"192.0.2.1\"}}";
    assert(dante_protocol_parse_record(valid, sizeof(valid) - 1, &message,
                                       error, sizeof(error)) == DANTE_PROTOCOL_OK);
    assert(message.flow.direction == ZST_DANTE_FLOW_RX);
    assert(message.flow.transport == ZST_DANTE_FLOW_MULTICAST);
    assert(strcmp(message.flow.multicast_address, "239.1.2.3") == 0);
    dante_protocol_message_clear(&message);

    const char bad_class[] = "{\"action\":\"createVideoMulticastRxFlow\",\"parameters\":{\"flowIndex\":7,\"channelIndex\":1,\"port\":5004,\"multicastAddress\":\"192.0.2.3\",\"transmitterAddress\":\"192.0.2.1\"}}";
    assert(dante_protocol_parse_record(bad_class, sizeof(bad_class) - 1, &message,
                                       error, sizeof(error)) == DANTE_PROTOCOL_MALFORMED);
    const char trailing[] = "{\"action\":\"requestConfiguration\",\"parameters\":{}}{}";
    assert(dante_protocol_parse_record(trailing, sizeof(trailing) - 1, &message,
                                       error, sizeof(error)) == DANTE_PROTOCOL_MALFORMED);

    char* encoded = NULL;
    size_t length = 0;
    assert(dante_protocol_encode_rx_status(7, ZST_DANTE_RX_STATUS_NOT_RECEIVING_PACKETS,
                                           &encoded, &length) == ZST_OK);
    assert(record_has_action(encoded, (int)length, "reportRxFlowStatus"));
    free(encoded);
}

static void
test_event_ownership(void)
{
    zst_dante_flow_t flow = {
        .direction = ZST_DANTE_FLOW_TX,
        .transport = ZST_DANTE_FLOW_UNICAST,
        .flow_index = 5,
        .channel_index = 2,
        .port = 5004,
        .receiver_address = strdup("192.0.2.20"),
        .transmitter_address = strdup("192.0.2.10")
    };
    zst_event_t* event = zst_event_new_dante_flow_created(NULL, &flow);
    assert(event);
    flow.receiver_address[0] = 'X';
    free(flow.receiver_address);
    free(flow.transmitter_address);
    assert(strcmp(event->as.dante_flow.flow.receiver_address, "192.0.2.20") == 0);
    assert(strcmp(event->as.dante_flow.flow.transmitter_address, "192.0.2.10") == 0);
    zst_event_destroy(event);
}

static zst_event_t*
next_relevant(zst_bus_t* bus)
{
    for (int i = 0; i < 40; i++) {
        zst_event_t* event = NULL;
        if (zst_bus_pop(bus, &event, 100) != ZST_OK) continue;
        if (event->type == ZST_EVENT_STATE_CHANGED) {
            zst_event_destroy(event);
            continue;
        }
        return event;
    }
    return NULL;
}

static void
test_session(void)
{
    char directory[] = "/tmp/zst-dante-XXXXXX";
    assert(mkdtemp(directory));
    char path[108];
    snprintf(path, sizeof(path), "%s/dvr", directory);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    assert(listener >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    assert(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(listener, 2) == 0);

    fake_dvr_t dvr = { .listener = listener };
    pthread_t server_thread;
    assert(pthread_create(&server_thread, NULL, fake_dvr_thread, &dvr) == 0);
    zst_bus_t* bus = zst_bus_create();
    zst_element_t* session = zst_dante_session_create(path);
    assert(bus && session);
    session->bus = bus;
    assert(zst_element_set_property_uint(session, ZST_DANTE_SESSION_PROP_TX_VIDEO_CHANNELS, 2) == ZST_OK);
    assert(zst_element_set_property_uint(session, ZST_DANTE_SESSION_PROP_RX_VIDEO_CHANNELS, 1) == ZST_OK);
    assert(zst_element_set_property_uint(session, ZST_DANTE_SESSION_PROP_RECONNECT_DELAY_MS, 20) == ZST_OK);
    assert(zst_element_set_state(session, ZST_STATE_PLAYING) == ZST_OK);

    int creates = 0, warnings = 0, configuration = 0;
    while (creates < 4 || warnings < 4 || configuration < 1) {
        zst_event_t* event = next_relevant(bus);
        assert(event);
        if (event->type == ZST_EVENT_DANTE_FLOW_CREATED) creates++;
        else if (event->type == ZST_EVENT_WARNING) warnings++;
        else if (event->type == ZST_EVENT_DANTE_CONFIGURATION_REQUESTED) configuration++;
        zst_event_destroy(event);
    }
    assert(zst_dante_session_report_rx_flow_status(
               session, 3, ZST_DANTE_RX_STATUS_OK) == ZST_OK);
    assert(zst_dante_session_report_tx_channel_status(
               session, 1, ZST_DANTE_TX_STATUS_EXT_NOT_CONNECTED) == ZST_OK);

    int deletes = 0, disconnected = 0, reconnected = 0;
    while (!reconnected) {
        zst_event_t* event = next_relevant(bus);
        assert(event);
        if (event->type == ZST_EVENT_DANTE_FLOW_DELETED) deletes++;
        else if (event->type == ZST_EVENT_DANTE_DISCONNECTED) disconnected++;
        else if (event->type == ZST_EVENT_DANTE_CONNECTED && disconnected) reconnected = 1;
        zst_event_destroy(event);
    }
    assert(deletes == 4 && disconnected == 1);
    assert(zst_element_set_state(session, ZST_STATE_NULL) == ZST_OK);
    pthread_join(server_thread, NULL);
    if (dvr.failed || !dvr.saw_stop)
        fprintf(stderr, "fake DVR failed at stage %d (saw_stop=%d)\n", dvr.failed, dvr.saw_stop);
    assert(!dvr.failed && dvr.saw_stop);
    session->bus = NULL;
    zst_element_destroy(session);
    zst_bus_destroy(bus);
    close(listener);
    unlink(path);
    rmdir(directory);
}

int
main(void)
{
    test_codec();
    test_event_ownership();
    test_session();
    puts("Dante session tests passed");
    return 0;
}
