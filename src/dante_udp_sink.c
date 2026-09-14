/*=============================================================================
    dante_udp_sink.c - Dante IPv4 UDP media transmitter
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_timestamp_pacer.h"

typedef struct {
    int fd;
    char destination_address[INET_ADDRSTRLEN];
    char transmitter_address[INET_ADDRSTRLEN];
    char multicast_interface_address[INET_ADDRSTRLEN];
    uint16_t port;
    uint8_t ttl;
    bool loop;
    bool timestamp_pacing;
    bool timing_observe;
    struct sockaddr_in destination;
    zst_timestamp_pacer_t pacer;
    bool pacer_initialized;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t send_errors;
    uint64_t last_packet_size;
    uint64_t send_errors_eagain;
    uint64_t send_errors_eintr;
    uint64_t send_errors_other;
    uint64_t send_last_errno;
    uint32_t send_buffer_size;
    uint32_t send_buffer_request;
    time_t last_error_log_sec;
    uint64_t timing_last_attempt_ns;
    uint64_t timing_current_burst_packets;
    uint64_t timing_gap_count;
    uint64_t timing_gap_total_ns;
    uint64_t timing_gap_min_ns;
    uint64_t timing_gap_max_ns;
    uint64_t timing_gap_le_5us;
    uint64_t timing_gap_le_20us;
    uint64_t timing_gap_le_100us;
    uint64_t timing_duration_total_ns;
    uint64_t timing_duration_min_ns;
    uint64_t timing_duration_max_ns;
    uint64_t eagain_last_gap_ns;
    uint64_t eagain_last_burst_packets;
    uint64_t eagain_max_burst_packets;
} dante_udp_sink_t;

static uint64_t
monotonic_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t
metric_min(uint64_t current, uint64_t value)
{
    return current == UINT64_MAX || value < current ? value : current;
}

static bool
parse_ipv4(const char* value, struct in_addr* address)
{
    return value && value[0] != '\0' && inet_pton(AF_INET, value, address) == 1;
}

static bool
is_multicast(struct in_addr address)
{
    return IN_MULTICAST(ntohl(address.s_addr));
}

static bool
valid_unicast(struct in_addr address)
{
    uint32_t host = ntohl(address.s_addr);
    return host != INADDR_ANY && host != INADDR_BROADCAST && !IN_MULTICAST(host);
}

static bool
parse_uint(const char* value, uint64_t min, uint64_t max, uint64_t* out)
{
    char* end = NULL;
    unsigned long long parsed;
    if (!value || value[0] == '\0' || value[0] == '-') return false;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < min || parsed > max) return false;
    *out = (uint64_t)parsed;
    return true;
}

static bool
parse_bool(const char* value, bool* out)
{
    if (!value || !out) return false;
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) *out = true;
    else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) *out = false;
    else return false;
    return true;
}

static zst_result_t
copy_address(char* destination, size_t size, const char* value, bool allow_empty,
             bool unicast_only)
{
    struct in_addr address;
    if (allow_empty && value && value[0] == '\0') {
        destination[0] = '\0';
        return ZST_OK;
    }
    if (!parse_ipv4(value, &address) ||
        (unicast_only ? !valid_unicast(address)
                      : (!valid_unicast(address) && !is_multicast(address))) ||
        strlen(value) >= size) return ZST_ERROR;
    strcpy(destination, value);
    return ZST_OK;
}

static void
sink_close_socket(dante_udp_sink_t* sink)
{
    if (sink->fd >= 0) {
        close(sink->fd);
        sink->fd = -1;
    }
}

static zst_result_t
sink_send_datagram(dante_udp_sink_t* sink, const zst_buffer_t* input)
{
    uint64_t attempt_ns = 0;
    uint64_t gap_ns = 0;
    if (sink->timing_observe) {
        attempt_ns = monotonic_time_ns();
        if (sink->timing_last_attempt_ns != 0) {
            gap_ns = attempt_ns - sink->timing_last_attempt_ns;
            sink->timing_gap_count++;
            sink->timing_gap_total_ns += gap_ns;
            sink->timing_gap_min_ns = metric_min(sink->timing_gap_min_ns, gap_ns);
            if (gap_ns > sink->timing_gap_max_ns) sink->timing_gap_max_ns = gap_ns;
            if (gap_ns <= 5000) sink->timing_gap_le_5us++;
            if (gap_ns <= 20000) sink->timing_gap_le_20us++;
            if (gap_ns <= 100000) sink->timing_gap_le_100us++;
        }
        sink->timing_current_burst_packets = gap_ns > 0 && gap_ns <= 100000
            ? sink->timing_current_burst_packets + 1 : 1;
        sink->timing_last_attempt_ns = attempt_ns;
    }
    ssize_t sent = sendto(sink->fd, input->memory.data, input->memory.size, 0,
                          (struct sockaddr*)&sink->destination, sizeof(sink->destination));
    if (sink->timing_observe) {
        uint64_t duration_ns = monotonic_time_ns() - attempt_ns;
        sink->timing_duration_total_ns += duration_ns;
        sink->timing_duration_min_ns = metric_min(sink->timing_duration_min_ns, duration_ns);
        if (duration_ns > sink->timing_duration_max_ns) sink->timing_duration_max_ns = duration_ns;
    }
    if (sent >= 0 && (size_t)sent == input->memory.size) {
        sink->packets_sent++;
        sink->bytes_sent += (uint64_t)sent;
        sink->last_packet_size = (uint64_t)sent;
        return ZST_OK;
    }
    sink->send_errors++;
    if (sent < 0) {
        int code = errno;
        sink->send_last_errno = (uint64_t)code;
        if (code == EAGAIN || code == EWOULDBLOCK) {
            sink->send_errors_eagain++;
            if (sink->timing_observe) {
                sink->eagain_last_gap_ns = gap_ns;
                sink->eagain_last_burst_packets = sink->timing_current_burst_packets;
                if (sink->timing_current_burst_packets > sink->eagain_max_burst_packets)
                    sink->eagain_max_burst_packets = sink->timing_current_burst_packets;
            }
        }
        else if (code == EINTR) sink->send_errors_eintr++;
        else sink->send_errors_other++;
        time_t now = time(NULL);
        if (now != sink->last_error_log_sec) {
            sink->last_error_log_sec = now;
            fprintf(stderr, "[danteudpsink tx=%s:%u] sendto failed: %s (errno=%d) size=%llu\n",
                    sink->destination_address, (unsigned)sink->port, strerror(code), code,
                    (unsigned long long)input->memory.size);
        }
    } else {
        sink->send_errors_other++;
    }
    return (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        ? ZST_TIMEOUT : ZST_ERROR;
}

static zst_result_t
sink_open(zst_element_t* element)
{
    dante_udp_sink_t* sink = element->priv;
    struct in_addr transmitter = { .s_addr = htonl(INADDR_ANY) };
    struct sockaddr_in local = {0};
    if (!parse_ipv4(sink->destination_address, &sink->destination.sin_addr) ||
        sink->port == 0) return ZST_ERROR;
    if (sink->transmitter_address[0] != '\0' &&
        (!parse_ipv4(sink->transmitter_address, &transmitter) ||
         !valid_unicast(transmitter))) return ZST_ERROR;

    sink_close_socket(sink);
    sink->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sink->fd < 0) return ZST_ERROR;
    if (sink->send_buffer_request > 0) {
        int requested = (int)sink->send_buffer_request;
        (void)setsockopt(sink->fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
    }
    int flags = fcntl(sink->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(sink->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        sink_close_socket(sink);
        return ZST_ERROR;
    }
    local.sin_family = AF_INET;
    local.sin_addr = transmitter;
    local.sin_port = 0;
    if (bind(sink->fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        sink_close_socket(sink);
        return ZST_ERROR;
    }
    sink->destination.sin_family = AF_INET;
    sink->destination.sin_port = htons(sink->port);

    if (is_multicast(sink->destination.sin_addr)) {
        struct in_addr interface_address = transmitter;
        if (sink->multicast_interface_address[0] != '\0' &&
            (!parse_ipv4(sink->multicast_interface_address, &interface_address) ||
             !valid_unicast(interface_address))) {
            sink_close_socket(sink);
            return ZST_ERROR;
        }
        unsigned char ttl = sink->ttl;
        unsigned char loop = sink->loop ? 1 : 0;
        if (setsockopt(sink->fd, IPPROTO_IP, IP_MULTICAST_IF,
                       &interface_address, sizeof(interface_address)) < 0 ||
            setsockopt(sink->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
            setsockopt(sink->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
            sink_close_socket(sink);
            return ZST_ERROR;
        }
    }

    if (!sink->pacer_initialized) {
        zst_timestamp_pacer_init(&sink->pacer);
        sink->pacer_initialized = true;
    }
    zst_timestamp_pacer_set_enabled(&sink->pacer, sink->timestamp_pacing);
    zst_timestamp_pacer_reset(&sink->pacer);
    uint32_t send_buffer_size = 0;
    socklen_t send_buffer_size_len = sizeof(send_buffer_size);
    if (getsockopt(sink->fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
                   &send_buffer_size_len) != 0)
        send_buffer_size = 0;
    sink->send_buffer_size = send_buffer_size;
    sink->packets_sent = 0;
    sink->bytes_sent = 0;
    sink->send_errors = 0;
    sink->last_packet_size = 0;
    sink->send_errors_eagain = 0;
    sink->send_errors_eintr = 0;
    sink->send_errors_other = 0;
    sink->send_last_errno = 0;
    sink->last_error_log_sec = 0;
    sink->timing_last_attempt_ns = 0;
    sink->timing_current_burst_packets = 0;
    sink->timing_gap_count = 0;
    sink->timing_gap_total_ns = 0;
    sink->timing_gap_min_ns = UINT64_MAX;
    sink->timing_gap_max_ns = 0;
    sink->timing_gap_le_5us = 0;
    sink->timing_gap_le_20us = 0;
    sink->timing_gap_le_100us = 0;
    sink->timing_duration_total_ns = 0;
    sink->timing_duration_min_ns = UINT64_MAX;
    sink->timing_duration_max_ns = 0;
    sink->eagain_last_gap_ns = 0;
    sink->eagain_last_burst_packets = 0;
    sink->eagain_max_burst_packets = 0;
    return ZST_OK;
}

static zst_result_t
sink_close(zst_element_t* element)
{
    dante_udp_sink_t* sink = element->priv;
    sink_close_socket(sink);
    if (sink->pacer_initialized) {
        zst_timestamp_pacer_deinit(&sink->pacer);
        sink->pacer_initialized = false;
    }
    return ZST_OK;
}

static zst_result_t
sink_start(zst_element_t* element)
{
    dante_udp_sink_t* sink = element->priv;
    if (sink->pacer_initialized) zst_timestamp_pacer_reset(&sink->pacer);
    return ZST_OK;
}

static zst_result_t
sink_stop(zst_element_t* element)
{
    dante_udp_sink_t* sink = element->priv;
    if (sink->pacer_initialized) zst_timestamp_pacer_reset(&sink->pacer);
    return ZST_OK;
}

static zst_result_t
sink_process(zst_element_t* element, zst_buffer_t* input, zst_buffer_t** output)
{
    dante_udp_sink_t* sink = element->priv;
    if (output) *output = NULL;
    if (sink->fd < 0) return ZST_ERROR;
    /* NULL input means no data — treat as idle, not an error. */
    if (!input) return ZST_OK;
    if (!input->memory.data && input->memory.size != 0) return ZST_ERROR;
    if (input->memory.size > 65507) {
        sink->send_errors++;
        return ZST_ERROR;
    }
    if (sink->timestamp_pacing && sink->pacer_initialized) {
        int dropped = 0;
        zst_time_t timestamp = input->dts ? input->dts : input->pts;
        zst_result_t result = zst_timestamp_pacer_wait(&sink->pacer, element->clock,
                                                        timestamp, &dropped);
        if (result != ZST_OK) {
            if (result != ZST_AGAIN) sink->send_errors++;
            return result == ZST_AGAIN ? ZST_OK : result;
        }
    }
    return sink_send_datagram(sink, input);
}

