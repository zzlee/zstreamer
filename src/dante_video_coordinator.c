/*=============================================================================
    dante_video_coordinator.c - Dynamic Dante H.264 video flow coordinator
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zstreamer/elements/zst_dante_video_coordinator.h"
#include "zstreamer/elements/zst_dante_session.h"
#include "zstreamer/elements/zst_dante_udp_sink.h"
#include "zstreamer/elements/zst_dante_udp_source.h"
#include "zstreamer/elements/zst_rtp_depayloader.h"
#include "zstreamer/elements/zst_rtp_payloader.h"
#include "zst_buffer.h"
#include "zst_pad_event.h"
#include "zst_pipeline.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct dante_video_coordinator dante_video_coordinator_t;
typedef struct dante_video_route dante_video_route_t;

typedef struct dante_video_channel {
    uint32_t index;
    zst_dante_flow_direction_t direction;
    zst_pad_t* pad;
    uint8_t* sps;
    size_t sps_size;
    uint8_t* pps;
    size_t pps_size;
    int tx_status_initialized;
    int tx_was_linked;
    struct dante_video_channel* next;
} dante_video_channel_t;

typedef struct {
    dante_video_coordinator_t* coordinator;
    dante_video_channel_t* channel;
} channel_pad_context_t;

typedef struct {
    zst_buffer_t* buffer;
    uint16_t sequence;
    uint64_t arrival_ns;
} reorder_slot_t;

typedef struct {
    dante_video_route_t* route;
    zst_pad_t* src_pad;
    reorder_slot_t* slots;
    uint32_t window;
    uint64_t timeout_ns;
    uint16_t expected;
    int have_expected;
    uint32_t locked_ssrc;
    pthread_mutex_t lock;
    uint64_t ssrc_inactive_ns;
} dante_reorder_t;

typedef struct {
    dante_video_route_t* route;
    zst_pad_t* output_pad;
} dante_output_t;

struct dante_video_route {
    zst_dante_flow_direction_t direction;
    zst_dante_flow_transport_t transport;
    uint32_t flow_index;
    uint32_t channel_index;
    uint16_t port;
    char receiver_address[INET_ADDRSTRLEN];
    char multicast_address[INET_ADDRSTRLEN];
    char transmitter_address[INET_ADDRSTRLEN];
    zst_element_t* first;
    zst_element_t* second;
    zst_element_t* third;
    zst_element_t* fourth;
    zst_pad_t* tx_pay_sink;
    _Atomic int active;
    int rx_receiving;
    int rx_status_initialized;
    int parameter_sets_pending;
    _Atomic uint64_t last_valid_rtp_time_ns;
    struct dante_video_route* next;
};

struct dante_video_coordinator {
    pthread_mutex_t lock;
    pthread_cond_t watchdog_cond;
    pthread_t watchdog_thread;
    int watchdog_started;
    int watchdog_stop;
    zst_element_t* element;
    zst_element_t* session;
    dante_video_channel_t* channels;
    dante_video_route_t* routes;
    uint32_t flow_count;
    uint32_t health_timeout_ms;
    uint32_t reorder_window;
    uint32_t reorder_timeout_ms;
    char multicast_interface_address[INET_ADDRSTRLEN];
};

static const zst_element_ops_t coordinator_ops;

static void
channel_pad_destroy(zst_pad_t* pad)
{
    channel_pad_context_t* context = pad ? pad->priv : NULL;
    if (!context) return;
    dante_video_coordinator_t* coordinator = context->coordinator;
    dante_video_channel_t* channel = context->channel;
    pthread_mutex_lock(&coordinator->lock);
    dante_video_channel_t** link = &coordinator->channels;
    while (*link && *link != channel) link = &(*link)->next;
    if (*link) *link = channel->next;
    pthread_mutex_unlock(&coordinator->lock);
    free(channel->sps);
    free(channel->pps);
    free(channel);
    free(context);
    pad->priv = NULL;
}

static uint64_t
monotonic_time_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int
parse_uint(const char* value, uint64_t minimum, uint64_t maximum, uint64_t* out)
{
    char* end = NULL;
    unsigned long long number;
    if (!value || !*value || *value == '-') return 0;
    errno = 0;
    number = strtoull(value, &end, 10);
    if (errno || !end || *end || number < minimum || number > maximum) return 0;
    *out = (uint64_t)number;
    return 1;
}

static int
valid_ipv4(const char* value, int multicast, int allow_any)
{
    struct in_addr address;
    if (!value || !*value || inet_pton(AF_INET, value, &address) != 1) return 0;
    uint32_t host = ntohl(address.s_addr);
    if (multicast) return IN_MULTICAST(host) != 0;
    return !IN_MULTICAST(host) && host != INADDR_BROADCAST &&
           (allow_any || host != INADDR_ANY);
}

static int
empty_address(const char* value)
{
    return !value || !*value;
}

static int
valid_flow_shape(const zst_dante_flow_t* flow)
{
    if (!flow || flow->port == 0 ||
        (flow->direction != ZST_DANTE_FLOW_TX && flow->direction != ZST_DANTE_FLOW_RX) ||
        (flow->transport != ZST_DANTE_FLOW_UNICAST &&
         flow->transport != ZST_DANTE_FLOW_MULTICAST) ||
        !valid_ipv4(flow->transmitter_address, 0, 0)) return 0;
    if (flow->transport == ZST_DANTE_FLOW_UNICAST) {
        return valid_ipv4(flow->receiver_address, 0, 0) &&
               empty_address(flow->multicast_address);
    }
    return valid_ipv4(flow->multicast_address, 1, 0) &&
           empty_address(flow->receiver_address);
}

static dante_video_channel_t*
find_channel(dante_video_coordinator_t* coordinator,
             zst_dante_flow_direction_t direction, uint32_t index)
{
    for (dante_video_channel_t* channel = coordinator->channels; channel;
         channel = channel->next) {
        if (channel->direction == direction && channel->index == index) return channel;
    }
    return NULL;
}

static dante_video_route_t*
find_route(dante_video_coordinator_t* coordinator,
           zst_dante_flow_direction_t direction, uint32_t flow_index)
{
    for (dante_video_route_t* route = coordinator->routes; route; route = route->next) {
        if (route->direction == direction && route->flow_index == flow_index) return route;
    }
    return NULL;
}

static int
channel_has_flow(dante_video_coordinator_t* coordinator,
                 zst_dante_flow_direction_t direction, uint32_t channel_index)
{
    for (dante_video_route_t* route = coordinator->routes; route; route = route->next) {
        if (route->direction == direction && route->channel_index == channel_index) return 1;
    }
    return 0;
}

static void
copy_flow(dante_video_route_t* route, const zst_dante_flow_t* flow)
{
    route->direction = flow->direction;
    route->transport = flow->transport;
    route->flow_index = flow->flow_index;
    route->channel_index = flow->channel_index;
    route->port = flow->port;
    if (flow->receiver_address)
        snprintf(route->receiver_address, sizeof(route->receiver_address), "%s",
                 flow->receiver_address);
    if (flow->multicast_address)
        snprintf(route->multicast_address, sizeof(route->multicast_address), "%s",
                 flow->multicast_address);
    if (flow->transmitter_address)
        snprintf(route->transmitter_address, sizeof(route->transmitter_address), "%s",
                 flow->transmitter_address);
    atomic_init(&route->active, 0);
    atomic_init(&route->last_valid_rtp_time_ns, 0);
}

static uint32_t
random_u32(void)
{
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t amount = read(fd, &value, sizeof(value));
        close(fd);
        if (amount != (ssize_t)sizeof(value)) value = 0;
    }
    if (value == 0) {
        value = (uint32_t)monotonic_time_ns() ^ (uint32_t)(uintptr_t)&value ^
                (uint32_t)(uintptr_t)pthread_self();
        if (value == 0) value = 1;
    }
    return value;
}

static void
replace_parameter_set(uint8_t** destination, size_t* destination_size,
                      const uint8_t* nal, size_t nal_size)
{
    uint8_t* copy = malloc(nal_size + 4u);
    if (!copy) return;
    copy[0] = copy[1] = copy[2] = 0;
    copy[3] = 1;
    memcpy(copy + 4, nal, nal_size);
    free(*destination);
    *destination = copy;
    *destination_size = nal_size + 4u;
}

static void
cache_h264_parameter_sets(dante_video_channel_t* channel, const zst_buffer_t* buffer)
{
    if (!buffer || !buffer->memory.data || buffer->memory.size < 5) return;
    const uint8_t* data = buffer->memory.data;
    size_t size = buffer->memory.size;
    size_t cursor = 0;
    while (cursor + 3 < size) {
        size_t start = cursor;
        while (start + 3 < size &&
               !(data[start] == 0 && data[start + 1] == 0 &&
                 (data[start + 2] == 1 ||
                  (start + 3 < size && data[start + 2] == 0 && data[start + 3] == 1)))) {
            start++;
        }
        if (start + 3 >= size) break;
        size_t code = data[start + 2] == 1 ? 3u : 4u;
        size_t nal_start = start + code;
        size_t end = nal_start;
        while (end + 3 < size &&
               !(data[end] == 0 && data[end + 1] == 0 &&
                 (data[end + 2] == 1 ||
                  (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1)))) {
            end++;
        }
        if (end + 3 >= size) end = size;
        while (end > nal_start && data[end - 1] == 0) end--;
        if (end > nal_start) {
            uint8_t type = data[nal_start] & 0x1f;
            if (type == 7)
                replace_parameter_set(&channel->sps, &channel->sps_size,
                                      data + nal_start, end - nal_start);
            else if (type == 8)
                replace_parameter_set(&channel->pps, &channel->pps_size,
                                      data + nal_start, end - nal_start);
        }
        cursor = end;
    }
}

static zst_buffer_t*
make_parameter_set_buffer(const dante_video_channel_t* channel, uint64_t pts)
{
    if (!channel->sps || !channel->pps) return NULL;
    size_t size = channel->sps_size + channel->pps_size;
    zst_buffer_t* buffer = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!buffer) return NULL;
    uint8_t* data = malloc(size);
    if (!data) {
        zst_buffer_unref(buffer);
        return NULL;
    }
    memcpy(data, channel->sps, channel->sps_size);
    memcpy(data + channel->sps_size, channel->pps, channel->pps_size);
    buffer->memory.data = data;
    buffer->memory.priv = data;
    buffer->memory.release = free;
    buffer->memory.size = size;
    buffer->pts = pts;
    return buffer;
}

static zst_result_t
tx_channel_push(zst_pad_t* pad, zst_buffer_t* buffer)
{
    channel_pad_context_t* context = pad ? pad->priv : NULL;
    if (!context || !buffer) return ZST_ERROR;
    dante_video_coordinator_t* coordinator = context->coordinator;
    zst_result_t result = ZST_OK;
    pthread_mutex_lock(&coordinator->lock);
    cache_h264_parameter_sets(context->channel, buffer);
    for (dante_video_route_t* route = coordinator->routes; route; route = route->next) {
        if (route->direction != ZST_DANTE_FLOW_TX ||
            route->channel_index != context->channel->index ||
            !atomic_load_explicit(&route->active, memory_order_acquire)) continue;
        /* Parameter sets are emitted only with a subsequent access unit, so
         * they cannot acquire an unrelated RTP timestamp at flow creation. */
        const uint8_t* bytes = buffer->memory.data;
        int access_unit = bytes && buffer->memory.size >= 5 &&
                          (bytes[4] & 0x1f) >= 1 && (bytes[4] & 0x1f) <= 5;
        zst_buffer_t* parameter_sets = route->parameter_sets_pending && access_unit
            ? make_parameter_set_buffer(context->channel, buffer->pts) : NULL;
        if (parameter_sets && route->tx_pay_sink && route->tx_pay_sink->push) {
            (void)route->tx_pay_sink->push(route->tx_pay_sink, parameter_sets);
            zst_buffer_unref(parameter_sets);
            route->parameter_sets_pending = 0;
        }
        zst_result_t one = route->tx_pay_sink && route->tx_pay_sink->push
            ? route->tx_pay_sink->push(route->tx_pay_sink, buffer) : ZST_ERROR;
        if (one != ZST_OK) result = one;
    }
    pthread_mutex_unlock(&coordinator->lock);
    return result;
}

