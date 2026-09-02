#define _POSIX_C_SOURCE 200809L

#include "dante_dep_context.h"
#include "dante_dep_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint8_t* address;
    size_t length;
} dep_mapping_t;

struct dep_context {
    char* name;
    unsigned references;
    pthread_mutex_t lock;
    pthread_t worker;
    bool worker_running;
    bool stop_worker;
    dep_endpoint_t* endpoints;
    dep_mapping_t control;
    dep_mapping_t tx;
    dep_mapping_t rx;
    dep_shared_header_t* header;
    dep_shared_header_t metadata;
    sem_t* timing;
    uint8_t* tx_first;
    uint8_t* rx_first;
    uint64_t next_period;
    uint32_t reset_serial;
    uint64_t fallback_samples;
    uint64_t pts_origin;
    uint64_t last_pts;
    bool pts_origin_set;
    struct dep_context* next;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static dep_context_t* registry;

static bool endpoint_channels_valid(const dep_endpoint_t* endpoint,
                                    const dep_shared_header_t* header);
static bool endpoint_prepare(dep_endpoint_t* endpoint, uint32_t period_samples,
                             uint32_t sample_rate);

static bool add_size(size_t a, size_t b, size_t* out)
{
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}

static bool mul_size(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static char* canonical_name(const char* name)
{
    size_t length;
    char* canonical;
    if (!name || !*name) return NULL;
    if (name[0] == '/') return strdup(name);
    length = strlen(name);
    canonical = malloc(length + 2);
    if (!canonical) return NULL;
    canonical[0] = '/';
    memcpy(canonical + 1, name, length + 1);
    return canonical;
}

static void mapping_close(dep_mapping_t* mapping)
{
    if (mapping->address) munmap(mapping->address, mapping->length);
    if (mapping->fd >= 0) close(mapping->fd);
    mapping->fd = -1;
    mapping->address = NULL;
    mapping->length = 0;
}

static int open_named(const char* name, int (*open_fn)(const char*, int), int flags)
{
    int descriptor = open_fn(name, flags);
    if (descriptor >= 0 || name[0] == '/') return descriptor;
    size_t length = strlen(name);
    char* normalized = malloc(length + 2);
    if (!normalized) return -1;
    normalized[0] = '/';
    memcpy(normalized + 1, name, length + 1);
    descriptor = open_fn(normalized, flags);
    free(normalized);
    return descriptor;
}

static int shm_open_adapter(const char* name, int flags)
{
    return shm_open(name, flags, 0);
}

static bool mapping_open(dep_mapping_t* mapping, const char* name)
{
    struct stat status;
    void* address;
    mapping->fd = open_named(name, shm_open_adapter, O_RDWR);
    if (mapping->fd < 0) return false;
    if (fstat(mapping->fd, &status) != 0 || status.st_size <= 0 ||
        (uintmax_t)status.st_size > SIZE_MAX) {
        mapping_close(mapping);
        return false;
    }
    mapping->length = (size_t)status.st_size;
    address = mmap(NULL, mapping->length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   mapping->fd, 0);
    if (address == MAP_FAILED) {
        mapping->address = NULL;
        mapping_close(mapping);
        return false;
    }
    mapping->address = address;
    return true;
}

static sem_t* semaphore_open_flexible(const char* name)
{
    sem_t* semaphore = sem_open(name, 0);
    if (semaphore != SEM_FAILED || name[0] == '/') return semaphore;
    size_t length = strlen(name);
    char* normalized = malloc(length + 2);
    if (!normalized) return SEM_FAILED;
    normalized[0] = '/';
    memcpy(normalized + 1, name, length + 1);
    semaphore = sem_open(normalized, 0);
    free(normalized);
    return semaphore;
}

static void context_disconnect(dep_context_t* context)
{
    if (context->timing != SEM_FAILED) sem_close(context->timing);
    context->timing = SEM_FAILED;
    context->header = NULL;
    context->tx_first = NULL;
    context->rx_first = NULL;
    mapping_close(&context->rx);
    mapping_close(&context->tx);
    mapping_close(&context->control);
    for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
        pthread_mutex_lock(&endpoint->lock);
        endpoint->active = false;
        endpoint->sample_rate = 0;
        endpoint->used_frames = 0;
        endpoint->read_frame = 0;
        pthread_cond_broadcast(&endpoint->ready);
        pthread_mutex_unlock(&endpoint->lock);
    }
}

static bool region_valid(size_t length, uint32_t offset, size_t bytes)
{
    size_t end;
    return add_size((size_t)offset, bytes, &end) && end <= length;
}

static bool channel_region_valid(size_t length, uint32_t offset,
                                 uint32_t channels, uint32_t bytes_per_channel)
{
    size_t bytes;
    return mul_size(channels, bytes_per_channel, &bytes) &&
           region_valid(length, offset, bytes);
}

static bool context_connect(dep_context_t* context)
{
    dep_shared_header_t snapshot;
    dep_timing_descriptor_t descriptor;
    uint32_t before;
    uint32_t after;
    uint32_t magic_before;
    uint32_t magic_after;
    char* tx_name = NULL;
    char* rx_name = NULL;
    bool separate;

    context_disconnect(context);
    if (!mapping_open(&context->control, context->name) ||
        context->control.length < sizeof(snapshot)) goto fail;
    before = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->reset_serial,
                              __ATOMIC_ACQUIRE);
    magic_before = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->magic,
                                   __ATOMIC_ACQUIRE);
    if ((before & 1u) != 0) goto fail;
    atomic_thread_fence(memory_order_acquire);
    memcpy(&snapshot, context->control.address, sizeof(snapshot));
    atomic_thread_fence(memory_order_acquire);
    after = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->reset_serial,
                             __ATOMIC_ACQUIRE);
    magic_after = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->magic,
                                  __ATOMIC_ACQUIRE);
    separate = (snapshot.flags & DEP_LAYOUT_SEPARATE) != 0;
    if (after != before || (after & 1u) != 0 || magic_before != DEP_HEADER_MAGIC ||
        magic_after != DEP_HEADER_MAGIC || snapshot.magic != DEP_HEADER_MAGIC ||
        snapshot.object_bytes < sizeof(snapshot) ||
        (!separate && snapshot.object_bytes > context->control.length) ||
        snapshot.metadata_bytes < sizeof(snapshot) ||
        snapshot.metadata_bytes > snapshot.object_bytes ||
        snapshot.metadata_bytes > context->control.length ||
        snapshot.encoding != DEP_ENCODING_PCM32 || snapshot.sample_rate == 0 ||
        snapshot.samples_per_channel == 0 || snapshot.samples_per_period == 0 ||
        snapshot.samples_per_period > snapshot.samples_per_channel ||
        (uint64_t)snapshot.bytes_per_channel !=
            (uint64_t)snapshot.samples_per_channel * sizeof(int32_t) ||
        snapshot.timing_offset == 0 ||
        !region_valid(snapshot.object_bytes, snapshot.timing_offset, sizeof(descriptor))) goto fail;
    memcpy(&descriptor, context->control.address + snapshot.timing_offset, sizeof(descriptor));
    after = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->reset_serial,
                             __ATOMIC_ACQUIRE);
    magic_after = __atomic_load_n(&((dep_shared_header_t*)context->control.address)->magic,
                                  __ATOMIC_ACQUIRE);
    if (after != before || (after & 1u) != 0 || magic_after != DEP_HEADER_MAGIC ||
        descriptor.descriptor_bytes < sizeof(descriptor) ||
        !region_valid(snapshot.object_bytes, snapshot.timing_offset,
                      descriptor.descriptor_bytes) ||
        descriptor.kind != DEP_TIMING_SIGNAL_EVENT ||
        !memchr(descriptor.name, '\0', sizeof(descriptor.name))) goto fail;

    if (separate) {
        size_t base = strlen(context->name);
        if (base > SIZE_MAX - 3) goto fail;
        tx_name = malloc(base + 3);
        rx_name = malloc(base + 3);
        if (!tx_name || !rx_name) goto fail;
        memcpy(tx_name, context->name, base);
        memcpy(rx_name, context->name, base);
        memcpy(tx_name + base, "Tx", 3);
        memcpy(rx_name + base, "Rx", 3);
        if (!mapping_open(&context->tx, tx_name) || !mapping_open(&context->rx, rx_name)) goto fail;
    }

    dep_mapping_t* tx_map = separate ? &context->tx : &context->control;
    dep_mapping_t* rx_map = separate ? &context->rx : &context->control;
    if (!channel_region_valid(tx_map->length, snapshot.tx_offset,
                              snapshot.tx_channels, snapshot.bytes_per_channel) ||
        !channel_region_valid(rx_map->length, snapshot.rx_offset,
                              snapshot.rx_channels, snapshot.bytes_per_channel)) goto fail;
    context->timing = semaphore_open_flexible(descriptor.name);
    if (context->timing == SEM_FAILED) goto fail;
    context->header = (dep_shared_header_t*)context->control.address;
    context->metadata = snapshot;
    context->tx_first = tx_map->address + snapshot.tx_offset;
    context->rx_first = rx_map->address + snapshot.rx_offset;
    context->next_period = snapshot.period_count;
    context->reset_serial = after;
    context->fallback_samples = 0;
    context->pts_origin_set = false;
    for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
        if (atomic_load_explicit(&endpoint->enabled, memory_order_acquire) &&
            endpoint_channels_valid(endpoint, &context->metadata))
            (void)endpoint_prepare(endpoint, snapshot.samples_per_period,
                                   snapshot.sample_rate);
    }
    free(tx_name);
    free(rx_name);
    return true;

