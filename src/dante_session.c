#define _POSIX_C_SOURCE 200809L

/* Keep dante session trace/debug logs compiled in even in NDEBUG (Release)
   builds so runtime zst_log_set_level() controls their emission. */
#define ZST_LOG_LEVEL ZST_LOG_LEVEL_TRACE
#include "zstreamer/elements/zst_dante_session.h"
#include "zst_log.h"
#include "dante_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct dante_flow_node {
    zst_dante_flow_t flow;
    struct dante_flow_node* next;
} dante_flow_node_t;

typedef struct {
    char socket_path[sizeof(((struct sockaddr_un*)0)->sun_path)];
    uint32_t tx_video_channels;
    uint32_t rx_video_channels;
    int reconnect;
    uint32_t reconnect_delay_ms;
    int max_reconnect_attempts;
    size_t max_record_size;
    pthread_mutex_t lock;
    pthread_cond_t stop_cond;
    int synchronization_ready;
    int stop_requested;
    int thread_started;
    pthread_t thread;
    int fd;
    zst_dante_session_state_t session_state;
    dante_flow_node_t* flows;
    uint32_t active_tx_flows;
    uint32_t active_rx_flows;
    zst_element_t* element;
} dante_session_t;

static zst_element_ops_t dante_session_ops;

static void
post_event(zst_element_t* el, zst_event_t* event)
{
    if (!event) return;
    if (!el->bus || zst_bus_post(el->bus, event) != ZST_OK) zst_event_destroy(event);
}

static void
post_warning(zst_element_t* el, const char* message)
{
    post_event(el, zst_event_new_warning(el, ZST_ERROR_INVALID_ARGUMENT, message));
}