static int
parse_rtp_header(const zst_buffer_t* buffer, uint16_t* sequence, uint32_t* ssrc)
{
    if (!buffer || !buffer->memory.data || buffer->memory.size < 12) return 0;
    const uint8_t* packet = buffer->memory.data;
    if ((packet[0] & 0xc0) != 0x80 || (packet[1] & 0x7f) != 96) return 0;
    size_t header = 12u + (size_t)(packet[0] & 0x0f) * 4u;
    if (header > buffer->memory.size) return 0;
    if (packet[0] & 0x10) {
        if (header + 4u > buffer->memory.size) return 0;
        size_t words = ((size_t)packet[header + 2] << 8) | packet[header + 3];
        header += 4u + words * 4u;
        if (header > buffer->memory.size) return 0;
    }
    *sequence = (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
    *ssrc = ((uint32_t)packet[8] << 24) | ((uint32_t)packet[9] << 16) |
            ((uint32_t)packet[10] << 8) | packet[11];
    return *ssrc != 0;
}

static zst_result_t
reorder_emit(dante_reorder_t* reorder, reorder_slot_t* slot)
{
    zst_buffer_t* buffer = slot->buffer;
    slot->buffer = NULL;
    zst_result_t result = zst_pad_push(reorder->src_pad, buffer);
    zst_buffer_unref(buffer);
    reorder->expected++;
    return result;
}

static zst_result_t
reorder_drain(dante_reorder_t* reorder)
{
    zst_result_t result = ZST_OK;
    while (reorder->have_expected) {
        reorder_slot_t* slot = &reorder->slots[reorder->expected % reorder->window];
        if (!slot->buffer || slot->sequence != reorder->expected) break;
        zst_result_t one = reorder_emit(reorder, slot);
        if (one != ZST_OK) result = one;
    }
    return result;
}

static void
reorder_skip_gap_if_timed_out(dante_reorder_t* reorder, uint64_t now)
{
    uint16_t nearest = 0;
    uint16_t nearest_distance = UINT16_MAX;
    int found = 0;
    for (uint32_t i = 0; i < reorder->window; i++) {
        reorder_slot_t* slot = &reorder->slots[i];
        if (!slot->buffer || now - slot->arrival_ns < reorder->timeout_ns) continue;
        uint16_t distance = (uint16_t)(slot->sequence - reorder->expected);
        if (distance < nearest_distance) {
            nearest = slot->sequence;
            nearest_distance = distance;
            found = 1;
        }
    }
    if (found) reorder->expected = nearest;
}

static zst_result_t
reorder_push(zst_pad_t* pad, zst_buffer_t* buffer)
{
    dante_reorder_t* reorder = pad && pad->parent ? pad->parent->priv : NULL;
    uint16_t sequence;
    uint32_t ssrc;
    if (!reorder || !parse_rtp_header(buffer, &sequence, &ssrc)) return ZST_OK;
    pthread_mutex_lock(&reorder->lock);
    uint64_t now = monotonic_time_ns();
    if (reorder->locked_ssrc == 0) {
        reorder->locked_ssrc = ssrc;
        if (zst_element_set_property_uint(reorder->route->third, "ssrc", ssrc) != ZST_OK) {
            pthread_mutex_unlock(&reorder->lock);
            return ZST_ERROR;
        }
    } else if (reorder->locked_ssrc != ssrc) {
        /* A quiet flow may legitimately restart with a new SSRC.  The generic
         * depayloader has no reset API, so it is retargeted before probation. */
        uint64_t last = atomic_load_explicit(&reorder->route->last_valid_rtp_time_ns,
                                             memory_order_acquire);
        if (!last || now - last <= reorder->ssrc_inactive_ns) {
            pthread_mutex_unlock(&reorder->lock);
            return ZST_OK;
        }
        for (uint32_t i = 0; i < reorder->window; i++) {
            if (reorder->slots[i].buffer) zst_buffer_unref(reorder->slots[i].buffer);
            reorder->slots[i].buffer = NULL;
        }
        reorder->locked_ssrc = ssrc;
        reorder->have_expected = 0;
        if (zst_element_set_property_uint(reorder->route->third, "ssrc", ssrc) != ZST_OK) {
            pthread_mutex_unlock(&reorder->lock);
            return ZST_ERROR;
        }
    }

    if (!reorder->have_expected) {
        reorder->expected = sequence;
        reorder->have_expected = 1;
    }
    int16_t signed_distance = (int16_t)(sequence - reorder->expected);
    if (signed_distance < 0) { pthread_mutex_unlock(&reorder->lock); return ZST_OK; }
    while ((uint16_t)(sequence - reorder->expected) >= reorder->window) {
        reorder_slot_t* expected = &reorder->slots[reorder->expected % reorder->window];
        if (expected->buffer && expected->sequence == reorder->expected)
            (void)reorder_emit(reorder, expected);
        else
            reorder->expected++;
    }

    reorder_slot_t* slot = &reorder->slots[sequence % reorder->window];
    if (slot->buffer) {
        if (slot->sequence == sequence) {
            pthread_mutex_unlock(&reorder->lock);
            return ZST_OK;
        }
        zst_buffer_unref(slot->buffer);
    }
    slot->buffer = zst_buffer_ref(buffer);
    slot->sequence = sequence;
    slot->arrival_ns = now;
    atomic_store_explicit(&reorder->route->last_valid_rtp_time_ns, now, memory_order_release);
    zst_result_t result = reorder_drain(reorder);
    reorder_skip_gap_if_timed_out(reorder, slot->arrival_ns);
    zst_result_t after_timeout = reorder_drain(reorder);
    pthread_mutex_unlock(&reorder->lock);
    return after_timeout != ZST_OK ? after_timeout : result;
}

static void
reorder_watchdog_drain(dante_reorder_t* reorder, uint64_t now)
{
    if (!reorder || !reorder->slots) return;
    pthread_mutex_lock(&reorder->lock);
    reorder_skip_gap_if_timed_out(reorder, now);
    (void)reorder_drain(reorder);
    pthread_mutex_unlock(&reorder->lock);
}

static zst_result_t
reorder_open(zst_element_t* element)
{
    dante_reorder_t* reorder = element->priv;
    reorder->slots = calloc(reorder->window, sizeof(*reorder->slots));
    reorder->have_expected = 0;
    reorder->locked_ssrc = 0;
    return reorder->slots ? ZST_OK : ZST_ERROR;
}

static zst_result_t
reorder_close(zst_element_t* element)
{
    dante_reorder_t* reorder = element->priv;
    if (reorder->slots) {
        for (uint32_t i = 0; i < reorder->window; i++)
            if (reorder->slots[i].buffer) zst_buffer_unref(reorder->slots[i].buffer);
        free(reorder->slots);
        reorder->slots = NULL;
    }
    reorder->have_expected = 0;
    pthread_mutex_destroy(&reorder->lock);
    return ZST_OK;
}

static const zst_element_ops_t reorder_ops = {
    .name = "dantevideoreorder",
    .open = reorder_open,
    .close = reorder_close
};

static zst_element_t*
reorder_create(dante_video_route_t* route, uint32_t window, uint32_t timeout_ms)
{
    dante_reorder_t* reorder = calloc(1, sizeof(*reorder));
    if (!reorder) return NULL;
    reorder->route = route;
    reorder->window = window;
    reorder->timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    reorder->ssrc_inactive_ns = (uint64_t)timeout_ms * 1000000ULL * 50u;
    pthread_mutex_init(&reorder->lock, NULL);
    zst_element_t* element = zst_element_create(&reorder_ops, reorder);
    if (!element) {
        free(reorder);
        return NULL;
    }
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    reorder->src_pad = zst_pad_create("src", ZST_PAD_SRC);
    if (!sink || !reorder->src_pad || zst_element_add_pad(element, sink) != ZST_OK ||
        zst_element_add_pad(element, reorder->src_pad) != ZST_OK) {
        if (sink && !sink->parent) zst_pad_unref(sink);
        if (reorder->src_pad && !reorder->src_pad->parent) zst_pad_unref(reorder->src_pad);
        zst_element_destroy(element);
        return NULL;
    }
    sink->push = reorder_push;
    return element;
}

static zst_result_t
output_push(zst_pad_t* pad, zst_buffer_t* buffer)
{
    dante_output_t* output = pad && pad->parent ? pad->parent->priv : NULL;
    if (!output || !atomic_load_explicit(&output->route->active, memory_order_acquire))
        return ZST_OK;
    return zst_pad_push(output->output_pad, buffer);
}

static const zst_element_ops_t output_ops = {
    .name = "dantevideooutput"
};

static zst_element_t*
output_create(dante_video_route_t* route, zst_pad_t* output_pad)
{
    dante_output_t* output = calloc(1, sizeof(*output));
    if (!output) return NULL;
    output->route = route;
    output->output_pad = output_pad;
    zst_element_t* element = zst_element_create(&output_ops, output);
    if (!element) {
        free(output);
        return NULL;
    }
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    if (!sink || zst_element_add_pad(element, sink) != ZST_OK) {
        if (sink) zst_pad_unref(sink);
        zst_element_destroy(element);
        return NULL;
    }
    sink->push = output_push;
    return element;
}

static void
destroy_unowned_route_elements(dante_video_route_t* route)
{
    if (route->fourth) zst_element_destroy(route->fourth);
    if (route->third) zst_element_destroy(route->third);
    if (route->second) zst_element_destroy(route->second);
    if (route->first) zst_element_destroy(route->first);
}

static zst_result_t
configure_tx_route(dante_video_route_t* route)
{
    route->first = zst_rtp_payloader_create();
    route->second = zst_dante_udp_sink_create();
    if (!route->first || !route->second) return ZST_ERROR;
    const char* destination = route->transport == ZST_DANTE_FLOW_MULTICAST
        ? route->multicast_address : route->receiver_address;
    if (zst_element_set_property_string(route->first, "codec", "h264") != ZST_OK ||
        zst_element_set_property_uint(route->first, "payload-type", 96) != ZST_OK ||
        zst_element_set_property_string(route->second, "destination-address", destination) != ZST_OK ||
        zst_element_set_property_string(route->second, "transmitter-address",
                                        route->transmitter_address) != ZST_OK ||
        zst_element_set_property_uint(route->second, "port", route->port) != ZST_OK)
        return ZST_ERROR;
    route->tx_pay_sink = zst_element_get_pad(route->first, "sink");
    return route->tx_pay_sink ? ZST_OK : ZST_ERROR;
}

static zst_result_t
configure_rx_route(dante_video_coordinator_t* coordinator,
                   dante_video_route_t* route, zst_pad_t* output_pad)
{
    route->first = zst_dante_udp_source_create();
    route->second = reorder_create(route, coordinator->reorder_window,
                                   coordinator->reorder_timeout_ms);
    route->third = zst_rtp_depayloader_create();
    route->fourth = output_create(route, output_pad);
    if (!route->first || !route->second || !route->third || !route->fourth) return ZST_ERROR;
    const char* local = route->transport == ZST_DANTE_FLOW_UNICAST
        ? route->receiver_address : "0.0.0.0";
    const char* group = route->transport == ZST_DANTE_FLOW_MULTICAST
        ? route->multicast_address : "";
    if (zst_element_set_property_string(route->first, "local-address", local) != ZST_OK ||
        zst_element_set_property_string(route->first, "multicast-address", group) != ZST_OK ||
        zst_element_set_property_string(route->first, "multicast-interface-address",
                                        coordinator->multicast_interface_address) != ZST_OK ||
        zst_element_set_property_string(route->first, "transmitter-address",
                                        route->transmitter_address) != ZST_OK ||
        zst_element_set_property_uint(route->first, "port", route->port) != ZST_OK ||
        zst_element_set_property_string(route->third, "codec", "h264") != ZST_OK ||
        zst_element_set_property_uint(route->third, "payload-type", 96) != ZST_OK)
        return ZST_ERROR;
    return zst_element_get_pad(route->third, "sink") ? ZST_OK : ZST_ERROR;
}

static zst_result_t
add_and_link_route(zst_pipeline_t* pipeline, dante_video_route_t* route)
{
    zst_element_t* elements[4] = {route->first, route->second, route->third, route->fourth};
    uint32_t count = route->direction == ZST_DANTE_FLOW_TX ? 2u : 4u;
    uint32_t added = 0;
    zst_result_t result = zst_pipeline_reconfigure_begin(pipeline);
    if (result != ZST_OK) return result;
    for (; added < count; added++) {
        result = zst_pipeline_add_element_dynamic(pipeline, elements[added]);
        if (result != ZST_OK) break;
    }
    if (result == ZST_OK) {
        for (uint32_t i = 0; i + 1 < count; i++) {
            zst_pad_t* source = zst_element_get_pad(elements[i], "src");
            zst_pad_t* sink = zst_element_get_pad(elements[i + 1], "sink");
            result = zst_pipeline_link_pads_dynamic(pipeline, source, sink);
            if (result != ZST_OK) break;
        }
    }
    if (result != ZST_OK) {
        while (added > 0) {
            added--;
            (void)zst_pipeline_remove_element_dynamic(pipeline, elements[added]);
        }
    }
    (void)zst_pipeline_reconfigure_end(pipeline);
    return result;
}

static void
remove_route_elements(zst_pipeline_t* pipeline, dante_video_route_t* route)
{
    zst_element_t* elements[4] = {route->first, route->second, route->third, route->fourth};
    uint32_t count = route->direction == ZST_DANTE_FLOW_TX ? 2u : 4u;
    if (zst_pipeline_reconfigure_begin(pipeline) != ZST_OK) return;
    for (uint32_t i = 0; i < count; i++)
        (void)zst_pipeline_remove_element_dynamic(pipeline, elements[i]);
    (void)zst_pipeline_reconfigure_end(pipeline);
    for (uint32_t i = 0; i < count; i++) zst_element_destroy(elements[i]);
}

static void
report_initial_status(dante_video_coordinator_t* coordinator)
{
    if (!coordinator->session) return;
    for (dante_video_route_t* route = coordinator->routes; route; route = route->next) {
        if (route->direction == ZST_DANTE_FLOW_RX) {
            (void)zst_dante_session_report_rx_flow_status(
                coordinator->session, route->flow_index,
                route->rx_receiving ? ZST_DANTE_RX_STATUS_OK
                                    : ZST_DANTE_RX_STATUS_NOT_RECEIVING_PACKETS);
        }
    }
    for (dante_video_channel_t* channel = coordinator->channels; channel;
         channel = channel->next) {
        if (channel->direction != ZST_DANTE_FLOW_TX) continue;
        int linked = zst_pad_is_linked(channel->pad);
        (void)zst_dante_session_report_tx_channel_status(
            coordinator->session, channel->index,
            linked ? ZST_DANTE_TX_STATUS_OK : ZST_DANTE_TX_STATUS_EXT_NOT_CONNECTED);
        channel->tx_status_initialized = 1;
        channel->tx_was_linked = linked;
    }
}

static void*
watchdog_main(void* argument)
{
    dante_video_coordinator_t* coordinator = argument;
    pthread_mutex_lock(&coordinator->lock);
    while (!coordinator->watchdog_stop) {
        uint64_t now = monotonic_time_ns();
        uint64_t timeout = (uint64_t)coordinator->health_timeout_ms * 1000000ULL;
        if (coordinator->session) {
            for (dante_video_route_t* route = coordinator->routes; route; route = route->next) {
                if (route->direction != ZST_DANTE_FLOW_RX) continue;
                dante_reorder_t* reorder = route->second ? route->second->priv : NULL;
                reorder_watchdog_drain(reorder, now);
                uint64_t last = atomic_load_explicit(&route->last_valid_rtp_time_ns,
                                                     memory_order_acquire);
                int receiving = last != 0 && now >= last && now - last <= timeout;
                if (!route->rx_status_initialized || receiving != route->rx_receiving) {
                    route->rx_receiving = receiving;
                    if (zst_dante_session_report_rx_flow_status(
                        coordinator->session, route->flow_index,
                        receiving ? ZST_DANTE_RX_STATUS_OK
                                  : ZST_DANTE_RX_STATUS_NOT_RECEIVING_PACKETS) == ZST_OK)
                        route->rx_status_initialized = 1;
                }
            }
            for (dante_video_channel_t* channel = coordinator->channels; channel;
                 channel = channel->next) {
                if (channel->direction != ZST_DANTE_FLOW_TX) continue;
                int linked = zst_pad_is_linked(channel->pad);
                if (!channel->tx_status_initialized || linked != channel->tx_was_linked) {
                    if (zst_dante_session_report_tx_channel_status(
                        coordinator->session, channel->index,
                        linked ? ZST_DANTE_TX_STATUS_OK
                               : ZST_DANTE_TX_STATUS_EXT_NOT_CONNECTED) == ZST_OK) {
                        channel->tx_status_initialized = 1;
                        channel->tx_was_linked = linked;
                    }
                }
            }
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 50000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&coordinator->watchdog_cond,
                                     &coordinator->lock, &deadline);
    }
    pthread_mutex_unlock(&coordinator->lock);
    return NULL;
}

static zst_result_t
coordinator_start(zst_element_t* element)
{
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    if (!coordinator->watchdog_started) {
        coordinator->watchdog_stop = 0;
        if (pthread_create(&coordinator->watchdog_thread, NULL, watchdog_main,
                           coordinator) != 0) {
            pthread_mutex_unlock(&coordinator->lock);
            return ZST_ERROR;
        }
        coordinator->watchdog_started = 1;
    }
    pthread_mutex_unlock(&coordinator->lock);
    return ZST_OK;
}

static zst_result_t
coordinator_stop(zst_element_t* element)
{
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    int join = coordinator->watchdog_started;
    coordinator->watchdog_stop = 1;
    pthread_cond_broadcast(&coordinator->watchdog_cond);
    pthread_mutex_unlock(&coordinator->lock);
    if (join) pthread_join(coordinator->watchdog_thread, NULL);
    pthread_mutex_lock(&coordinator->lock);
    coordinator->watchdog_started = 0;
    pthread_mutex_unlock(&coordinator->lock);
    return ZST_OK;
}

static zst_result_t
coordinator_close(zst_element_t* element)
{
    dante_video_coordinator_t* coordinator = element->priv;
    (void)coordinator_stop(element);
    /* Route elements retain route pointers.  Tear them down before channel-pad
     * finalizers can release their contexts during element destruction. */
    pthread_mutex_lock(&coordinator->lock);
    dante_video_route_t* routes = coordinator->routes;
    coordinator->routes = NULL;
    coordinator->flow_count = 0;
    pthread_mutex_unlock(&coordinator->lock);
    while (routes) {
        dante_video_route_t* next = routes->next;
        atomic_store_explicit(&routes->active, 0, memory_order_release);
        if (element->pipeline) remove_route_elements(element->pipeline, routes);
        else destroy_unowned_route_elements(routes);
        free(routes);
        routes = next;
    }
    return ZST_OK;
}

static zst_result_t
coordinator_set_property(zst_element_t* element, const char* name, const char* value)
{
    dante_video_coordinator_t* coordinator = element->priv;
    uint64_t number;
    if (!name || !value) return ZST_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&coordinator->lock);
    zst_result_t result = ZST_OK;
    if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_HEALTH_TIMEOUT_MS) == 0) {
        if (!parse_uint(value, 1, 60000, &number)) result = ZST_ERROR_INVALID_ARGUMENT;
        else coordinator->health_timeout_ms = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_WINDOW) == 0) {
        if (!parse_uint(value, 1, ZST_DANTE_VIDEO_COORDINATOR_MAX_REORDER_WINDOW, &number) ||
            coordinator->flow_count != 0) result = ZST_ERROR_INVALID_ARGUMENT;
        else coordinator->reorder_window = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_TIMEOUT_MS) == 0) {
        if (!parse_uint(value, 1, 10000, &number) || coordinator->flow_count != 0)
            result = ZST_ERROR_INVALID_ARGUMENT;
        else coordinator->reorder_timeout_ms = (uint32_t)number;
    } else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_MULTICAST_INTERFACE_ADDRESS) == 0) {
        if (!valid_ipv4(value, 0, 1) || coordinator->flow_count != 0)
            result = ZST_ERROR_INVALID_ARGUMENT;
        else snprintf(coordinator->multicast_interface_address,
                      sizeof(coordinator->multicast_interface_address), "%s", value);
    } else {
        result = ZST_ERROR_INVALID_ARGUMENT;
    }
    pthread_cond_broadcast(&coordinator->watchdog_cond);
    pthread_mutex_unlock(&coordinator->lock);
    return result;
}