fail:
    free(tx_name);
    free(rx_name);
    context_disconnect(context);
    return false;
}

static bool endpoint_channels_valid(const dep_endpoint_t* endpoint,
                                    const dep_shared_header_t* header)
{
    uint32_t available = endpoint->direction == DEP_ENDPOINT_RX
        ? header->rx_channels : header->tx_channels;
    for (uint32_t i = 0; i < endpoint->channel_count; ++i)
        if (endpoint->channel_map[i] >= available) return false;
    return true;
}

static bool endpoint_prepare(dep_endpoint_t* endpoint, uint32_t period_samples,
                             uint32_t sample_rate)
{
    size_t frames;
    size_t sample_count;
    if (!mul_size(endpoint->queue_periods, period_samples, &frames) || frames == 0)
        return false;
    if (endpoint->block_samples > frames) frames = endpoint->block_samples;
    if (!mul_size(frames, endpoint->channel_count, &sample_count) ||
        sample_count > SIZE_MAX / sizeof(int32_t) || frames > SIZE_MAX / sizeof(uint64_t)) return false;
    pthread_mutex_lock(&endpoint->lock);
    if (endpoint->capacity_frames != frames ||
        endpoint->configured_period_samples != period_samples) {
        int32_t* samples = malloc(sample_count * sizeof(int32_t));
        uint64_t* timestamps = endpoint->direction == DEP_ENDPOINT_RX
            ? malloc(frames * sizeof(uint64_t)) : NULL;
        if (!samples || (endpoint->direction == DEP_ENDPOINT_RX && !timestamps)) {
            free(samples);
            free(timestamps);
            pthread_mutex_unlock(&endpoint->lock);
            return false;
        }
        free(endpoint->samples);
        free(endpoint->timestamps);
        endpoint->samples = samples;
        endpoint->timestamps = timestamps;
        endpoint->capacity_frames = frames;
        endpoint->configured_period_samples = period_samples;
        endpoint->read_frame = 0;
        endpoint->used_frames = 0;
    }
    endpoint->sample_rate = sample_rate;
    endpoint->active = atomic_load_explicit(&endpoint->enabled, memory_order_acquire);
    pthread_mutex_unlock(&endpoint->lock);
    return true;
}

