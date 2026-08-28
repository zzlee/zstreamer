/*=============================================================================
    test_dante_video_coordinator.c - Public Dante video coordinator tests
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zstreamer/elements/zst_dante_video_coordinator.h"
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"

#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    _Atomic uint32_t count;
    uint8_t nal_types[8];
} capture_t;

static zst_result_t
capture_push(zst_pad_t* pad, zst_buffer_t* buffer)
{
    capture_t* capture = pad->parent->priv;
    uint32_t slot = atomic_load_explicit(&capture->count, memory_order_relaxed);
    if (slot < sizeof(capture->nal_types) && buffer->memory.size >= 5) {
        const uint8_t* bytes = buffer->memory.data;
        capture->nal_types[slot] = bytes[4] & 0x1f;
    }
    atomic_fetch_add_explicit(&capture->count, 1, memory_order_release);
    return ZST_OK;
}

static const zst_element_ops_t capture_ops = { .name = "dantecapture" };
static const zst_element_ops_t producer_ops = { .name = "danteproducer" };

static zst_element_t*
capture_create(capture_t** capture_out)
{
    capture_t* capture = calloc(1, sizeof(*capture));
    zst_element_t* element = capture ? zst_element_create(&capture_ops, capture) : NULL;
    zst_pad_t* pad = element ? zst_pad_create("sink", ZST_PAD_SINK) : NULL;
    if (!element || !pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        if (element) zst_element_destroy(element);
        else free(capture);
        return NULL;
    }
    pad->push = capture_push;
    *capture_out = capture;
    return element;
}

static zst_element_t*
producer_create(void)
{
    zst_element_t* element = zst_element_create(&producer_ops, calloc(1, 1));
    zst_pad_t* pad = element ? zst_pad_create("src", ZST_PAD_SRC) : NULL;
    if (!element || !pad || zst_element_add_pad(element, pad) != ZST_OK) {
        if (pad) zst_pad_unref(pad);
        if (element) zst_element_destroy(element);
        return NULL;
    }
    return element;
}

static uint16_t
unused_port(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    socklen_t size = sizeof(address);
    assert(fd >= 0);
    assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr*)&address, &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static int
bound_socket(const char* address, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(port) };
    assert(fd >= 0);
    assert(inet_pton(AF_INET, address, &local.sin_addr) == 1);
    assert(bind(fd, (struct sockaddr*)&local, sizeof(local)) == 0);
    return fd;
}

static void
send_rtp(int fd, uint16_t port, uint16_t sequence, uint32_t ssrc, uint8_t nal_type)
{
    uint8_t packet[14] = {
        0x80, 0xe0, (uint8_t)(sequence >> 8), (uint8_t)sequence,
        0, 0, 0, (uint8_t)sequence,
        (uint8_t)(ssrc >> 24), (uint8_t)(ssrc >> 16),
        (uint8_t)(ssrc >> 8), (uint8_t)ssrc,
        (uint8_t)(0x60 | nal_type), nal_type
    };
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    assert(sendto(fd, packet, sizeof(packet), 0,
                  (struct sockaddr*)&destination, sizeof(destination)) ==
           (ssize_t)sizeof(packet));
}

static int
wait_for_count(capture_t* capture, uint32_t count, unsigned timeout_ms)
{
    struct timespec delay = { .tv_nsec = 1000000L };
    for (unsigned i = 0; i < timeout_ms; i++) {
        if (atomic_load_explicit(&capture->count, memory_order_acquire) >= count) return 1;
        nanosleep(&delay, NULL);
    }
    return 0;
}

static ssize_t
receive_datagram_bounded(int fd, void* buffer, size_t size)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    return poll(&descriptor, 1, 1000) == 1 ? recv(fd, buffer, size, 0) : -1;
}

static zst_dante_flow_t
unicast_flow(zst_dante_flow_direction_t direction, uint32_t flow_index,
             uint32_t channel_index, uint16_t port)
{
    zst_dante_flow_t flow = {
        .direction = direction,
        .transport = ZST_DANTE_FLOW_UNICAST,
        .flow_index = flow_index,
        .channel_index = channel_index,
        .port = port,
        .receiver_address = "127.0.0.1",
        .transmitter_address = "127.0.0.2"
    };
    return flow;
}

static void
test_api_validation(void)
{
    zst_element_t* coordinator = zst_dante_video_coordinator_create();
    assert(coordinator);
    assert(strcmp(coordinator->ops->name, ZST_DANTE_VIDEO_COORDINATOR_FACTORY) == 0);
    assert(zst_dante_video_coordinator_request_tx_input_pad(coordinator, 4));
    assert(!zst_dante_video_coordinator_request_tx_input_pad(coordinator, 4));
    assert(zst_element_set_property_uint(
               coordinator, ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_WINDOW, 0) != ZST_OK);
    zst_dante_flow_t bad = unicast_flow(ZST_DANTE_FLOW_TX, 1, 4, 5004);
    bad.receiver_address = "239.1.1.1";
    assert(zst_dante_video_coordinator_apply_flow(coordinator, &bad) != ZST_OK);
    zst_element_destroy(coordinator);
}

static void
test_dynamic_rx_and_tx(void)
{
    uint16_t rx_port = unused_port();
    uint16_t tx_port_a = unused_port();
    uint16_t tx_port_b = unused_port();
    int sender = bound_socket("127.0.0.2", 0);
    int receiver_a = bound_socket("127.0.0.1", tx_port_a);
    int receiver_b = bound_socket("127.0.0.1", tx_port_b);
    zst_pipeline_t* pipeline = zst_pipeline_create();
    zst_element_t* coordinator = zst_dante_video_coordinator_create();
    zst_element_t* producer = producer_create();
    capture_t* capture_data = NULL;
    zst_element_t* capture = capture_create(&capture_data);
    assert(pipeline && coordinator && producer && capture);

    zst_pad_t* tx_input = zst_dante_video_coordinator_request_tx_input_pad(coordinator, 0);
    zst_pad_t* rx_output = zst_dante_video_coordinator_request_rx_output_pad(coordinator, 0);
    assert(tx_input && rx_output);
    assert(zst_pipeline_add(pipeline, producer) == ZST_OK);
    assert(zst_pipeline_add(pipeline, coordinator) == ZST_OK);
    assert(zst_pipeline_add(pipeline, capture) == ZST_OK);
    assert(zst_pad_link(zst_element_get_pad(producer, "src"), tx_input) == ZST_OK);
    assert(zst_pad_link(rx_output, zst_element_get_pad(capture, "sink")) == ZST_OK);
    assert(zst_pipeline_set_state(pipeline, ZST_STATE_READY) == ZST_OK);

    zst_dante_flow_t rx = unicast_flow(ZST_DANTE_FLOW_RX, 10, 0, rx_port);
    assert(zst_dante_video_coordinator_apply_flow(coordinator, &rx) == ZST_OK);
    assert(zst_dante_video_coordinator_apply_flow(coordinator, &rx) != ZST_OK);

    zst_scheduler_config_t config = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    zst_scheduler_t* scheduler = zst_scheduler_create(&config);
    assert(scheduler);
    assert(zst_scheduler_attach(scheduler, pipeline) == ZST_OK);
    assert(zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(scheduler) == ZST_OK);

    /* Establish 65534, then prove both out-of-order delivery and 16-bit wrap. */
    send_rtp(sender, rx_port, 65534, 0x12345678u, 1);
    send_rtp(sender, rx_port, 0, 0x12345678u, 3);
    send_rtp(sender, rx_port, 65535, 0x12345678u, 2);
    assert(wait_for_count(capture_data, 3, 1000));
    assert(capture_data->nal_types[0] == 1);
    assert(capture_data->nal_types[1] == 2);
    assert(capture_data->nal_types[2] == 3);
    assert(zst_dante_video_coordinator_remove_flow(coordinator, &rx) == ZST_OK);

    static const uint8_t parameter_sets[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
        0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2
    };
    zst_buffer_t* headers = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    assert(headers);
    headers->memory.data = (void*)parameter_sets;
    headers->memory.size = sizeof(parameter_sets);
    assert(zst_pad_push(zst_element_get_pad(producer, "src"), headers) == ZST_OK);
    zst_buffer_unref(headers);

    zst_dante_flow_t tx_a = unicast_flow(ZST_DANTE_FLOW_TX, 20, 0, tx_port_a);
    zst_dante_flow_t tx_b = unicast_flow(ZST_DANTE_FLOW_TX, 21, 0, tx_port_b);
    assert(zst_dante_video_coordinator_apply_flow(coordinator, &tx_a) == ZST_OK);
    assert(zst_dante_video_coordinator_apply_flow(coordinator, &tx_b) == ZST_OK);
    assert(zst_dante_video_coordinator_get_flow_count(coordinator) == 2);

    static const uint8_t annex_b[] = {0, 0, 0, 1, 0x65, 0xaa, 0xbb};
    zst_buffer_t* access_unit = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    assert(access_unit);
    access_unit->memory.data = (void*)annex_b;
    access_unit->memory.size = sizeof(annex_b);
    access_unit->pts = 1000000000ULL;
    assert(zst_pad_push(zst_element_get_pad(producer, "src"), access_unit) == ZST_OK);
    zst_buffer_unref(access_unit);

    uint8_t datagram[1500];
    assert(receive_datagram_bounded(receiver_a, datagram, sizeof(datagram)) > 12);
    assert((datagram[1] & 0x7f) == 96);
    assert(receive_datagram_bounded(receiver_b, datagram, sizeof(datagram)) > 12);

    assert(zst_dante_video_coordinator_release_tx_input_pad(coordinator, tx_input) != ZST_OK);
    assert(zst_dante_video_coordinator_remove_flow(coordinator, &tx_a) == ZST_OK);
    assert(zst_dante_video_coordinator_remove_flow(coordinator, &tx_b) == ZST_OK);
    assert(zst_dante_video_coordinator_get_flow_count(coordinator) == 0);
    assert(zst_scheduler_stop(scheduler) == ZST_OK);
    assert(zst_pipeline_set_state(pipeline, ZST_STATE_NULL) == ZST_OK);
    zst_scheduler_destroy(scheduler);
    assert(zst_dante_video_coordinator_release_tx_input_pad(coordinator, tx_input) == ZST_OK);
    assert(zst_dante_video_coordinator_release_rx_output_pad(coordinator, rx_output) == ZST_OK);
    zst_pipeline_destroy(pipeline);
    close(sender);
    close(receiver_a);
    close(receiver_b);
}

int
main(void)
{
    test_api_validation();
    test_dynamic_rx_and_tx();
    printf("Dante video coordinator tests passed\n");
    return 0;
}