static zst_result_t
send_record(dante_session_t* session, const void* data, size_t length)
{
    if (!data || length == 0) return ZST_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&session->lock);
    int fd = session->fd;
    pthread_mutex_unlock(&session->lock);
    if (fd < 0) return ZST_ERROR;

    /* SOCK_SEQPACKET preserves record boundaries: never split one protocol
     * message into multiple send() calls.  Retry EAGAIN once after a bounded
     * writable wait, then treat a real send failure as connection loss. */
    ssize_t sent = send(fd, data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct pollfd descriptor = { .fd = fd, .events = POLLOUT };
        int ready;
        do ready = poll(&descriptor, 1, 50); while (ready < 0 && errno == EINTR);
        if (ready > 0 && (descriptor.revents & POLLOUT))
            sent = send(fd, data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    if (sent == (ssize_t)length) return ZST_OK;

    pthread_mutex_lock(&session->lock);
    if (session->fd == fd) {
        session->fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (!session->stop_requested)
            session->session_state = ZST_DANTE_SESSION_RECONNECT_WAIT;
    }
    pthread_mutex_unlock(&session->lock);
    return ZST_ERROR;
}

static void
trace_send(const char* tag, const void* data, size_t length)
{
    ZST_LOG_TRACE("dantesession", "TX %s (%zu bytes)", tag, length);
    const unsigned char* p = (const unsigned char*)data;
    size_t show = length < 256 ? length : 256;
    for (size_t i = 0; i < show; i += 64) {
        char line[65];
        size_t n = 0;
        for (size_t j = i; j < i + 64 && j < show; j++)
            line[n++] = (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.';
        line[n] = '\0';
        ZST_LOG_TRACE("dantesession", "TX |%s|", line);
    }
}

static void
trace_recv(const void* data, size_t length)
{
    ZST_LOG_TRACE("dantesession", "RX (%zu bytes)", length);
    const unsigned char* p = (const unsigned char*)data;
    size_t show = length < 256 ? length : 256;
    for (size_t i = 0; i < show; i += 64) {
        char line[65];
        size_t n = 0;
        for (size_t j = i; j < i + 64 && j < show; j++)
            line[n++] = (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.';
        line[n] = '\0';
        ZST_LOG_TRACE("dantesession", "RX |%s|", line);
    }
}

static zst_result_t
send_start(dante_session_t* session)
{
    char* record = NULL;
    size_t length = 0;
    zst_result_t result = dante_protocol_encode_start(session->tx_video_channels,
                                                       session->rx_video_channels,
                                                       &record, &length);
    if (result == ZST_OK) {
        trace_send("start", record, length);
        result = send_record(session, record, length);
    }
    free(record);
    return result;
}

static void
close_connection(dante_session_t* session)
{
    pthread_mutex_lock(&session->lock);
    int fd = session->fd;
    session->fd = -1;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    pthread_mutex_unlock(&session->lock);
}

static void
free_flow(zst_dante_flow_t* flow)
{
    free(flow->receiver_address);
    free(flow->multicast_address);
    free(flow->transmitter_address);
}

static void
invalidate_flows(dante_session_t* session, int post_disconnected)
{
    pthread_mutex_lock(&session->lock);
    dante_flow_node_t* flows = session->flows;
    session->flows = NULL;
    session->active_tx_flows = 0;
    session->active_rx_flows = 0;
    session->session_state = session->stop_requested
        ? ZST_DANTE_SESSION_STOPPED : ZST_DANTE_SESSION_RECONNECT_WAIT;
    pthread_mutex_unlock(&session->lock);
    while (flows) {
        dante_flow_node_t* next = flows->next;
        post_event(session->element,
                   zst_event_new_dante_flow_deleted(session->element, &flows->flow));
        free_flow(&flows->flow);
        free(flows);
        flows = next;
    }
    if (post_disconnected)
        post_event(session->element, zst_event_new_dante_disconnected(session->element));
}

static int
connect_socket(dante_session_t* session)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, session->socket_path, strlen(session->socket_path) + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    pthread_mutex_lock(&session->lock);
    if (session->stop_requested) {
        pthread_mutex_unlock(&session->lock);
        close(fd);
        return -1;
    }
    session->fd = fd;
    pthread_mutex_unlock(&session->lock);
    socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 strlen(address.sun_path) + 1);
    int connect_result = connect(fd, (struct sockaddr*)&address, size);
    if (connect_result != 0 && errno == EINPROGRESS) {
        struct pollfd descriptor = { .fd = fd, .events = POLLOUT };
        int ready;
        do ready = poll(&descriptor, 1, 50); while (ready < 0 && errno == EINTR);
        int error = 0;
        socklen_t error_size = sizeof(error);
        if (ready != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error) {
            pthread_mutex_lock(&session->lock);
            if (session->fd == fd) session->fd = -1;
            pthread_mutex_unlock(&session->lock);
            close(fd);
            return -1;
        }
    } else if (connect_result != 0 && errno != EISCONN) {
        pthread_mutex_lock(&session->lock);
        if (session->fd == fd) session->fd = -1;
        pthread_mutex_unlock(&session->lock);
        close(fd);
        return -1;
    }
    return fd;
}

static int
wait_to_reconnect(dante_session_t* session)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += session->reconnect_delay_ms / 1000;
    deadline.tv_nsec += (long)(session->reconnect_delay_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&session->lock);
    while (!session->stop_requested) {
        int result = pthread_cond_timedwait(&session->stop_cond, &session->lock, &deadline);
        if (result == ETIMEDOUT) break;
    }
    int keep_running = !session->stop_requested;
    pthread_mutex_unlock(&session->lock);
    return keep_running;
}

static dante_flow_node_t*
find_flow(dante_session_t* session, zst_dante_flow_direction_t direction,
          uint32_t flow_index, dante_flow_node_t*** link_out)
{
    dante_flow_node_t** link = &session->flows;
    while (*link) {
        if ((*link)->flow.direction == direction && (*link)->flow.flow_index == flow_index) {
            if (link_out) *link_out = link;
            return *link;
        }
        link = &(*link)->next;
    }
    if (link_out) *link_out = link;
    return NULL;
}

static void
handle_create(dante_session_t* session, dante_message_t* message)
{
    uint32_t channel_count = message->flow.direction == ZST_DANTE_FLOW_TX
        ? session->tx_video_channels : session->rx_video_channels;
    if (message->flow.channel_index >= channel_count) {
        post_warning(session->element, "Dante flow channelIndex is not advertised by this session");
        return;
    }
    dante_flow_node_t* node = calloc(1, sizeof(*node));
    if (!node) {
        post_warning(session->element, "Dante flow registry allocation failed");
        return;
    }
    node->flow = message->flow;
    pthread_mutex_lock(&session->lock);
    if (find_flow(session, node->flow.direction, node->flow.flow_index, NULL)) {
        pthread_mutex_unlock(&session->lock);
        free(node);
        post_warning(session->element, "duplicate Dante flow create rejected");
        return;
    }
    node->next = session->flows;
    session->flows = node;
    if (node->flow.direction == ZST_DANTE_FLOW_TX) session->active_tx_flows++;
    else session->active_rx_flows++;
    pthread_mutex_unlock(&session->lock);
    memset(&message->flow, 0, sizeof(message->flow));
    post_event(session->element,
               zst_event_new_dante_flow_created(session->element, &node->flow));
}