static uint64_t samples_to_ns(uint64_t samples, uint32_t rate)
{
    uint64_t whole = samples / rate;
    uint64_t remainder = samples % rate;
    uint64_t value;
    uint64_t fraction;
    if (whole > UINT64_MAX / UINT64_C(1000000000)) return UINT64_MAX;
    value = whole * UINT64_C(1000000000);
    fraction = remainder * UINT64_C(1000000000) / rate;
    return fraction > UINT64_MAX - value ? UINT64_MAX : value + fraction;
}

static uint64_t period_pts(dep_context_t* context, uint64_t period,
                           uint32_t period_samples, uint32_t rate)
{
    dep_shared_header_t* header = context->header;
    uint32_t flags1 = __atomic_load_n(&header->flags, __ATOMIC_ACQUIRE);
    uint64_t pair_period = __atomic_load_n(&header->period_count, __ATOMIC_RELAXED);
    uint64_t pair_time = __atomic_load_n(&header->monotonic_ns, __ATOMIC_RELAXED);
    uint32_t flags2 = __atomic_load_n(&header->flags, __ATOMIC_ACQUIRE);
    uint64_t pair_period2 = __atomic_load_n(&header->period_count, __ATOMIC_RELAXED);
    uint64_t pair_time2 = __atomic_load_n(&header->monotonic_ns, __ATOMIC_RELAXED);
    if (!(flags1 & DEP_TIMING_WRITE_ACTIVE) && flags1 == flags2 &&
        pair_period == pair_period2 && pair_time == pair_time2 && pair_time != 0) {
        uint64_t delta = period >= pair_period
            ? period - pair_period : pair_period - period;
        uint64_t delta_samples;
        if (delta <= UINT64_MAX / period_samples) {
            delta_samples = delta * period_samples;
            uint64_t whole = delta_samples / rate;
            uint64_t remainder = delta_samples % rate;
            if (whole <= UINT64_MAX / UINT64_C(1000000000)) {
                uint64_t nanoseconds = whole * UINT64_C(1000000000) +
                    remainder * UINT64_C(1000000000) / rate;
                if (period >= pair_period) {
                    if (nanoseconds <= UINT64_MAX - pair_time)
                        return pair_time + nanoseconds;
                } else if (nanoseconds <= pair_time) {
                    return pair_time - nanoseconds;
                }
            }
        }
    }
    return samples_to_ns(context->fallback_samples, rate);
}