static zst_result_t
coordinator_get_property(zst_element_t* element, const char* name,
                         char* output, size_t output_size)
{
    if (!name || !output || output_size == 0) return ZST_ERROR_INVALID_ARGUMENT;
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    zst_result_t result = ZST_OK;
    if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_HEALTH_TIMEOUT_MS) == 0)
        snprintf(output, output_size, "%u", coordinator->health_timeout_ms);
    else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_WINDOW) == 0)
        snprintf(output, output_size, "%u", coordinator->reorder_window);
    else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_TIMEOUT_MS) == 0)
        snprintf(output, output_size, "%u", coordinator->reorder_timeout_ms);
    else if (strcmp(name, ZST_DANTE_VIDEO_COORDINATOR_PROP_MULTICAST_INTERFACE_ADDRESS) == 0)
        snprintf(output, output_size, "%s", coordinator->multicast_interface_address);
    else
        result = ZST_ERROR_INVALID_ARGUMENT;
    pthread_mutex_unlock(&coordinator->lock);
    return result;
}

static const zst_element_ops_t coordinator_ops = {
    .name = ZST_DANTE_VIDEO_COORDINATOR_FACTORY,
    .close = coordinator_close,
    .start = coordinator_start,
    .stop = coordinator_stop,
    .set_property = coordinator_set_property,
    .get_property = coordinator_get_property
};