static void
handle_delete(dante_session_t* session, const dante_message_t* message)
{
    pthread_mutex_lock(&session->lock);
    dante_flow_node_t** link = NULL;
    dante_flow_node_t* node = find_flow(session, message->delete_direction,
                                        message->delete_flow_index, &link);
    if (node) {
        *link = node->next;
        if (node->flow.direction == ZST_DANTE_FLOW_TX) session->active_tx_flows--;
        else session->active_rx_flows--;
    }
    pthread_mutex_unlock(&session->lock);
    if (!node) {
        post_warning(session->element, "delete for unknown Dante flow ignored");
        return;
    }
    post_event(session->element,
               zst_event_new_dante_flow_deleted(session->element, &node->flow));
    free_flow(&node->flow);
    free(node);
}

static void
hex_dump_record(const void* data, size_t length)
{
    const unsigned char* p = (const unsigned char*)data;
    size_t show = length < 128 ? length : 128;
    ZST_LOG_WARN("dantesession", "RAW %zu bytes:", length);
    for (size_t i = 0; i < show; i += 16) {
        char line[160];
        int off = 0;
        off += snprintf(line + off, sizeof(line) - (size_t)off, "    %04zx: ", i);
        for (size_t j = i; j < i + 16 && j < show; j++)
            off += snprintf(line + off, sizeof(line) - (size_t)off, "%02x ", p[j]);
        off += snprintf(line + off, sizeof(line) - (size_t)off, " | ");
        for (size_t j = i; j < i + 16 && j < show; j++)
            off += snprintf(line + off, sizeof(line) - (size_t)off, "%c",
                            (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
        ZST_LOG_WARN("dantesession", "%s", line);
    }
    if (show < length)
        ZST_LOG_WARN("dantesession", "    ... (%zu more bytes)", length - show);
}

static const char*
action_name(dante_action_type_t type)
{
    switch (type) {
        case DANTE_ACTION_REQUEST_CONFIGURATION: return "requestConfiguration";
        case DANTE_ACTION_CREATE_FLOW: return "createFlow";
        case DANTE_ACTION_DELETE_FLOW: return "deleteFlow";
        default: return "unknown";
    }
}

static void
handle_record(dante_session_t* session, const void* data, size_t length)
{
    trace_recv(data, length);
    dante_message_t message;
    char error[192];
    dante_protocol_result_t result = dante_protocol_parse_record(
        data, length, &message, error, sizeof(error));
    if (result != DANTE_PROTOCOL_OK) {
        ZST_LOG_WARN("dantesession", "PARSE FAIL: %s", error);
        hex_dump_record(data, length);
        post_warning(session->element, error);
        return;
    }
    ZST_LOG_DEBUG("dantesession", "action=%s", action_name(message.type));
    if (message.type == DANTE_ACTION_REQUEST_CONFIGURATION) {
        ZST_LOG_TRACE("dantesession", "requestConfiguration ignored (start already advertised)");
        post_event(session->element,
                   zst_event_new_dante_configuration_requested(session->element));
    } else if (message.type == DANTE_ACTION_CREATE_FLOW) {
        ZST_LOG_DEBUG("dantesession", "createFlow dir=%s flow=%u ch=%u port=%u",
                     message.flow.direction == ZST_DANTE_FLOW_TX ? "TX" : "RX",
                     message.flow.flow_index, message.flow.channel_index, message.flow.port);
        handle_create(session, &message);
    } else {
        ZST_LOG_DEBUG("dantesession", "deleteFlow dir=%s flow=%u",
                     message.delete_direction == ZST_DANTE_FLOW_TX ? "TX" : "RX",
                     message.delete_flow_index);
        handle_delete(session, &message);
    }
    dante_protocol_message_clear(&message);
}

static void
receive_loop(dante_session_t* session)
{
    unsigned char* record = malloc(session->max_record_size);
    if (!record) return;
    for (;;) {
        struct iovec iov = { .iov_base = record, .iov_len = session->max_record_size };
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        pthread_mutex_lock(&session->lock);
        int fd = session->fd;
        int stopping = session->stop_requested;
        pthread_mutex_unlock(&session->lock);
        if (stopping || fd < 0) break;
        struct pollfd descriptor = { .fd = fd, .events = POLLIN };
        int ready;
        do ready = poll(&descriptor, 1, 50); while (ready < 0 && errno == EINTR);
        if (ready == 0) continue;
        if (ready < 0 || descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(descriptor.revents & POLLIN)) continue;
        ssize_t received = recvmsg(fd, &message, 0);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (received <= 0) break;
        if (message.msg_flags & MSG_TRUNC) {
            post_warning(session->element, "oversized or truncated Dante record rejected");
            continue;
        }
        handle_record(session, record, (size_t)received);
    }
    free(record);
}

static void*
dante_worker(void* argument)
{
    dante_session_t* session = argument;
    int reconnect_attempts = 0;
    ZST_LOG_INFO("dantesession", "worker started, socket=%s", session->socket_path);
    for (;;) {
        pthread_mutex_lock(&session->lock);
        int stopping = session->stop_requested;
        session->session_state = stopping ? ZST_DANTE_SESSION_STOPPED
                                          : ZST_DANTE_SESSION_CONNECTING;
        pthread_mutex_unlock(&session->lock);
        if (stopping) break;

        ZST_LOG_INFO("dantesession", "connecting to %s (attempt %d)...", session->socket_path,
             reconnect_attempts + 1);
        int fd = connect_socket(session);
        if (fd < 0) {
            ZST_LOG_WARN("dantesession", "connect failed: %s", strerror(errno));
            if (!session->reconnect ||
                (session->max_reconnect_attempts >= 0 &&
                 reconnect_attempts >= session->max_reconnect_attempts)) break;
            reconnect_attempts++;
            pthread_mutex_lock(&session->lock);
            session->session_state = ZST_DANTE_SESSION_RECONNECT_WAIT;
            pthread_mutex_unlock(&session->lock);
            if (!wait_to_reconnect(session)) break;
            continue;
        }
        ZST_LOG_DEBUG("dantesession", "connected (fd=%d)", fd);

        if (send_start(session) != ZST_OK) {
            ZST_LOG_ERROR("dantesession", "send start failed");
            close_connection(session);
            if (!session->reconnect ||
                (session->max_reconnect_attempts >= 0 &&
                 reconnect_attempts >= session->max_reconnect_attempts)) break;
            reconnect_attempts++;
            if (!wait_to_reconnect(session)) break;
            continue;
        }
        reconnect_attempts = 0;
        pthread_mutex_lock(&session->lock);
        session->session_state = ZST_DANTE_SESSION_CONNECTED;
        pthread_mutex_unlock(&session->lock);
        ZST_LOG_INFO("dantesession", "session CONNECTED, entering receive loop");
        post_event(session->element, zst_event_new_dante_connected(session->element));

        receive_loop(session);
        ZST_LOG_INFO("dantesession", "receive loop exited");
        pthread_mutex_lock(&session->lock);
        int orderly_stop = session->stop_requested;
        pthread_mutex_unlock(&session->lock);
        close_connection(session);
        invalidate_flows(session, 1);
        if (orderly_stop || !session->reconnect ||
            (session->max_reconnect_attempts >= 0 &&
             reconnect_attempts >= session->max_reconnect_attempts)) break;
        reconnect_attempts++;
        if (!wait_to_reconnect(session)) break;
    }
    pthread_mutex_lock(&session->lock);
    session->session_state = ZST_DANTE_SESSION_STOPPED;
    pthread_mutex_unlock(&session->lock);
    return NULL;
}

static zst_result_t
dante_open(zst_element_t* el)
{
    dante_session_t* session = el->priv;
    if (pthread_mutex_init(&session->lock, NULL) != 0) return ZST_ERROR;
    if (pthread_cond_init(&session->stop_cond, NULL) != 0) {
        pthread_mutex_destroy(&session->lock);
        return ZST_ERROR;
    }
    session->synchronization_ready = 1;
    session->fd = -1;
    session->session_state = ZST_DANTE_SESSION_STOPPED;
    return ZST_OK;
}

static zst_result_t
dante_start(zst_element_t* el)
{
    dante_session_t* session = el->priv;
    pthread_mutex_lock(&session->lock);
    session->stop_requested = 0;
    session->thread_started = 1;
    pthread_mutex_unlock(&session->lock);
    if (pthread_create(&session->thread, NULL, dante_worker, session) != 0) {
        pthread_mutex_lock(&session->lock);
        session->thread_started = 0;
        pthread_mutex_unlock(&session->lock);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
dante_stop(zst_element_t* el)
{
    dante_session_t* session = el->priv;
    char* record = NULL;
    size_t length = 0;
    (void)dante_protocol_encode_stop(&record, &length);

    pthread_mutex_lock(&session->lock);
    int started = session->thread_started;
    session->stop_requested = 1;
    pthread_cond_broadcast(&session->stop_cond);
    int fd = session->fd;
    if (fd >= 0 && record && length > 0)
        (void)send(fd, record, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    pthread_mutex_unlock(&session->lock);
    free(record);
    if (started) pthread_join(session->thread, NULL);
    pthread_mutex_lock(&session->lock);
    session->thread_started = 0;
    pthread_mutex_unlock(&session->lock);
    return ZST_OK;
}

static zst_result_t
dante_close(zst_element_t* el)
{
    dante_session_t* session = el->priv;
    if (!session->synchronization_ready) return ZST_OK;
    close_connection(session);
    invalidate_flows(session, 0);
    pthread_cond_destroy(&session->stop_cond);
    pthread_mutex_destroy(&session->lock);
    session->synchronization_ready = 0;
    return ZST_OK;
}

static int
parse_uint(const char* text, uint64_t maximum, uint64_t* result)
{
    if (!text || !*text || *text == '-') return 0;
    char* end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > maximum) return 0;
    *result = value;
    return 1;
}

static zst_result_t
dante_set_property(zst_element_t* el, const char* name, const char* value)
{
    dante_session_t* session = el->priv;
    if (!name || !value || session->thread_started) return ZST_ERROR;
    uint64_t number;
    if (strcmp(name, ZST_DANTE_SESSION_PROP_SOCKET_PATH) == 0) {
        size_t length = strlen(value);
        if (length == 0 || length >= sizeof(session->socket_path)) return ZST_ERROR_INVALID_ARGUMENT;
        memcpy(session->socket_path, value, length + 1);
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_TX_VIDEO_CHANNELS) == 0) {
        if (!parse_uint(value, UINT16_MAX, &number)) return ZST_ERROR_INVALID_ARGUMENT;
        session->tx_video_channels = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_RX_VIDEO_CHANNELS) == 0) {
        if (!parse_uint(value, UINT16_MAX, &number)) return ZST_ERROR_INVALID_ARGUMENT;
        session->rx_video_channels = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_RECONNECT) == 0) {
        if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) session->reconnect = 1;
        else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) session->reconnect = 0;
        else return ZST_ERROR_INVALID_ARGUMENT;
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_RECONNECT_DELAY_MS) == 0) {
        if (!parse_uint(value, UINT32_MAX, &number)) return ZST_ERROR_INVALID_ARGUMENT;
        session->reconnect_delay_ms = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_MAX_RECONNECT_ATTEMPTS) == 0) {
        if (strcmp(value, "-1") == 0) session->max_reconnect_attempts = -1;
        else {
            if (!parse_uint(value, INT_MAX, &number)) return ZST_ERROR_INVALID_ARGUMENT;
            session->max_reconnect_attempts = (int)number;
        }
    } else if (strcmp(name, ZST_DANTE_SESSION_PROP_MAX_RECORD_SIZE) == 0) {
        if (!parse_uint(value, ZST_DANTE_SESSION_MAX_RECORD_LIMIT, &number) || number == 0)
            return ZST_ERROR_INVALID_ARGUMENT;
        session->max_record_size = (size_t)number;
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static const char*
state_name(zst_dante_session_state_t state)
{
    switch (state) {
        case ZST_DANTE_SESSION_CONNECTING: return "connecting";
        case ZST_DANTE_SESSION_CONNECTED: return "connected";
        case ZST_DANTE_SESSION_RECONNECT_WAIT: return "reconnect-wait";
        default: return "stopped";
    }
}

static zst_result_t
dante_get_property(zst_element_t* el, const char* name, char* out, size_t size)
{
    dante_session_t* session = el->priv;
    if (!name || !out || size == 0) return ZST_ERROR_INVALID_ARGUMENT;
    zst_dante_session_state_t state = session->session_state;
    uint32_t tx_active = session->active_tx_flows;
    uint32_t rx_active = session->active_rx_flows;
    if (session->synchronization_ready) {
        pthread_mutex_lock(&session->lock);
        state = session->session_state;
        tx_active = session->active_tx_flows;
        rx_active = session->active_rx_flows;
        pthread_mutex_unlock(&session->lock);
    }
    if (strcmp(name, ZST_DANTE_SESSION_PROP_SOCKET_PATH) == 0)
        snprintf(out, size, "%s", session->socket_path);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_TX_VIDEO_CHANNELS) == 0)
        snprintf(out, size, "%" PRIu32, session->tx_video_channels);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_RX_VIDEO_CHANNELS) == 0)
        snprintf(out, size, "%" PRIu32, session->rx_video_channels);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_RECONNECT) == 0)
        snprintf(out, size, "%s", session->reconnect ? "true" : "false");
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_RECONNECT_DELAY_MS) == 0)
        snprintf(out, size, "%" PRIu32, session->reconnect_delay_ms);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_MAX_RECONNECT_ATTEMPTS) == 0)
        snprintf(out, size, "%d", session->max_reconnect_attempts);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_MAX_RECORD_SIZE) == 0)
        snprintf(out, size, "%zu", session->max_record_size);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_CONNECTED) == 0)
        snprintf(out, size, "%s", state == ZST_DANTE_SESSION_CONNECTED ? "true" : "false");
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_SESSION_STATE) == 0)
        snprintf(out, size, "%s", state_name(state));
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_ACTIVE_TX_FLOWS) == 0)
        snprintf(out, size, "%" PRIu32, tx_active);
    else if (strcmp(name, ZST_DANTE_SESSION_PROP_ACTIVE_RX_FLOWS) == 0)
        snprintf(out, size, "%" PRIu32, rx_active);
    else
        return ZST_ERROR;
    return ZST_OK;
}