static size_t period_start(uint64_t period, uint32_t samples, uint32_t capacity)
{
    return (size_t)(((period % capacity) * (uint64_t)samples) % capacity);
}

static uint64_t running_time_pts(dep_context_t* context, uint64_t timestamp)
{
    uint64_t pts;
    if (!context->pts_origin_set) {
        context->pts_origin = timestamp;
        context->last_pts = 0;
        context->pts_origin_set = true;
        return 0;
    }
    pts = timestamp >= context->pts_origin ? timestamp - context->pts_origin
                                            : samples_to_ns(context->fallback_samples,
                                                            context->metadata.sample_rate);
    if (pts < context->last_pts) pts = context->last_pts;
    context->last_pts = pts;
    return pts;
}

static void fifo_drop(dep_endpoint_t* endpoint, size_t frames)
{
    if (frames > endpoint->used_frames) frames = endpoint->used_frames;
    endpoint->read_frame = (endpoint->read_frame + frames) % endpoint->capacity_frames;
    endpoint->used_frames -= frames;
    endpoint->dropped_frames += frames;
}

static bool receive_period(dep_context_t* context, dep_endpoint_t* endpoint,
                           const dep_shared_header_t* header, uint64_t period,
                           uint64_t pts)
{
    size_t frames = header->samples_per_period;
    size_t start = period_start(period, header->samples_per_period,
                                header->samples_per_channel);
    uint64_t ring_periods = header->samples_per_channel / header->samples_per_period;
    uint64_t latest = __atomic_load_n(&context->header->period_count, __ATOMIC_ACQUIRE);
    if (latest < period + 1 || latest - period > ring_periods) return false;
    pthread_mutex_lock(&endpoint->lock);
    size_t original_read = endpoint->read_frame;
    size_t original_used = endpoint->used_frames;
    if (frames > endpoint->capacity_frames - endpoint->used_frames) {
        size_t remove = frames - (endpoint->capacity_frames - endpoint->used_frames);
        fifo_drop(endpoint, remove);
        endpoint->overruns++;
    }
    for (size_t i = 0; i < frames; ++i) {
        size_t slot = (endpoint->read_frame + endpoint->used_frames + i) % endpoint->capacity_frames;
        size_t ring_sample = (start + i) % header->samples_per_channel;
        for (uint32_t channel = 0; channel < endpoint->channel_count; ++channel) {
            int32_t* plane = (int32_t*)(context->rx_first +
                (size_t)endpoint->channel_map[channel] * header->bytes_per_channel);
            endpoint->samples[slot * endpoint->channel_count + channel] = plane[ring_sample];
        }
        uint64_t offset = samples_to_ns(i, header->sample_rate);
        endpoint->timestamps[slot] = offset > UINT64_MAX - pts ? UINT64_MAX : pts + offset;
    }
    endpoint->used_frames += frames;
    latest = __atomic_load_n(&context->header->period_count, __ATOMIC_ACQUIRE);
    if (latest < period + 1 || latest - period > ring_periods) {
        endpoint->read_frame = original_read;
        endpoint->used_frames = original_used;
        endpoint->overruns++;
        pthread_mutex_unlock(&endpoint->lock);
        return false;
    }
    endpoint->periods++;
    pthread_cond_broadcast(&endpoint->ready);
    pthread_mutex_unlock(&endpoint->lock);
    return true;
}