zst_element_t*
zst_dante_video_coordinator_create(void)
{
    dante_video_coordinator_t* coordinator = calloc(1, sizeof(*coordinator));
    if (!coordinator) return NULL;
    pthread_mutex_init(&coordinator->lock, NULL);
    pthread_cond_init(&coordinator->watchdog_cond, NULL);
    coordinator->health_timeout_ms = ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_HEALTH_TIMEOUT_MS;
    coordinator->reorder_window = ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_REORDER_WINDOW;
    coordinator->reorder_timeout_ms = ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_REORDER_TIMEOUT_MS;
    strcpy(coordinator->multicast_interface_address, "0.0.0.0");
    zst_element_t* element = zst_element_create(&coordinator_ops, coordinator);
    if (!element) {
        pthread_cond_destroy(&coordinator->watchdog_cond);
        pthread_mutex_destroy(&coordinator->lock);
        free(coordinator);
        return NULL;
    }
    coordinator->element = element;
    return element;
}

zst_result_t
zst_dante_video_coordinator_attach_session(zst_element_t* element,
                                            zst_element_t* session)
{
    if (!element || element->ops != &coordinator_ops) return ZST_ERROR_INVALID_ARGUMENT;
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    coordinator->session = session;
    for (dante_video_channel_t* channel = coordinator->channels; channel;
         channel = channel->next)
        channel->tx_status_initialized = 0;
    for (dante_video_route_t* route = coordinator->routes; route; route = route->next)
        route->rx_status_initialized = 0;
    pthread_cond_broadcast(&coordinator->watchdog_cond);
    pthread_mutex_unlock(&coordinator->lock);
    return ZST_OK;
}