static zst_element_ops_t dante_session_ops = {
    .name = "dantesession",
    .open = dante_open,
    .close = dante_close,
    .start = dante_start,
    .stop = dante_stop,
    .set_property = dante_set_property,
    .get_property = dante_get_property
};

zst_element_t*
zst_dante_session_create(const char* socket_path)
{
    dante_session_t* session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    const char* path = socket_path ? socket_path : ZST_DANTE_SESSION_DEFAULT_SOCKET_PATH;
    if (!*path || strlen(path) >= sizeof(session->socket_path)) {
        free(session);
        return NULL;
    }
    memcpy(session->socket_path, path, strlen(path) + 1);
    session->tx_video_channels = 1;
    session->rx_video_channels = 1;
    session->reconnect = 1;
    session->reconnect_delay_ms = 500;
    session->max_reconnect_attempts = -1;
    session->max_record_size = ZST_DANTE_SESSION_MAX_RECORD_LIMIT;
    session->fd = -1;
    session->session_state = ZST_DANTE_SESSION_STOPPED;
    zst_element_t* element = zst_element_create(&dante_session_ops, session);
    if (!element) {
        free(session);
        return NULL;
    }
    session->element = element;
    return element;
}

zst_result_t
zst_dante_session_report_rx_flow_status(zst_element_t* element,
                                        uint32_t flow_index,
                                        zst_dante_rx_flow_status_t status)
{
    if (!element || element->ops != &dante_session_ops || !element->priv)
        return ZST_ERROR_INVALID_ARGUMENT;
    dante_session_t* session = element->priv;
    if (!session->synchronization_ready) return ZST_ERROR;
    pthread_mutex_lock(&session->lock);
    int valid = session->session_state == ZST_DANTE_SESSION_CONNECTED &&
                find_flow(session, ZST_DANTE_FLOW_RX, flow_index, NULL) != NULL;
    pthread_mutex_unlock(&session->lock);
    if (!valid) return ZST_ERROR;
    char* record = NULL;
    size_t length = 0;
    zst_result_t result = dante_protocol_encode_rx_status(flow_index, status,
                                                           &record, &length);
    if (result == ZST_OK) result = send_record(session, record, length);
    free(record);
    return result;
}

zst_result_t
zst_dante_session_report_tx_channel_status(zst_element_t* element,
                                           uint32_t channel_index,
                                           zst_dante_tx_channel_status_t status)
{
    if (!element || element->ops != &dante_session_ops || !element->priv)
        return ZST_ERROR_INVALID_ARGUMENT;
    dante_session_t* session = element->priv;
    if (!session->synchronization_ready || channel_index >= session->tx_video_channels)
        return ZST_ERROR;
    pthread_mutex_lock(&session->lock);
    int connected = session->session_state == ZST_DANTE_SESSION_CONNECTED;
    pthread_mutex_unlock(&session->lock);
    if (!connected) return ZST_ERROR;
    char* record = NULL;
    size_t length = 0;
    zst_result_t result = dante_protocol_encode_tx_status(channel_index, status,
                                                           &record, &length);
    if (result == ZST_OK) result = send_record(session, record, length);
    free(record);
    return result;
}