static void transmit_period(dep_context_t* context, dep_endpoint_t* endpoint,
                            uint64_t period)
{
    const dep_shared_header_t* header = &context->metadata;
    size_t frames = header->samples_per_period;
    uint64_t lead = (uint64_t)endpoint->tx_lead_us * header->sample_rate / UINT64_C(1000000);
    size_t start = period_start(period, header->samples_per_period,
                                header->samples_per_channel);
    start = (start + lead % header->samples_per_channel) %
            header->samples_per_channel;
    pthread_mutex_lock(&endpoint->lock);
    bool short_read = endpoint->used_frames < frames;
    for (size_t i = 0; i < frames; ++i) {
        bool available = i < endpoint->used_frames;
        size_t slot = available ? (endpoint->read_frame + i) % endpoint->capacity_frames : 0;
        size_t ring_sample = (start + i) % header->samples_per_channel;
        for (uint32_t channel = 0; channel < endpoint->channel_count; ++channel) {
            int32_t* plane = (int32_t*)(context->tx_first +
                (size_t)endpoint->channel_map[channel] * header->bytes_per_channel);
            plane[ring_sample] = available
                ? endpoint->samples[slot * endpoint->channel_count + channel] : 0;
        }
    }
    fifo_drop(endpoint, endpoint->used_frames < frames ? endpoint->used_frames : frames);
    if (short_read) endpoint->underflows++;
    endpoint->periods++;
    pthread_mutex_unlock(&endpoint->lock);
}

static uint32_t reconnect_delay(dep_context_t* context)
{
    uint32_t delay = 100;
    bool found = false;
    for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
        if (atomic_load_explicit(&endpoint->enabled, memory_order_acquire) &&
            (!found || endpoint->reconnect_ms < delay)) {
            delay = endpoint->reconnect_ms;
            found = true;
        }
    }
    return found ? delay : 20;
}

static void sleep_ms(uint32_t milliseconds)
{
    struct timespec duration = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    nanosleep(&duration, NULL);
}

static void wait_for_signal(dep_context_t* context)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 20000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (sem_timedwait(context->timing, &deadline) != 0 && errno == EINTR) {}
}

static void mark_reset(dep_context_t* context)
{
    for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
        pthread_mutex_lock(&endpoint->lock);
        endpoint->resets++;
        pthread_mutex_unlock(&endpoint->lock);
    }
}

static void* context_worker(void* opaque)
{
    dep_context_t* context = opaque;
    for (;;) {
        pthread_mutex_lock(&context->lock);
        bool stopping = context->stop_worker;
        bool connected = context->header != NULL;
        uint32_t delay = reconnect_delay(context);
        pthread_mutex_unlock(&context->lock);
        if (stopping) break;
        if (!connected) {
            pthread_mutex_lock(&context->lock);
            if (!context->stop_worker) (void)context_connect(context);
            connected = context->header != NULL;
            pthread_mutex_unlock(&context->lock);
            if (!connected) sleep_ms(delay);
            continue;
        }

        wait_for_signal(context);
        pthread_mutex_lock(&context->lock);
        if (context->stop_worker) {
            pthread_mutex_unlock(&context->lock);
            break;
        }
        dep_shared_header_t* live_header = context->header;
        const dep_shared_header_t* header = &context->metadata;
        uint32_t magic = __atomic_load_n(&live_header->magic, __ATOMIC_ACQUIRE);
        uint32_t reset = __atomic_load_n(&live_header->reset_serial, __ATOMIC_ACQUIRE);
        if (magic != DEP_HEADER_MAGIC || reset != context->reset_serial || (reset & 1u)) {
            mark_reset(context);
            context_disconnect(context);
            pthread_mutex_unlock(&context->lock);
            continue;
        }
        uint64_t current = __atomic_load_n(&live_header->period_count, __ATOMIC_ACQUIRE);
        if (current < context->next_period) {
            mark_reset(context);
            context_disconnect(context);
            pthread_mutex_unlock(&context->lock);
            continue;
        }
        uint64_t available = current - context->next_period;
        uint64_t ring_periods = header->samples_per_channel / header->samples_per_period;
        if (ring_periods == 0) {
            context_disconnect(context);
            pthread_mutex_unlock(&context->lock);
            continue;
        }
        if (available > ring_periods) {
            uint64_t lost = available - ring_periods;
            context->next_period += lost;
            for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
                if (atomic_load_explicit(&endpoint->enabled, memory_order_acquire) &&
                    endpoint->direction == DEP_ENDPOINT_RX) {
                    pthread_mutex_lock(&endpoint->lock);
                    endpoint->overruns += lost;
                    pthread_mutex_unlock(&endpoint->lock);
                }
            }
        }
        atomic_thread_fence(memory_order_acquire);
        while (context->next_period < current) {
            uint64_t period = context->next_period++;
            uint64_t pts = running_time_pts(context, period_pts(context, period,
                                                                 header->samples_per_period,
                                                                 header->sample_rate));
            for (dep_endpoint_t* endpoint = context->endpoints; endpoint; endpoint = endpoint->next) {
                if (!atomic_load_explicit(&endpoint->enabled, memory_order_acquire) ||
                    !endpoint_channels_valid(endpoint, header) ||
                    !endpoint_prepare(endpoint, header->samples_per_period, header->sample_rate)) continue;
                if (endpoint->direction == DEP_ENDPOINT_RX)
                    (void)receive_period(context, endpoint, header, period, pts);
                else
                    transmit_period(context, endpoint, period);
            }
            atomic_thread_fence(memory_order_release);
            if (context->fallback_samples <= UINT64_MAX - header->samples_per_period)
                context->fallback_samples += header->samples_per_period;
        }
        pthread_mutex_unlock(&context->lock);
    }
    pthread_mutex_lock(&context->lock);
    context_disconnect(context);
    context->worker_running = false;
    pthread_mutex_unlock(&context->lock);
    return NULL;
}

