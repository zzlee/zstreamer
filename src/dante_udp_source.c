/*=============================================================================
    dante_udp_source.c - Source-filtered Dante IPv4 UDP media receiver
=============================================================================*/
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_dante_udp_source.h"

typedef struct {
    int fd;
    char local_address[INET_ADDRSTRLEN];
    char multicast_address[INET_ADDRSTRLEN];
    char multicast_interface_address[INET_ADDRSTRLEN];
    char transmitter_address[INET_ADDRSTRLEN];
    uint16_t port;
    uint32_t read_timeout_ms;
    size_t max_datagram_size;
    struct in_addr group;
    struct in_addr interface_addr;
    struct in_addr transmitter;
    int membership;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t packets_rejected;
    uint64_t packets_truncated;
    char last_packet_address[INET_ADDRSTRLEN];
    uint16_t last_packet_port;
    uint64_t last_packet_size;
    _Atomic uint64_t last_packet_time_ns;
} dante_udp_source_t;

static zst_element_ops_t source_ops;

static uint64_t
monotonic_time_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
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
valid_host_address(struct in_addr address, bool allow_any)
{
    uint32_t host = ntohl(address.s_addr);
    return (allow_any || host != INADDR_ANY) && host != INADDR_BROADCAST &&
           !IN_MULTICAST(host);
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

static zst_result_t
copy_ipv4_property(char* destination, size_t size, const char* value,
                   bool allow_empty, bool require_multicast, bool allow_any)
{
    struct in_addr address;
    if (allow_empty && value && value[0] == '\0') {
        destination[0] = '\0';
        return ZST_OK;
    }
    if (!parse_ipv4(value, &address)) return ZST_ERROR;
    if (require_multicast != is_multicast(address)) return ZST_ERROR;
    if (!require_multicast && !valid_host_address(address, allow_any)) return ZST_ERROR;
    if (strlen(value) >= size) return ZST_ERROR;
    strcpy(destination, value);
    return ZST_OK;
}

static void
source_leave_and_close(dante_udp_source_t* source)
{
    if (!source || source->fd < 0) return;
    if (source->membership == 2) {
#ifdef IP_DROP_SOURCE_MEMBERSHIP
        struct ip_mreq_source request = {
            .imr_multiaddr = source->group,
            .imr_interface = source->interface_addr,
            .imr_sourceaddr = source->transmitter
        };
        (void)setsockopt(source->fd, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP,
                         &request, sizeof(request));
#endif
    } else if (source->membership == 1) {
        struct ip_mreq request = {
            .imr_multiaddr = source->group,
            .imr_interface = source->interface_addr
        };
        (void)setsockopt(source->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         &request, sizeof(request));
    }
    close(source->fd);
    source->fd = -1;
    source->membership = 0;
}

static zst_result_t
source_open(zst_element_t* element)
{
    dante_udp_source_t* source = element->priv;
    struct in_addr local;
    bool multicast = source->multicast_address[0] != '\0';
    struct sockaddr_in bind_address = {0};

    if (!parse_ipv4(source->local_address, &local) || !valid_host_address(local, true) ||
        !parse_ipv4(source->transmitter_address, &source->transmitter) ||
        !valid_host_address(source->transmitter, false) || source->port == 0 ||
        source->max_datagram_size == 0) {
        return ZST_ERROR;
    }
    if (multicast) {
        if (!parse_ipv4(source->multicast_address, &source->group) ||
            !is_multicast(source->group) ||
            !parse_ipv4(source->multicast_interface_address, &source->interface_addr) ||
            !valid_host_address(source->interface_addr, true)) return ZST_ERROR;
    }

    source_leave_and_close(source);
    source->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (source->fd < 0) return ZST_ERROR;

    int reuse = 1;
    (void)setsockopt(source->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int flags = fcntl(source->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(source->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        source_leave_and_close(source);
        return ZST_ERROR;
    }

    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(source->port);
    bind_address.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : local.s_addr;
    if (bind(source->fd, (struct sockaddr*)&bind_address, sizeof(bind_address)) < 0) {
        source_leave_and_close(source);
        return ZST_ERROR;
    }

    if (multicast) {
#ifdef IP_ADD_SOURCE_MEMBERSHIP
        struct ip_mreq_source source_request = {
            .imr_multiaddr = source->group,
            .imr_interface = source->interface_addr,
            .imr_sourceaddr = source->transmitter
        };
        if (setsockopt(source->fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                       &source_request, sizeof(source_request)) == 0) {
            source->membership = 2;
        }
#endif
        if (source->membership == 0) {
            struct ip_mreq request = {
                .imr_multiaddr = source->group,
                .imr_interface = source->interface_addr
            };
            if (setsockopt(source->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           &request, sizeof(request)) < 0) {
                source_leave_and_close(source);
                return ZST_ERROR;
            }
            source->membership = 1;
        }
    }

    source->packets_received = 0;
    source->bytes_received = 0;
    source->packets_rejected = 0;
    source->packets_truncated = 0;
    source->last_packet_address[0] = '\0';
    source->last_packet_port = 0;
    source->last_packet_size = 0;
    atomic_store_explicit(&source->last_packet_time_ns, 0, memory_order_release);
    return ZST_OK;
}

static zst_result_t
source_close(zst_element_t* element)
{
    source_leave_and_close(element->priv);
    return ZST_OK;
}

static zst_result_t
source_process(zst_element_t* element, zst_buffer_t* input, zst_buffer_t** output)
{
    dante_udp_source_t* source = element->priv;
    struct sockaddr_in sender = {0};
    socklen_t sender_size = sizeof(sender);
    struct pollfd descriptor;
    zst_buffer_t* buffer;
    ssize_t received;
    (void)input;

    if (!output || source->fd < 0) return ZST_ERROR;
    *output = NULL;
    descriptor.fd = source->fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    /* A source can be removed while a scheduler worker is in process().
     * Slice even deliberately long configured waits so teardown is bounded. */
    int wait_ms = source->read_timeout_ms > 50 ? 50 : (int)source->read_timeout_ms;
    int ready = poll(&descriptor, 1, wait_ms);
    if (ready == 0 || (ready < 0 && errno == EINTR)) return ZST_TIMEOUT;
    if (ready < 0 || !(descriptor.revents & POLLIN)) return ZST_ERROR;

    buffer = zst_buffer_create(ZST_BUFFER_USER);
    if (!buffer) return ZST_ERROR;
    buffer->memory.data = malloc(source->max_datagram_size);
    if (!buffer->memory.data) {
        zst_buffer_unref(buffer);
        return ZST_ERROR;
    }
    buffer->memory.priv = buffer->memory.data;
    buffer->memory.release = free;
    received = recvfrom(source->fd, buffer->memory.data, source->max_datagram_size,
                        MSG_TRUNC, (struct sockaddr*)&sender, &sender_size);
    if (received < 0) {
        zst_buffer_unref(buffer);
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            ? ZST_TIMEOUT : ZST_ERROR;
    }

    if (!inet_ntop(AF_INET, &sender.sin_addr, source->last_packet_address,
                   sizeof(source->last_packet_address))) {
        source->last_packet_address[0] = '\0';
    }
    source->last_packet_port = ntohs(sender.sin_port);
    source->last_packet_size = (uint64_t)received;
    if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != source->transmitter.s_addr) {
        source->packets_rejected++;
        zst_buffer_unref(buffer);
        return ZST_TIMEOUT;
    }
    if ((size_t)received > source->max_datagram_size) {
        source->packets_truncated++;
        zst_buffer_unref(buffer);
        return ZST_TIMEOUT;
    }

    buffer->memory.size = (size_t)received;
    buffer->pts = element->clock ? zst_clock_get_time(element->clock) : 0;
    source->packets_received++;
    source->bytes_received += (uint64_t)received;
    atomic_store_explicit(&source->last_packet_time_ns, monotonic_time_ns(),
                          memory_order_release);
    *output = buffer;
    return ZST_OK;
}

static zst_result_t
source_set_property(zst_element_t* element, const char* name, const char* value)
{
    dante_udp_source_t* source = element->priv;
    uint64_t number;
    if (!name || !value) return ZST_ERROR;
    if (strcmp(name, "local-address") == 0)
        return copy_ipv4_property(source->local_address, sizeof(source->local_address), value, false, false, true);
    if (strcmp(name, "multicast-address") == 0)
        return copy_ipv4_property(source->multicast_address, sizeof(source->multicast_address), value, true, true, false);
    if (strcmp(name, "multicast-interface-address") == 0)
        return copy_ipv4_property(source->multicast_interface_address, sizeof(source->multicast_interface_address), value, false, false, true);
    if (strcmp(name, "transmitter-address") == 0)
        return copy_ipv4_property(source->transmitter_address, sizeof(source->transmitter_address), value, false, false, false);
    if (strcmp(name, "port") == 0) {
        if (!parse_uint(value, 1, 65535, &number)) return ZST_ERROR;
        source->port = (uint16_t)number;
        return ZST_OK;
    }
    if (strcmp(name, "read-timeout-ms") == 0) {
        if (!parse_uint(value, 0, 60000, &number)) return ZST_ERROR;
        source->read_timeout_ms = (uint32_t)number;
        return ZST_OK;
    }
    if (strcmp(name, "max-datagram-size") == 0) {
        if (!parse_uint(value, 1, 65535, &number)) return ZST_ERROR;
        source->max_datagram_size = (size_t)number;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
source_get_property(zst_element_t* element, const char* name, char* out, size_t size)
{
    dante_udp_source_t* source = element->priv;
    if (!name || !out || size == 0) return ZST_ERROR;
#define RETURN_STRING(v) do { snprintf(out, size, "%s", (v)); return ZST_OK; } while (0)
#define RETURN_UINT(v) do { snprintf(out, size, "%llu", (unsigned long long)(v)); return ZST_OK; } while (0)
    if (strcmp(name, "local-address") == 0) RETURN_STRING(source->local_address);
    if (strcmp(name, "multicast-address") == 0) RETURN_STRING(source->multicast_address);
    if (strcmp(name, "multicast-interface-address") == 0) RETURN_STRING(source->multicast_interface_address);
    if (strcmp(name, "transmitter-address") == 0) RETURN_STRING(source->transmitter_address);
    if (strcmp(name, "port") == 0) RETURN_UINT(source->port);
    if (strcmp(name, "read-timeout-ms") == 0) RETURN_UINT(source->read_timeout_ms);
    if (strcmp(name, "max-datagram-size") == 0) RETURN_UINT(source->max_datagram_size);
    if (strcmp(name, "packets-received") == 0) RETURN_UINT(source->packets_received);
    if (strcmp(name, "bytes-received") == 0) RETURN_UINT(source->bytes_received);
    if (strcmp(name, "packets-rejected") == 0) RETURN_UINT(source->packets_rejected);
    if (strcmp(name, "packets-truncated") == 0) RETURN_UINT(source->packets_truncated);
    if (strcmp(name, "last-packet-address") == 0) RETURN_STRING(source->last_packet_address);
    if (strcmp(name, "last-packet-port") == 0) RETURN_UINT(source->last_packet_port);
    if (strcmp(name, "last-packet-size") == 0) RETURN_UINT(source->last_packet_size);
    if (strcmp(name, "last-packet-time-ns") == 0)
        RETURN_UINT(atomic_load_explicit(&source->last_packet_time_ns, memory_order_acquire));
#undef RETURN_STRING
#undef RETURN_UINT
    return ZST_ERROR;
}

static zst_element_ops_t source_ops = {
    .name = "danteudpsrc",
    .open = source_open,
    .close = source_close,
    .process = source_process,
    .set_property = source_set_property,
    .get_property = source_get_property
};

zst_element_t*
zst_dante_udp_source_create(void)
{
    dante_udp_source_t* source = calloc(1, sizeof(*source));
    zst_element_t* element;
    zst_pad_t* pad;
    if (!source) return NULL;
    source->fd = -1;
    strcpy(source->local_address, "0.0.0.0");
    strcpy(source->multicast_interface_address, "0.0.0.0");
    source->port = 5004;
    source->read_timeout_ms = 10;
    source->max_datagram_size = 65535;
    element = zst_element_create(&source_ops, source);
    if (!element) {
        free(source);
        return NULL;
    }
    pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        zst_element_destroy(element);
        return NULL;
    }
    return element;
}

uint64_t
zst_dante_udp_source_get_last_packet_time_ns(zst_element_t* element)
{
    if (!element || element->ops != &source_ops || !element->priv) return 0;
    dante_udp_source_t* source = element->priv;
    return atomic_load_explicit(&source->last_packet_time_ns, memory_order_acquire);
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    return name && strcmp(name, "danteudpsrc") == 0 ? zst_dante_udp_source_create() : NULL;
}

static const zst_property_spec_t source_properties[] = {
    { "local-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0.0.0.0", "Local unicast bind address" },
    { "port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5004", "UDP destination port" },
    { "multicast-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Optional IPv4 multicast group" },
    { "multicast-interface-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0.0.0.0", "IPv4 multicast receive interface" },
    { "transmitter-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Required accepted transmitter IPv4 address" },
    { "read-timeout-ms", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "Bounded receive wait in milliseconds" },
    { "max-datagram-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "65535", "Maximum accepted datagram size" },
    { "packets-received", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Accepted datagrams" },
    { "bytes-received", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Accepted datagram bytes" },
    { "packets-rejected", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Datagrams rejected by transmitter filtering" },
    { "packets-truncated", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Oversized datagrams discarded" },
    { "last-packet-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE, "", "Most recent sender IPv4 address" },
    { "last-packet-port", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Most recent sender UDP port" },
    { "last-packet-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Most recent original datagram size" },
    { "last-packet-time-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Monotonic timestamp of most recent accepted datagram" }
};
static const zst_pad_template_t source_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "application/octet-stream" }
};
static const zst_element_desc_t source_elements[] = {{
    .name = "danteudpsrc", .long_name = "Dante UDP Source", .category = "Source/Network",
    .description = "Receives source-filtered Dante IPv4 UDP media datagrams", .author = "zstreamer",
    .properties = source_properties, .nb_properties = sizeof(source_properties) / sizeof(source_properties[0]),
    .pads = source_pads, .nb_pads = sizeof(source_pads) / sizeof(source_pads[0])
}};
static zst_plugin_t source_plugin = {
    .desc = { .name = "danteudpsrc_plugin", .author = "zstreamer", .version = "1.0.0" },
    .create_element = plugin_create_element
};
ZST_PLUGIN_EXPORT const zst_element_desc_t* zst_get_plugin_elements(uint32_t* count)
{
    if (count) *count = 1;
    return source_elements;
}
ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* plugin = malloc(sizeof(*plugin));
    if (plugin) *plugin = source_plugin;
    return plugin;
}
#endif