static zst_pad_t*
request_channel_pad(zst_element_t* element, zst_dante_flow_direction_t direction,
                    uint32_t channel_index)
{
    if (!element || element->ops != &coordinator_ops) return NULL;
    dante_video_coordinator_t* coordinator = element->priv;
    dante_video_channel_t* channel = calloc(1, sizeof(*channel));
    channel_pad_context_t* context = calloc(1, sizeof(*context));
    if (!channel || !context) {
        free(channel);
        free(context);
        return NULL;
    }
    char name[48];
    snprintf(name, sizeof(name), direction == ZST_DANTE_FLOW_TX
             ? "tx_sink_%u" : "rx_src_%u", channel_index);
    zst_pad_t* pad = zst_pad_create(name, direction == ZST_DANTE_FLOW_TX
                                   ? ZST_PAD_SINK : ZST_PAD_SRC);
    if (!pad) {
        free(channel);
        free(context);
        return NULL;
    }
    channel->index = channel_index;
    channel->direction = direction;
    channel->pad = pad;
    context->coordinator = coordinator;
    context->channel = channel;
    pad->priv = context;
    pad->destroy_priv = channel_pad_destroy;
    if (direction == ZST_DANTE_FLOW_TX) pad->push = tx_channel_push;
    else (void)zst_pad_set_unlinked_policy(pad, ZST_PAD_UNLINKED_DROP, 0);

    pthread_mutex_lock(&coordinator->lock);
    if (find_channel(coordinator, direction, channel_index) ||
        zst_element_add_pad(element, pad) != ZST_OK) {
        pthread_mutex_unlock(&coordinator->lock);
        pad->priv = NULL;
        zst_pad_unref(pad);
        free(context);
        free(channel);
        return NULL;
    }
    channel->next = coordinator->channels;
    coordinator->channels = channel;
    pthread_mutex_unlock(&coordinator->lock);
    return pad;
}