static zst_result_t
sink_set_property(zst_element_t* element, const char* name, const char* value)
{
    dante_udp_sink_t* sink = element->priv;
    uint64_t number;
    bool boolean;
    if (!name || !value) return ZST_ERROR;
    if (strcmp(name, "destination-address") == 0)
        return copy_address(sink->destination_address, sizeof(sink->destination_address), value, false, false);
    if (strcmp(name, "transmitter-address") == 0)
        return copy_address(sink->transmitter_address, sizeof(sink->transmitter_address), value, true, true);
    if (strcmp(name, "multicast-interface-address") == 0)
        return copy_address(sink->multicast_interface_address, sizeof(sink->multicast_interface_address), value, true, true);
    if (strcmp(name, "port") == 0) {
        if (!parse_uint(value, 1, 65535, &number)) return ZST_ERROR;
        sink->port = (uint16_t)number;
        return ZST_OK;
    }
    if (strcmp(name, "send-buffer-size") == 0) {
        if (!parse_uint(value, 0, 134217728, &number)) return ZST_ERROR;
        sink->send_buffer_request = (uint32_t)number;
        return ZST_OK;
    }
    if (strcmp(name, "ttl") == 0) {
        if (!parse_uint(value, 0, 255, &number)) return ZST_ERROR;
        sink->ttl = (uint8_t)number;
        return ZST_OK;
    }
    if (strcmp(name, "loop") == 0) {
        if (!parse_bool(value, &boolean)) return ZST_ERROR;
        sink->loop = boolean;
        return ZST_OK;
    }
    if (strcmp(name, "timestamp-pacing") == 0) {
        if (!parse_bool(value, &boolean)) return ZST_ERROR;
        sink->timestamp_pacing = boolean;
        if (sink->pacer_initialized) zst_timestamp_pacer_set_enabled(&sink->pacer, boolean);
        return ZST_OK;
    }
    if (strcmp(name, "timing-observe") == 0) {
        if (!parse_bool(value, &boolean)) return ZST_ERROR;
        sink->timing_observe = boolean;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
sink_get_property(zst_element_t* element, const char* name, char* out, size_t size)
{
    dante_udp_sink_t* sink = element->priv;
    if (!name || !out || size == 0) return ZST_ERROR;
#define RETURN_STRING(v) do { snprintf(out, size, "%s", (v)); return ZST_OK; } while (0)
#define RETURN_UINT(v) do { snprintf(out, size, "%llu", (unsigned long long)(v)); return ZST_OK; } while (0)
    if (strcmp(name, "destination-address") == 0) RETURN_STRING(sink->destination_address);
    if (strcmp(name, "transmitter-address") == 0) RETURN_STRING(sink->transmitter_address);
    if (strcmp(name, "multicast-interface-address") == 0)
        RETURN_STRING(sink->multicast_interface_address[0] ? sink->multicast_interface_address : sink->transmitter_address);
    if (strcmp(name, "port") == 0) RETURN_UINT(sink->port);
    if (strcmp(name, "ttl") == 0) RETURN_UINT(sink->ttl);
    if (strcmp(name, "loop") == 0) RETURN_STRING(sink->loop ? "true" : "false");
    if (strcmp(name, "timestamp-pacing") == 0) RETURN_STRING(sink->timestamp_pacing ? "true" : "false");
    if (strcmp(name, "timing-observe") == 0) RETURN_STRING(sink->timing_observe ? "true" : "false");
    if (strcmp(name, "packets-sent") == 0) RETURN_UINT(sink->packets_sent);
    if (strcmp(name, "bytes-sent") == 0) RETURN_UINT(sink->bytes_sent);
    if (strcmp(name, "send-errors") == 0) RETURN_UINT(sink->send_errors);
    if (strcmp(name, "send-errors-eagain") == 0) RETURN_UINT(sink->send_errors_eagain);
    if (strcmp(name, "send-errors-eintr") == 0) RETURN_UINT(sink->send_errors_eintr);
    if (strcmp(name, "send-errors-other") == 0) RETURN_UINT(sink->send_errors_other);
    if (strcmp(name, "send-last-errno") == 0) RETURN_UINT(sink->send_last_errno);
    if (strcmp(name, "send-buffer-size") == 0) RETURN_UINT(sink->send_buffer_size);
    if (strcmp(name, "last-packet-size") == 0) RETURN_UINT(sink->last_packet_size);
    if (strcmp(name, "send-gap-count") == 0) RETURN_UINT(sink->timing_gap_count);
    if (strcmp(name, "send-gap-total-ns") == 0) RETURN_UINT(sink->timing_gap_total_ns);
    if (strcmp(name, "send-gap-min-ns") == 0) RETURN_UINT(sink->timing_gap_min_ns == UINT64_MAX ? 0 : sink->timing_gap_min_ns);
    if (strcmp(name, "send-gap-max-ns") == 0) RETURN_UINT(sink->timing_gap_max_ns);
    if (strcmp(name, "send-gap-le-5us") == 0) RETURN_UINT(sink->timing_gap_le_5us);
    if (strcmp(name, "send-gap-le-20us") == 0) RETURN_UINT(sink->timing_gap_le_20us);
    if (strcmp(name, "send-gap-le-100us") == 0) RETURN_UINT(sink->timing_gap_le_100us);
    if (strcmp(name, "send-duration-total-ns") == 0) RETURN_UINT(sink->timing_duration_total_ns);
    if (strcmp(name, "send-duration-min-ns") == 0) RETURN_UINT(sink->timing_duration_min_ns == UINT64_MAX ? 0 : sink->timing_duration_min_ns);
    if (strcmp(name, "send-duration-max-ns") == 0) RETURN_UINT(sink->timing_duration_max_ns);
    if (strcmp(name, "eagain-last-gap-ns") == 0) RETURN_UINT(sink->eagain_last_gap_ns);
    if (strcmp(name, "eagain-last-burst-packets") == 0) RETURN_UINT(sink->eagain_last_burst_packets);
    if (strcmp(name, "eagain-max-burst-packets") == 0) RETURN_UINT(sink->eagain_max_burst_packets);
#undef RETURN_STRING
#undef RETURN_UINT
    return ZST_ERROR;
}

static zst_element_ops_t sink_ops = {
    .name = "danteudpsink",
    .open = sink_open,
    .close = sink_close,
    .start = sink_start,
    .stop = sink_stop,
    .process = sink_process,
    .set_property = sink_set_property,
    .get_property = sink_get_property
};

zst_element_t*
zst_dante_udp_sink_create(void)
{
    dante_udp_sink_t* sink = calloc(1, sizeof(*sink));
    zst_element_t* element;
    zst_pad_t* pad;
    if (!sink) return NULL;
    sink->fd = -1;
    sink->port = 5004;
    sink->ttl = 1;
    sink->loop = false;
    element = zst_element_create(&sink_ops, sink);
    if (!element) {
        free(sink);
        return NULL;
    }
    pad = zst_pad_create("sink", ZST_PAD_SINK);
    if (!pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        zst_element_destroy(element);
        return NULL;
    }
    return element;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    return name && strcmp(name, "danteudpsink") == 0 ? zst_dante_udp_sink_create() : NULL;
}
static const zst_property_spec_t sink_properties[] = {
    { "destination-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Destination IPv4 address" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5004", "UDP destination port" },
    { "transmitter-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Optional local transmitter IPv4 address; empty binds to 0.0.0.0" },
    { "multicast-interface-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Multicast interface; transmitter address when unset" },
    { "ttl", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "IPv4 multicast TTL" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Enable multicast loopback" },
    { "timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Pace sends from buffer timestamps" },
    { "timing-observe", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Collect UDP send timing metrics" },
    { "packets-sent", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Successfully sent datagrams" },
    { "bytes-sent", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Successfully sent bytes" },
    { "send-errors", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Failed datagram sends" },
    { "send-errors-eagain", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Send failures with EAGAIN/EWOULDBLOCK" },
    { "send-errors-eintr", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Send failures with EINTR" },
    { "send-errors-other", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Send failures with other errno" },
    { "send-last-errno", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "errno of the most recent send failure" },
    { "send-buffer-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Socket SO_SNDBUF request; 0 uses system default" },
    { "last-packet-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Most recent sent datagram size" },
    { "send-gap-count", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Measured inter-attempt gaps" },
    { "send-gap-total-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Total inter-attempt gap time" },
    { "send-gap-min-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Minimum inter-attempt gap" },
    { "send-gap-max-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Maximum inter-attempt gap" },
    { "send-gap-le-5us", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Inter-attempt gaps at most 5 us" },
    { "send-gap-le-20us", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Inter-attempt gaps at most 20 us" },
    { "send-gap-le-100us", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Inter-attempt gaps at most 100 us" },
    { "send-duration-total-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Total sendto syscall duration" },
    { "send-duration-min-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Minimum sendto syscall duration" },
    { "send-duration-max-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Maximum sendto syscall duration" },
    { "eagain-last-gap-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Gap before the most recent EAGAIN" },
    { "eagain-last-burst-packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "100 us burst length at the most recent EAGAIN" },
    { "eagain-max-burst-packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Largest 100 us burst ending in EAGAIN" }
};
static const zst_pad_template_t sink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "application/octet-stream" }
};
static const zst_element_desc_t sink_elements[] = {{
    .name = "danteudpsink", .long_name = "Dante UDP Sink", .category = "Sink/Network",
    .description = "Sends one Dante IPv4 UDP media datagram per buffer", .author = "zstreamer",
    .properties = sink_properties, .nb_properties = sizeof(sink_properties) / sizeof(sink_properties[0]),
    .pads = sink_pads, .nb_pads = sizeof(sink_pads) / sizeof(sink_pads[0])
}};
static zst_plugin_t sink_plugin = {
    .desc = { .name = "danteudpsink_plugin", .author = "zstreamer", .version = "1.0.0" },
    .create_element = plugin_create_element
};
ZST_PLUGIN_EXPORT const zst_element_desc_t* zst_get_plugin_elements(uint32_t* count)
{
    if (count) *count = 1;
    return sink_elements;
}
ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* plugin = malloc(sizeof(*plugin));
    if (plugin) *plugin = sink_plugin;
    return plugin;
}
#endif
