/*=============================================================================
    test_dante_udp.c - Dante IPv4 UDP media transport tests
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zstreamer/elements/zst_dante_udp_sink.h"
#include "zstreamer/elements/zst_dante_udp_source.h"

static uint16_t
unused_port(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t size = sizeof(address);
    assert(fd >= 0);
    assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr*)&address, &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static int
sender_socket(const char* address)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = 0 };
    assert(fd >= 0);
    assert(inet_pton(AF_INET, address, &local.sin_addr) == 1);
    assert(bind(fd, (struct sockaddr*)&local, sizeof(local)) == 0);
    return fd;
}

static void
send_payload(int fd, const char* address, uint16_t port, const void* data, size_t size)
{
    struct sockaddr_in destination = { .sin_family = AF_INET, .sin_port = htons(port) };
    assert(inet_pton(AF_INET, address, &destination.sin_addr) == 1);
    assert(sendto(fd, data, size, 0, (struct sockaddr*)&destination, sizeof(destination)) == (ssize_t)size);
}

static uint64_t
uint_property(zst_element_t* element, const char* name)
{
    char value[64];
    char* end;
    assert(zst_element_get_property(element, name, value, sizeof(value)) == ZST_OK);
    errno = 0;
    uint64_t result = strtoull(value, &end, 10);
    assert(errno == 0 && *end == '\0');
    return result;
}

static zst_buffer_t*
receive_bounded(zst_element_t* source, unsigned attempts)
{
    for (unsigned i = 0; i < attempts; ++i) {
        zst_buffer_t* output = NULL;
        zst_result_t result = source->ops->process(source, NULL, &output);
        if (result == ZST_OK) return output;
        assert(result == ZST_TIMEOUT);
    }
    return NULL;
}

static void
test_properties_and_required_configuration(void)
{
    zst_element_t* source = zst_dante_udp_source_create();
    zst_element_t* sink = zst_dante_udp_sink_create();
    char value[64];
    assert(source && sink);
    assert(strcmp(source->ops->name, "danteudpsrc") == 0);
    assert(strcmp(sink->ops->name, "danteudpsink") == 0);
    assert(zst_element_set_state(source, ZST_STATE_READY) == ZST_ERROR);
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_ERROR);
    assert(zst_element_set_property(source, "port", "12x") == ZST_ERROR);
    assert(zst_element_set_property(source, "port", "0") == ZST_ERROR);
    assert(zst_element_set_property(source, "transmitter-address", "239.1.1.1") == ZST_ERROR);
    assert(zst_element_set_property(source, "multicast-address", "127.0.0.1") == ZST_ERROR);
    assert(zst_element_set_property(source, "max-datagram-size", "65536") == ZST_ERROR);
    assert(zst_element_set_property(sink, "destination-address", "not-an-ip") == ZST_ERROR);
    assert(zst_element_set_property(sink, "transmitter-address", "0.0.0.0") == ZST_ERROR);
    assert(zst_element_set_property(sink, "ttl", "256") == ZST_ERROR);
    assert(zst_element_set_property(sink, "loop", "yes") == ZST_ERROR);
    assert(zst_element_set_property(sink, "transmitter-address", "127.0.0.2") == ZST_OK);
    assert(zst_element_get_property(sink, "multicast-interface-address", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "127.0.0.2") == 0);
    zst_element_destroy(source);
    zst_element_destroy(sink);
}

static void
test_source_filter_and_truncation(void)
{
    uint16_t port = unused_port();
    char port_text[16];
    zst_element_t* source = zst_dante_udp_source_create();
    int wrong = sender_socket("127.0.0.1");
    int accepted = sender_socket("127.0.0.2");
    const char good[] = "one-datagram-one-buffer";
    char oversized[64] = {0};
    snprintf(port_text, sizeof(port_text), "%u", port);
    assert(source);
    assert(zst_element_set_property(source, "local-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(source, "transmitter-address", "127.0.0.2") == ZST_OK);
    assert(zst_element_set_property(source, "port", port_text) == ZST_OK);
    assert(zst_element_set_property(source, "read-timeout-ms", "20") == ZST_OK);
    assert(zst_element_set_property(source, "max-datagram-size", "32") == ZST_OK);
    assert(zst_element_set_state(source, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(source, ZST_STATE_PLAYING) == ZST_OK);

    send_payload(wrong, "127.0.0.1", port, "wrong", 5);
    assert(receive_bounded(source, 1) == NULL);
    assert(uint_property(source, "packets-rejected") == 1);

    send_payload(accepted, "127.0.0.1", port, oversized, sizeof(oversized));
    assert(receive_bounded(source, 1) == NULL);
    assert(uint_property(source, "packets-truncated") == 1);
    assert(uint_property(source, "last-packet-size") == sizeof(oversized));

    send_payload(accepted, "127.0.0.1", port, good, sizeof(good));
    zst_buffer_t* output = receive_bounded(source, 3);
    assert(output && output->memory.size == sizeof(good));
    assert(memcmp(output->memory.data, good, sizeof(good)) == 0);
    assert(uint_property(source, "packets-received") == 1);
    assert(uint_property(source, "bytes-received") == sizeof(good));
    zst_buffer_unref(output);
    assert(zst_element_set_state(source, ZST_STATE_NULL) == ZST_OK);
    close(wrong);
    close(accepted);
    zst_element_destroy(source);
}

static void
test_long_read_timeout_is_sliced(void)
{
    zst_element_t* source = zst_dante_udp_source_create();
    struct timespec before, after;
    zst_buffer_t* output = NULL;
    assert(source);
    assert(zst_element_set_property(source, "transmitter-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(source, "read-timeout-ms", "60000") == ZST_OK);
    assert(zst_element_set_state(source, ZST_STATE_READY) == ZST_OK);
    clock_gettime(CLOCK_MONOTONIC, &before);
    assert(source->ops->process(source, NULL, &output) == ZST_TIMEOUT && !output);
    clock_gettime(CLOCK_MONOTONIC, &after);
    assert((after.tv_sec - before.tv_sec) * 1000L +
           (after.tv_nsec - before.tv_nsec) / 1000000L < 200);
    assert(zst_element_set_state(source, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(source);
}

static void
test_sink_transmitter_bind_and_stats(void)
{
    uint16_t port = unused_port();
    char port_text[16];
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(port) };
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 250000 };
    struct sockaddr_in sender = {0};
    socklen_t sender_size = sizeof(sender);
    zst_element_t* sink = zst_dante_udp_sink_create();
    zst_buffer_t* input = zst_buffer_create(ZST_BUFFER_USER);
    const char payload[] = "bound-transmitter";
    char received[64];
    char sender_text[INET_ADDRSTRLEN];
    assert(receiver >= 0 && sink && input);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (struct sockaddr*)&local, sizeof(local)) == 0);
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    snprintf(port_text, sizeof(port_text), "%u", port);
    assert(zst_element_set_property(sink, "destination-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(sink, "transmitter-address", "127.0.0.2") == ZST_OK);
    assert(zst_element_set_property(sink, "port", port_text) == ZST_OK);
    assert(zst_element_set_property(sink, "timestamp-pacing", "true") == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_PLAYING) == ZST_OK);
    input->memory.data = (void*)payload;
    input->memory.size = sizeof(payload);
    input->pts = 1000000;
    assert(sink->ops->process(sink, input, NULL) == ZST_OK);
    assert(recvfrom(receiver, received, sizeof(received), 0,
                    (struct sockaddr*)&sender, &sender_size) == (ssize_t)sizeof(payload));
    assert(inet_ntop(AF_INET, &sender.sin_addr, sender_text, sizeof(sender_text)) != NULL);
    assert(strcmp(sender_text, "127.0.0.2") == 0);
    assert(memcmp(received, payload, sizeof(payload)) == 0);
    assert(uint_property(sink, "packets-sent") == 1);
    assert(uint_property(sink, "bytes-sent") == sizeof(payload));
    assert(uint_property(sink, "send-errors") == 0);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    zst_buffer_unref(input);
    zst_element_destroy(sink);
    close(receiver);
}

static void
test_multicast_if_available(void)
{
    uint16_t port = unused_port();
    char port_text[16];
    const char payload[] = "multicast-loopback";
    zst_element_t* source = zst_dante_udp_source_create();
    zst_element_t* sink = zst_dante_udp_sink_create();
    zst_buffer_t* input = zst_buffer_create(ZST_BUFFER_USER);
    assert(source && sink && input);
    snprintf(port_text, sizeof(port_text), "%u", port);
    assert(zst_element_set_property(source, "multicast-address", "239.255.42.42") == ZST_OK);
    assert(zst_element_set_property(source, "multicast-interface-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(source, "transmitter-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(source, "port", port_text) == ZST_OK);
    assert(zst_element_set_property(source, "read-timeout-ms", "20") == ZST_OK);
    assert(zst_element_set_property(sink, "destination-address", "239.255.42.42") == ZST_OK);
    assert(zst_element_set_property(sink, "transmitter-address", "127.0.0.1") == ZST_OK);
    assert(zst_element_set_property(sink, "port", port_text) == ZST_OK);
    assert(zst_element_set_property(sink, "loop", "true") == ZST_OK);
    if (zst_element_set_state(source, ZST_STATE_READY) != ZST_OK ||
        zst_element_set_state(sink, ZST_STATE_READY) != ZST_OK) {
        printf("Dante UDP multicast test skipped: loopback multicast unavailable\n");
        zst_element_set_state(source, ZST_STATE_NULL);
        zst_element_set_state(sink, ZST_STATE_NULL);
        zst_buffer_unref(input);
        zst_element_destroy(source);
        zst_element_destroy(sink);
        return;
    }
    input->memory.data = (void*)payload;
    input->memory.size = sizeof(payload);
    assert(sink->ops->process(sink, input, NULL) == ZST_OK);
    zst_buffer_t* output = receive_bounded(source, 10);
    if (output) {
        assert(output->memory.size == sizeof(payload));
        assert(memcmp(output->memory.data, payload, sizeof(payload)) == 0);
        zst_buffer_unref(output);
    } else {
        printf("Dante UDP multicast receive skipped: namespace suppressed loopback\n");
    }
    assert(zst_element_set_state(source, ZST_STATE_NULL) == ZST_OK);
    assert(zst_element_set_state(sink, ZST_STATE_NULL) == ZST_OK);
    zst_buffer_unref(input);
    zst_element_destroy(source);
    zst_element_destroy(sink);
}

int
main(void)
{
    test_properties_and_required_configuration();
    test_source_filter_and_truncation();
    test_long_read_timeout_is_sliced();
    test_sink_transmitter_bind_and_stats();
    test_multicast_if_available();
    printf("Dante UDP tests passed\n");
    return 0;
}