zst_pad_t*
zst_dante_video_coordinator_request_tx_input_pad(zst_element_t* element,
                                                  uint32_t channel_index)
{
    return request_channel_pad(element, ZST_DANTE_FLOW_TX, channel_index);
}

zst_pad_t*
zst_dante_video_coordinator_request_rx_output_pad(zst_element_t* element,
                                                   uint32_t channel_index)
{
    return request_channel_pad(element, ZST_DANTE_FLOW_RX, channel_index);
}

static zst_result_t
release_channel_pad(zst_element_t* element, zst_pad_t* pad,
                    zst_dante_flow_direction_t direction)
{
    if (!element || element->ops != &coordinator_ops || !pad) return ZST_ERROR_INVALID_ARGUMENT;
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    dante_video_channel_t** link = &coordinator->channels;
    while (*link && ((*link)->pad != pad || (*link)->direction != direction))
        link = &(*link)->next;
    if (!*link || channel_has_flow(coordinator, direction, (*link)->index)) {
        pthread_mutex_unlock(&coordinator->lock);
        return ZST_ERROR;
    }
    dante_video_channel_t* channel = *link;
    *link = channel->next;
    channel_pad_context_t* context = pad->priv;
    pad->priv = NULL;
    pad->destroy_priv = NULL;
    pthread_mutex_unlock(&coordinator->lock);
    zst_result_t result = zst_element_remove_pad(element, pad);
    free(channel->sps);
    free(channel->pps);
    free(context);
    free(channel);
    return result;
}