static bool parse_channels(const char* text, uint32_t** map_out, uint32_t* count_out)
{
    char* copy;
    char* cursor;
    uint32_t* map = NULL;
    uint32_t count = 0;
    if (!text || !*text) return false;
    copy = strdup(text);
    if (!copy) return false;
    cursor = copy;
    while (*cursor) {
        char* end;
        unsigned long value;
        if (*cursor == '-' || *cursor == '+' || *cursor == ' ' || *cursor == '\t') goto fail;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno || end == cursor || value > UINT32_MAX ||
            (*end != ',' && *end != '\0')) goto fail;
        for (uint32_t i = 0; i < count; ++i) if (map[i] == (uint32_t)value) goto fail;
        uint32_t* grown = realloc(map, ((size_t)count + 1) * sizeof(*map));
        if (!grown) goto fail;
        map = grown;
        map[count++] = (uint32_t)value;
        if (*end == '\0') break;
        cursor = end + 1;
        if (*cursor == '\0') goto fail;
    }
    free(copy);
    *map_out = map;
    *count_out = count;
    return count != 0;
fail:
    free(copy);
    free(map);
    return false;
}

zst_result_t dep_endpoint_init(dep_endpoint_t* endpoint,
                               dep_endpoint_direction_t direction,
                               const char* channels, uint32_t queue_periods,
                               uint32_t block_samples, uint32_t tx_lead_us,
                               uint32_t reconnect_ms)
{
    memset(endpoint, 0, sizeof(*endpoint));
    if (queue_periods == 0 || reconnect_ms == 0 ||
        !parse_channels(channels, &endpoint->channel_map, &endpoint->channel_count)) return ZST_ERROR;
    endpoint->direction = direction;
    endpoint->queue_periods = queue_periods;
    endpoint->block_samples = block_samples;
    endpoint->tx_lead_us = tx_lead_us;
    endpoint->reconnect_ms = reconnect_ms;
    atomic_init(&endpoint->enabled, false);
    if (pthread_mutex_init(&endpoint->lock, NULL) != 0) goto fail;
    if (pthread_cond_init(&endpoint->ready, NULL) != 0) {
        pthread_mutex_destroy(&endpoint->lock);
        goto fail;
    }
    return ZST_OK;
fail:
    free(endpoint->channel_map);
    endpoint->channel_map = NULL;
    return ZST_ERROR;
}

void dep_endpoint_deinit(dep_endpoint_t* endpoint)
{
    if (!endpoint) return;
    dep_context_detach(endpoint);
    pthread_cond_destroy(&endpoint->ready);
    pthread_mutex_destroy(&endpoint->lock);
    free(endpoint->channel_map);
    free(endpoint->samples);
    free(endpoint->timestamps);
    memset(endpoint, 0, sizeof(*endpoint));
}

dep_context_t* dep_context_acquire(const char* shm_name)
{
    dep_context_t* context;
    char* canonical = canonical_name(shm_name);
    if (!canonical) return NULL;
    pthread_mutex_lock(&registry_lock);
    for (context = registry; context; context = context->next) {
        if (strcmp(context->name, canonical) == 0) {
            context->references++;
            pthread_mutex_unlock(&registry_lock);
            free(canonical);
            return context;
        }
    }
    context = calloc(1, sizeof(*context));
    if (!context) {
        free(canonical);
        pthread_mutex_unlock(&registry_lock);
        return NULL;
    }
    context->name = canonical;
    context->references = 1;
    context->control.fd = context->tx.fd = context->rx.fd = -1;
    context->timing = SEM_FAILED;
    if (!context->name || pthread_mutex_init(&context->lock, NULL) != 0) {
        free(context->name);
        free(context);
        pthread_mutex_unlock(&registry_lock);
        return NULL;
    }
    context->next = registry;
    registry = context;
    pthread_mutex_unlock(&registry_lock);
    return context;
}

