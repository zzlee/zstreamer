/*=============================================================================
    dante_udp_source.c - Source-filtered Dante IPv4 UDP media receiver
=============================================================================*/
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
    int socket_rx_buffer;
    size_t max_datagram_size;
    struct in_addr group;
    struct in_addr interface_addr;
    struct in_addr transmitter;
    int membership;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t packets_rejected;
    uint64_t packets_truncated;
    uint64_t rtp_packets;
    uint64_t rtp_lost;
    uint64_t rtp_out_of_order;
    uint16_t rtp_expected_seq;
    int rtp_have_seq;
    char last_packet_address[INET_ADDRSTRLEN];
    uint16_t last_packet_port;
    uint64_t last_packet_size;
    _Atomic uint64_t last_packet_time_ns;
    pthread_t reader_thread;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    zst_buffer_t** ring;
    size_t ring_cap;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    uint64_t queue_overflow;
    _Atomic int reader_running;
    int reader_started;
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
source_update_rtp_stats(dante_udp_source_t* source, const uint8_t* data, size_t size)
{
    if (!source || !data || size < 12 || (data[0] & 0xc0) != 0x80) return;
    uint16_t seq = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
    source->rtp_packets++;
    if (!source->rtp_have_seq) {
        source->rtp_expected_seq = (uint16_t)(seq + 1u);
        source->rtp_have_seq = 1;
        return;
    }
    int16_t delta = (int16_t)(seq - source->rtp_expected_seq);
    if (delta == 0) {
        source->rtp_expected_seq = (uint16_t)(seq + 1u);
    } else if (delta > 0) {
        source->rtp_lost += (uint16_t)delta;
        source->rtp_expected_seq = (uint16_t)(seq + 1u);
    } else {
        source->rtp_out_of_order++;
    }
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

#define DANTE_UDP_SOURCE_RING_CAP 4096u

/* Per-packet storage is served from a module-wide chunk pool instead of
 * malloc/free of max_datagram_size (64 KB) per datagram.  The receive path
 * churns one allocation per packet (about 80 MB/s at 1250 pps); allocator
 * lock contention can then stall the reader thread long enough for the
 * ~212 KB kernel socket buffer to overflow.  Chunks are 2048 B, sized for
 * 1500 B Ethernet datagrams; anything larger is counted as truncated via
 * MSG_TRUNC, mirroring the old behaviour for oversized datagrams.  Keeping
 * chunk lifetimes module-global (instead of element-owned) avoids any
 * use-after-free if a downstream element drops a buffer after this element
 * is torn down. */
#define DANTE_POOL_CHUNK_SIZE 2048u
#define DANTE_POOL_MAX_CHUNKS 8192u

typedef struct dante_udp_chunk {
    struct dante_udp_chunk* next;
    _Alignas(32) uint8_t data[DANTE_POOL_CHUNK_SIZE];
} dante_udp_chunk_t;

static pthread_mutex_t source_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static dante_udp_chunk_t* source_pool_free;
static size_t source_pool_count;

static dante_udp_chunk_t*
source_chunk_acquire(void)
{
    pthread_mutex_lock(&source_pool_lock);
    dante_udp_chunk_t* chunk = source_pool_free;
    if (chunk) {
        source_pool_free = chunk->next;
        pthread_mutex_unlock(&source_pool_lock);
        return chunk;
    }
    if (source_pool_count < DANTE_POOL_MAX_CHUNKS) {
        source_pool_count++;
        pthread_mutex_unlock(&source_pool_lock);
        return malloc(sizeof(dante_udp_chunk_t));
    }
    pthread_mutex_unlock(&source_pool_lock);
    return malloc(sizeof(dante_udp_chunk_t));
}

static void
source_chunk_release(void* priv)
{
    dante_udp_chunk_t* chunk = priv;
    if (!chunk) return;
    pthread_mutex_lock(&source_pool_lock);
    if (source_pool_count <= DANTE_POOL_MAX_CHUNKS) {
        chunk->next = source_pool_free;
        source_pool_free = chunk;
    } else {
        free(chunk);
    }
    pthread_mutex_unlock(&source_pool_lock);
}

/* The kernel receive buffer cannot be raised without net.core.rmem_max and a
 * busy downstream (decode) path can stall the scheduler worker that used to
 * drive the socket.  A dedicated reader thread drains the socket as soon as
 * datagrams arrive and parks them in a bounded ring; process() only dequeues,
 * so reception is decoupled from downstream scheduling pressure. */
static void*
source_reader_main(void* arg)
{
    dante_udp_source_t* source = arg;
    while (atomic_load_explicit(&source->reader_running, memory_order_acquire)) {
        struct pollfd descriptor;
        if (source->fd < 0) break;
        descriptor.fd = source->fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        int ready = poll(&descriptor, 1, 50);
        if (ready < 0) break;
        if (ready == 0) continue;
        if (!(descriptor.revents & POLLIN)) continue;
        if (source->fd < 0) break;

        dante_udp_chunk_t* chunk = source_chunk_acquire();
        if (!chunk) continue;
        zst_buffer_t* buffer = zst_buffer_create(ZST_BUFFER_USER);
        if (!buffer) {
            source_chunk_release(chunk);
            continue;
        }
        buffer->memory.type = ZST_MEMORY_CPU;
        buffer->memory.data = chunk->data;
        buffer->memory.size = 0;
        buffer->memory.priv = chunk;
        buffer->memory.release = source_chunk_release;

        struct sockaddr_in sender = {0};
        socklen_t sender_size = sizeof(sender);
        ssize_t received = recvfrom(source->fd, buffer->memory.data,
                                    DANTE_POOL_CHUNK_SIZE, MSG_TRUNC,
                                    (struct sockaddr*)&sender, &sender_size);
        if (received < 0) {
            zst_buffer_unref(buffer);
            continue;
        }
        if (!inet_ntop(AF_INET, &sender.sin_addr, source->last_packet_address,
                       sizeof(source->last_packet_address))) {
            source->last_packet_address[0] = '\0';
        }
        source->last_packet_port = ntohs(sender.sin_port);
        source->last_packet_size = (uint64_t)received;
        if (sender.sin_family != AF_INET ||
            sender.sin_addr.s_addr != source->transmitter.s_addr) {
            source->packets_rejected++;
            zst_buffer_unref(buffer);
            continue;
        }
        if ((size_t)received > DANTE_POOL_CHUNK_SIZE) {
            source->packets_truncated++;
            zst_buffer_unref(buffer);
            continue;
        }
        buffer->memory.size = (size_t)received;
        source_update_rtp_stats(source, buffer->memory.data, buffer->memory.size);
        source->packets_received++;
        source->bytes_received += (uint64_t)received;
        atomic_store_explicit(&source->last_packet_time_ns, monotonic_time_ns(),
                              memory_order_release);

        pthread_mutex_lock(&source->queue_lock);
        if (source->queue_count >= source->ring_cap) {
            source->queue_overflow++;
            pthread_mutex_unlock(&source->queue_lock);
            zst_buffer_unref(buffer);
            continue;
        }
        source->ring[source->queue_tail] = buffer;
        source->queue_tail = (source->queue_tail + 1) % source->ring_cap;
        source->queue_count++;
        pthread_cond_signal(&source->queue_cond);
        pthread_mutex_unlock(&source->queue_lock);
    }
    return NULL;
}

static void
source_reader_stop(dante_udp_source_t* source)
{
    if (!source) return;
    if (source->reader_started) {
        atomic_store_explicit(&source->reader_running, 0, memory_order_release);
        pthread_mutex_lock(&source->queue_lock);
        pthread_cond_broadcast(&source->queue_cond);
        pthread_mutex_unlock(&source->queue_lock);
        if (source->fd >= 0) shutdown(source->fd, SHUT_RDWR);
        pthread_join(source->reader_thread, NULL);
        source->reader_started = 0;
        pthread_mutex_lock(&source->queue_lock);
        for (size_t i = 0; i < source->queue_count; i++) {
            size_t idx = (source->queue_head + i) % source->ring_cap;
            zst_buffer_unref(source->ring[idx]);
        }
        source->queue_head = 0;
        source->queue_tail = 0;
        source->queue_count = 0;
        pthread_mutex_unlock(&source->queue_lock);
    }
    if (source->fd >= 0) source_leave_and_close(source);
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

    /* open() can be re-entered on a previously opened element; stop any stale
     * reader thread before touching the socket again. */
    if (source->reader_started) source_reader_stop(source);

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
    /* Bursty QDMA delivery plus a busy decode path can out-run the kernel's
     * default receive buffer (~212 KB), which shows up as UDP RcvbufErrors
     * and RTP sequence gaps.  Ask for a large buffer; the kernel reports the
     * actual (capped by net.core.rmem_max) value back. */
    if (source->socket_rx_buffer > 0 &&
        setsockopt(source->fd, SOL_SOCKET, SO_RCVBUF, &source->socket_rx_buffer,
                   sizeof(source->socket_rx_buffer)) == 0) {
        int actual = 0;
        socklen_t actual_len = sizeof(actual);
        if (getsockopt(source->fd, SOL_SOCKET, SO_RCVBUF, &actual, &actual_len) == 0) {
            ZST_LOG_INFO("udpsrc", "socket RX buffer %d bytes (requested %d)",
                         actual, source->socket_rx_buffer);
        }
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
    source->rtp_packets = 0;
    source->rtp_lost = 0;
    source->rtp_out_of_order = 0;
    source->rtp_expected_seq = 0;
    source->rtp_have_seq = 0;
    source->last_packet_address[0] = '\0';
    source->last_packet_port = 0;
    source->last_packet_size = 0;
    atomic_store_explicit(&source->last_packet_time_ns, 0, memory_order_release);

    if (source->ring) free(source->ring);
    source->ring = calloc(source->ring_cap, sizeof(zst_buffer_t*));
    if (!source->ring) return ZST_ERROR;
    source->queue_head = 0;
    source->queue_tail = 0;
    source->queue_count = 0;
    source->queue_overflow = 0;
    atomic_store_explicit(&source->reader_running, 1, memory_order_release);
    if (pthread_create(&source->reader_thread, NULL, source_reader_main, source) != 0) {
        atomic_store_explicit(&source->reader_running, 0, memory_order_release);
        free(source->ring);
        source->ring = NULL;
        return ZST_ERROR;
    }
    source->reader_started = 1;
    return ZST_OK;
}

static zst_result_t
source_close(zst_element_t* element)
{
    dante_udp_source_t* source = element->priv;
    source_reader_stop(source);
    if (source->ring) {
        free(source->ring);
        source->ring = NULL;
    }
    pthread_cond_destroy(&source->queue_cond);
    pthread_mutex_destroy(&source->queue_lock);
    return ZST_OK;
}

static zst_result_t
source_process(zst_element_t* element, zst_buffer_t* input, zst_buffer_t** output)
{
    dante_udp_source_t* source = element->priv;
    (void)input;
    if (!output) return ZST_ERROR;
    *output = NULL;
    /* Source elements are driven with NULL input by the scheduler.  Until the
     * reader thread is running (or after teardown) this is idle, not a fault. */
    if (!source->reader_started) return ZST_OK;

    pthread_mutex_lock(&source->queue_lock);
    if (source->queue_count == 0 &&
        atomic_load_explicit(&source->reader_running, memory_order_acquire)) {
        int wait_ms = (int)source->read_timeout_ms;
        if (wait_ms > 50) wait_ms = 50;
        if (wait_ms > 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += wait_ms / 1000;
            deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            while (source->queue_count == 0 &&
                   atomic_load_explicit(&source->reader_running, memory_order_acquire)) {
                int rc = pthread_cond_timedwait(&source->queue_cond, &source->queue_lock,
                                                &deadline);
                if (rc != 0) break;
            }
        }
    }
    if (source->queue_count == 0) {
        pthread_mutex_unlock(&source->queue_lock);
        return atomic_load_explicit(&source->reader_running, memory_order_acquire)
            ? ZST_TIMEOUT : ZST_OK;
    }
    zst_buffer_t* buffer = source->ring[source->queue_head];
    source->queue_head = (source->queue_head + 1) % source->ring_cap;
    source->queue_count--;
    pthread_mutex_unlock(&source->queue_lock);

    buffer->pts = element->clock ? zst_clock_get_time(element->clock) : 0;
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
    if (strcmp(name, "socket-rx-buffer") == 0) {
        if (!parse_uint(value, 0, 67108864, &number)) return ZST_ERROR;
        source->socket_rx_buffer = (int)number;
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
    if (strcmp(name, "socket-rx-buffer") == 0) RETURN_UINT(source->socket_rx_buffer);
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
    if (strcmp(name, "rtp-packets") == 0) RETURN_UINT(source->rtp_packets);
    if (strcmp(name, "rtp-lost") == 0) RETURN_UINT(source->rtp_lost);
    if (strcmp(name, "rtp-out-of-order") == 0) RETURN_UINT(source->rtp_out_of_order);
    if (strcmp(name, "rtp-loss-rate-ppm") == 0) {
        uint64_t total = source->rtp_packets + source->rtp_lost;
        RETURN_UINT(total ? (source->rtp_lost * 1000000ULL) / total : 0);
    }
    if (strcmp(name, "queue-depth") == 0) {
        pthread_mutex_lock(&source->queue_lock);
        uint64_t depth = (uint64_t)source->queue_count;
        pthread_mutex_unlock(&source->queue_lock);
        RETURN_UINT(depth);
    }
    if (strcmp(name, "queue-overflow") == 0) RETURN_UINT(source->queue_overflow);
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
    source->ring_cap = DANTE_UDP_SOURCE_RING_CAP;
    pthread_mutex_init(&source->queue_lock, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&source->queue_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    strcpy(source->local_address, "0.0.0.0");
    strcpy(source->multicast_interface_address, "0.0.0.0");
    source->port = 5004;
    source->read_timeout_ms = 10;
    source->socket_rx_buffer = 8 * 1024 * 1024;
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
    { "last-packet-time-ns", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Monotonic timestamp of most recent accepted datagram" },
    { "rtp-packets", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Accepted RTP packets with valid v2 headers" },
    { "rtp-lost", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Estimated RTP sequence gaps" },
    { "rtp-out-of-order", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Late or duplicate RTP packets" },
    { "rtp-loss-rate-ppm", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Estimated RTP loss rate in parts per million" },
    { "queue-depth", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Datagrams currently parked by the reader thread" },
    { "queue-overflow", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE, "0", "Datagrams dropped because the reader ring was full" }
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