zst_result_t
zst_dante_video_coordinator_release_tx_input_pad(zst_element_t* element, zst_pad_t* pad)
{
    return release_channel_pad(element, pad, ZST_DANTE_FLOW_TX);
}

zst_result_t
zst_dante_video_coordinator_release_rx_output_pad(zst_element_t* element, zst_pad_t* pad)
{
    return release_channel_pad(element, pad, ZST_DANTE_FLOW_RX);
}

zst_result_t
zst_dante_video_coordinator_apply_flow(zst_element_t* element,
                                        const zst_dante_flow_t* flow)
{
    if (!element || element->ops != &coordinator_ops || !valid_flow_shape(flow) ||
        !element->pipeline || element->pipeline->state < ZST_STATE_READY)
        return ZST_ERROR_INVALID_ARGUMENT;
    dante_video_coordinator_t* coordinator = element->priv;
    dante_video_route_t* route = calloc(1, sizeof(*route));
    if (!route) return ZST_ERROR;
    copy_flow(route, flow);

    pthread_mutex_lock(&coordinator->lock);
    dante_video_channel_t* channel = find_channel(coordinator, flow->direction,
                                                  flow->channel_index);
    if (!channel || find_route(coordinator, flow->direction, flow->flow_index) ||
        (flow->direction == ZST_DANTE_FLOW_RX &&
         channel_has_flow(coordinator, ZST_DANTE_FLOW_RX, flow->channel_index))) {
        pthread_mutex_unlock(&coordinator->lock);
        free(route);
        return ZST_ERROR_INVALID_ARGUMENT;
    }
    zst_result_t result = flow->direction == ZST_DANTE_FLOW_TX
        ? configure_tx_route(route)
        : configure_rx_route(coordinator, route, channel->pad);
    if (result != ZST_OK) {
        pthread_mutex_unlock(&coordinator->lock);
        destroy_unowned_route_elements(route);
        free(route);
        return result;
    }
    result = add_and_link_route(element->pipeline, route);
    if (result != ZST_OK) {
        pthread_mutex_unlock(&coordinator->lock);
        destroy_unowned_route_elements(route);
        free(route);
        return result;
    }
    if (flow->direction == ZST_DANTE_FLOW_TX) {
        uint32_t ssrc = random_u32();
        uint16_t sequence = (uint16_t)random_u32();
        if (zst_element_set_property_uint(route->first, "ssrc", ssrc) != ZST_OK ||
            zst_element_set_property_uint(route->first, "seq", sequence) != ZST_OK) {
            pthread_mutex_unlock(&coordinator->lock);
            remove_route_elements(element->pipeline, route);
            free(route);
            return ZST_ERROR;
        }
    }
    route->next = coordinator->routes;
    coordinator->routes = route;
    coordinator->flow_count++;
    atomic_store_explicit(&route->active, 1, memory_order_release);
    route->parameter_sets_pending = flow->direction == ZST_DANTE_FLOW_TX &&
                                    channel->sps && channel->pps;
    zst_pad_t* event_pad = flow->direction == ZST_DANTE_FLOW_TX
        ? zst_pad_ref(channel->pad) : NULL;
    if (flow->direction == ZST_DANTE_FLOW_RX && coordinator->session)
        (void)zst_dante_session_report_rx_flow_status(
            coordinator->session, flow->flow_index,
            ZST_DANTE_RX_STATUS_NOT_RECEIVING_PACKETS);
    pthread_cond_broadcast(&coordinator->watchdog_cond);
    pthread_mutex_unlock(&coordinator->lock);
    if (flow->direction == ZST_DANTE_FLOW_TX) {
        zst_pad_event_t* event = zst_pad_event_new_force_keyframe();
        if (event) {
            (void)zst_pad_push_event_upstream(event_pad, event);
            zst_pad_event_unref(event);
        }
        zst_pad_unref(event_pad);
    }
    return ZST_OK;
}