void dep_context_release(dep_context_t* context)
{
    if (!context) return;
    pthread_mutex_lock(&registry_lock);
    if (--context->references != 0) {
        pthread_mutex_unlock(&registry_lock);
        return;
    }
    dep_context_t** link = &registry;
    while (*link && *link != context) link = &(*link)->next;
    if (*link) *link = context->next;
    pthread_mutex_unlock(&registry_lock);
    pthread_mutex_lock(&context->lock);
    context->stop_worker = true;
    bool join = context->worker_running;
    pthread_mutex_unlock(&context->lock);
    if (join) pthread_join(context->worker, NULL);
    pthread_mutex_lock(&context->lock);
    context_disconnect(context);
    pthread_mutex_unlock(&context->lock);
    pthread_mutex_destroy(&context->lock);
    free(context->name);
    free(context);
}

zst_result_t dep_context_attach(dep_context_t* context, dep_endpoint_t* endpoint)
{
    if (!context || !endpoint || endpoint->context) return ZST_ERROR;
    pthread_mutex_lock(&context->lock);
    endpoint->context = context;
    endpoint->next = context->endpoints;
    context->endpoints = endpoint;
    pthread_mutex_unlock(&context->lock);
    return ZST_OK;
}

void dep_context_detach(dep_endpoint_t* endpoint)
{
    dep_context_t* context = endpoint ? endpoint->context : NULL;
    if (!context) return;
    dep_endpoint_stop(endpoint);
    pthread_mutex_lock(&context->lock);
    dep_endpoint_t** link = &context->endpoints;
    while (*link && *link != endpoint) link = &(*link)->next;
    if (*link) *link = endpoint->next;
    endpoint->context = NULL;
    endpoint->next = NULL;
    pthread_mutex_unlock(&context->lock);
}

zst_result_t dep_endpoint_start(dep_endpoint_t* endpoint)
{
    dep_context_t* context = endpoint ? endpoint->context : NULL;
    if (!context) return ZST_ERROR;
    pthread_mutex_lock(&context->lock);
    atomic_store_explicit(&endpoint->enabled, true, memory_order_release);
    if (!context->worker_running) {
        context->stop_worker = false;
        context->worker_running = true;
        if (pthread_create(&context->worker, NULL, context_worker, context) != 0) {
            context->worker_running = false;
            atomic_store_explicit(&endpoint->enabled, false, memory_order_release);
            pthread_mutex_unlock(&context->lock);
            return ZST_ERROR;
        }
    }
    pthread_mutex_unlock(&context->lock);
    return ZST_OK;
}

void dep_endpoint_stop(dep_endpoint_t* endpoint)
{
    dep_context_t* context = endpoint ? endpoint->context : NULL;
    if (!context) return;
    pthread_mutex_lock(&context->lock);
    atomic_store_explicit(&endpoint->enabled, false, memory_order_release);
    bool any = false;
    for (dep_endpoint_t* item = context->endpoints; item; item = item->next)
        if (atomic_load_explicit(&item->enabled, memory_order_acquire)) any = true;
    if (!any) context->stop_worker = true;
    bool join = !any && context->worker_running;
    pthread_mutex_unlock(&context->lock);
    pthread_mutex_lock(&endpoint->lock);
    endpoint->active = false;
    pthread_cond_broadcast(&endpoint->ready);
    pthread_mutex_unlock(&endpoint->lock);
    if (join) pthread_join(context->worker, NULL);
}