zst_result_t
zst_dante_video_coordinator_remove_flow(zst_element_t* element,
                                         const zst_dante_flow_t* flow)
{
    if (!element || element->ops != &coordinator_ops || !flow || !element->pipeline ||
        (flow->direction != ZST_DANTE_FLOW_TX && flow->direction != ZST_DANTE_FLOW_RX))
        return ZST_ERROR_INVALID_ARGUMENT;
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    dante_video_route_t** link = &coordinator->routes;
    while (*link && ((*link)->direction != flow->direction ||
                     (*link)->flow_index != flow->flow_index))
        link = &(*link)->next;
    if (!*link || (*link)->channel_index != flow->channel_index) {
        pthread_mutex_unlock(&coordinator->lock);
        return ZST_ERROR;
    }
    dante_video_route_t* route = *link;
    atomic_store_explicit(&route->active, 0, memory_order_release);
    *link = route->next;
    coordinator->flow_count--;
    pthread_mutex_unlock(&coordinator->lock);
    remove_route_elements(element->pipeline, route);
    free(route);
    return ZST_OK;
}

uint32_t
zst_dante_video_coordinator_get_flow_count(zst_element_t* element)
{
    if (!element || element->ops != &coordinator_ops) return 0;
    dante_video_coordinator_t* coordinator = element->priv;
    pthread_mutex_lock(&coordinator->lock);
    uint32_t count = coordinator->flow_count;
    pthread_mutex_unlock(&coordinator->lock);
    return count;
}