zst_result_t dep_endpoint_read(dep_endpoint_t* endpoint, int32_t** samples_out,
                               uint32_t* frames_out, uint64_t* pts_out,
                               uint32_t* rate_out)
{
    struct timespec deadline;
    if (!endpoint || endpoint->direction != DEP_ENDPOINT_RX || !samples_out ||
        !frames_out || !pts_out || !rate_out) return ZST_ERROR;
    *samples_out = NULL;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 50000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&endpoint->lock);
    size_t wanted = endpoint->block_samples ? endpoint->block_samples
                                             : endpoint->configured_period_samples;
    while (atomic_load_explicit(&endpoint->enabled, memory_order_acquire) &&
           endpoint->used_frames < wanted) {
        if (pthread_cond_timedwait(&endpoint->ready, &endpoint->lock, &deadline) == ETIMEDOUT) break;
        wanted = endpoint->block_samples ? endpoint->block_samples
                                         : endpoint->configured_period_samples;
    }
    if (!atomic_load_explicit(&endpoint->enabled, memory_order_acquire) || wanted == 0 ||
        endpoint->used_frames < wanted) {
        pthread_mutex_unlock(&endpoint->lock);
        return ZST_TIMEOUT;
    }
    size_t sample_count;
    if (wanted > UINT32_MAX || !mul_size(wanted, endpoint->channel_count, &sample_count) ||
        sample_count > SIZE_MAX / sizeof(int32_t)) {
        pthread_mutex_unlock(&endpoint->lock);
        return ZST_ERROR;
    }
    int32_t* output = malloc(sample_count * sizeof(int32_t));
    if (!output) {
        pthread_mutex_unlock(&endpoint->lock);
        return ZST_ERROR;
    }
    *pts_out = endpoint->timestamps[endpoint->read_frame];
    *rate_out = endpoint->sample_rate;
    for (size_t i = 0; i < wanted; ++i) {
        size_t slot = (endpoint->read_frame + i) % endpoint->capacity_frames;
        memcpy(output + i * endpoint->channel_count,
               endpoint->samples + slot * endpoint->channel_count,
               endpoint->channel_count * sizeof(int32_t));
    }
    fifo_drop(endpoint, wanted);
    pthread_mutex_unlock(&endpoint->lock);
    *samples_out = output;
    *frames_out = (uint32_t)wanted;
    return ZST_OK;
}

zst_result_t dep_endpoint_write(dep_endpoint_t* endpoint, const int32_t* samples,
                                uint32_t frames)
{
    if (!endpoint || endpoint->direction != DEP_ENDPOINT_TX || !samples || frames == 0)
        return ZST_ERROR;
    pthread_mutex_lock(&endpoint->lock);
    if (!atomic_load_explicit(&endpoint->enabled, memory_order_acquire) || !endpoint->samples ||
        endpoint->capacity_frames == 0) {
        pthread_mutex_unlock(&endpoint->lock);
        return ZST_AGAIN;
    }
    size_t skip = frames > endpoint->capacity_frames ? frames - endpoint->capacity_frames : 0;
    if (skip) {
        endpoint->overruns++;
        endpoint->dropped_frames += skip;
    }
    size_t accepted = frames - skip;
    if (accepted > endpoint->capacity_frames - endpoint->used_frames) {
        size_t drop = accepted - (endpoint->capacity_frames - endpoint->used_frames);
        fifo_drop(endpoint, drop);
        endpoint->overruns++;
    }
    for (size_t i = 0; i < accepted; ++i) {
        size_t slot = (endpoint->read_frame + endpoint->used_frames + i) % endpoint->capacity_frames;
        memcpy(endpoint->samples + slot * endpoint->channel_count,
               samples + (skip + i) * endpoint->channel_count,
               endpoint->channel_count * sizeof(int32_t));
    }
    endpoint->used_frames += accepted;
    pthread_mutex_unlock(&endpoint->lock);
    return ZST_OK;
}

bool dep_endpoint_is_active(dep_endpoint_t* endpoint)
{
    bool value;
    pthread_mutex_lock(&endpoint->lock);
    value = endpoint->active;
    pthread_mutex_unlock(&endpoint->lock);
    return value;
}

uint32_t dep_endpoint_sample_rate(dep_endpoint_t* endpoint)
{
    uint32_t value;
    pthread_mutex_lock(&endpoint->lock);
    value = endpoint->sample_rate;
    pthread_mutex_unlock(&endpoint->lock);
    return value;
}

uint64_t dep_endpoint_stat(dep_endpoint_t* endpoint, const char* name)
{
    uint64_t value = 0;
    pthread_mutex_lock(&endpoint->lock);
    if (strcmp(name, "periods") == 0 || strcmp(name, "period-count") == 0)
        value = endpoint->periods;
    else if (strcmp(name, "resets") == 0 || strcmp(name, "reset-count") == 0)
        value = endpoint->resets;
    else if (strcmp(name, "overruns") == 0 || strcmp(name, "overrun-count") == 0)
        value = endpoint->overruns;
    else if (strcmp(name, "underflows") == 0 || strcmp(name, "underflow-count") == 0)
        value = endpoint->underflows;
    else if (strcmp(name, "dropped-frames") == 0)
        value = endpoint->dropped_frames;
    pthread_mutex_unlock(&endpoint->lock);
    return value;
}
