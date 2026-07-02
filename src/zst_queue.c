/*=============================================================================
    zst_queue.c — High-Performance Lock-Free Bounded Ring Queue
=============================================================================*/

#define _POSIX_C_SOURCE 199309L

#include "zst_queue.h"
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

/* --- MANDATORY HEADERS FOR ATOMICS & CACHE ALIGNMENT --- */
#include <stdatomic.h>  /* Resolves _Atomic, atomic_store_explicit, memory_order_relaxed, etc. */
#include <stdalign.h>   /* Resolves alignas macro expressions */

#define CACHE_LINE_SIZE 64
#define RING_MASK(q, idx) ((idx) & ((q)->capacity - 1))

typedef struct {
    _Atomic(zst_buffer_t*) buf;
    _Atomic(uint64_t)      sequence;
    _Atomic(zst_time_t)    pts;
    _Atomic(zst_time_t)    duration;
} zst_queue_slot_t;

struct zst_queue {
    zst_queue_config_t cfg;
    uint32_t           capacity;
    bool               has_extra_limits; /* When false, skip zst_queue_is_full() — sequence array
                                            backpressure alone provides the capacity limit, avoiding
                                            a cross-thread read of q->head on every push. */
    zst_queue_slot_t* ring;

    /* Align head and tail indexes to separate cache lines to prevent false sharing */
    alignas(CACHE_LINE_SIZE) _Atomic(uint64_t) head;
    alignas(CACHE_LINE_SIZE) _Atomic(uint64_t) tail;
    
    alignas(CACHE_LINE_SIZE) _Atomic(uint64_t) approx_bytes;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    _Atomic(uint32_t) waiters_push;
    _Atomic(uint32_t) waiters_pop;
};

static inline uint32_t next_power_of_two(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 2 ? 2 : v;
}

zst_queue_t*
zst_queue_create(const zst_queue_config_t* cfg)
{
    zst_queue_t* q = calloc(1, sizeof(*q));
    if (!q) return NULL;

    if (cfg) {
        q->cfg = *cfg;
    } else {
        q->cfg.mode         = ZST_QUEUE_SYNC;
        q->cfg.max_buffers  = 16;
        q->cfg.max_bytes    = 0;
        q->cfg.max_duration = 0;
    }

    /* Enforce a power-of-two capacity to utilize bitwise masking over modulo arithmetic */
    q->capacity = next_power_of_two(q->cfg.max_buffers > 0 ? q->cfg.max_buffers : 16);
    q->has_extra_limits = (q->cfg.max_bytes > 0 || q->cfg.max_duration > 0 || (q->cfg.max_buffers > 0 && q->cfg.max_buffers < q->capacity));
    q->ring = malloc(q->capacity * sizeof(zst_queue_slot_t));
    if (!q->ring) {
        free(q);
        return NULL;
    }

    for (uint32_t i = 0; i < q->capacity; i++) {
        atomic_store_explicit(&q->ring[i].buf, NULL, memory_order_relaxed);
        atomic_store_explicit(&q->ring[i].sequence, (uint64_t)i, memory_order_relaxed);
        atomic_store_explicit(&q->ring[i].pts, 0, memory_order_relaxed);
        atomic_store_explicit(&q->ring[i].duration, 0, memory_order_relaxed);
    }

    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->approx_bytes, 0, memory_order_relaxed);

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    atomic_store_explicit(&q->waiters_push, 0, memory_order_relaxed);
    atomic_store_explicit(&q->waiters_pop, 0, memory_order_relaxed);

    return q;
}

void
zst_queue_destroy(zst_queue_t* q)
{
    if (!q) return;
    zst_queue_flush(q);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->lock);
    free(q->ring);
    free(q);
}

static zst_time_t
zst_queue_get_duration(zst_queue_t* q)
{
    uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (tail <= head) return 0;

    // Find the first fully written slot from the head
    uint64_t first_idx = head;
    zst_time_t head_pts = 0;
    bool found_head = false;
    while (first_idx < tail) {
        zst_queue_slot_t* slot = &q->ring[RING_MASK(q, first_idx)];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (seq == first_idx + 1) {
            head_pts = atomic_load_explicit(&slot->pts, memory_order_relaxed);
            found_head = true;
            break;
        }
        first_idx++;
    }

    // Find the last fully written slot from the tail
    uint64_t last_idx = tail - 1;
    zst_time_t tail_pts = 0;
    bool found_tail = false;
    while (last_idx >= head && last_idx >= first_idx) {
        zst_queue_slot_t* slot = &q->ring[RING_MASK(q, last_idx)];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (seq == last_idx + 1) {
            tail_pts = atomic_load_explicit(&slot->pts, memory_order_relaxed);
            found_tail = true;
            break;
        }
        if (last_idx == 0) break;
        last_idx--;
    }

    if (found_head && found_tail && tail_pts >= head_pts && head_pts != 0) {
        return tail_pts - head_pts;
    }

    /* Fallback: sum of durations of all fully written slots */
    zst_time_t sum = 0;
    for (uint64_t i = head; i < tail; i++) {
        zst_queue_slot_t* slot = &q->ring[RING_MASK(q, i)];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (seq == i + 1) {
            sum += atomic_load_explicit(&slot->duration, memory_order_relaxed);
        }
    }
    return sum;
}

static bool
zst_queue_is_full(zst_queue_t* q)
{
    if (q->cfg.max_buffers > 0 && zst_queue_size(q) >= q->cfg.max_buffers) {
        return true;
    }
    if (q->cfg.max_bytes > 0 && 
        atomic_load_explicit(&q->approx_bytes, memory_order_relaxed) >= q->cfg.max_bytes) {
        return true;
    }
    if (q->cfg.max_duration > 0 && zst_queue_get_duration(q) >= q->cfg.max_duration) {
        return true;
    }
    return false;
}

zst_result_t
zst_queue_push(zst_queue_t* q, zst_buffer_t* buf, uint32_t timeout_ms)
{
    if (!q || !buf) return ZST_ERROR;

    zst_queue_slot_t* slot;
    uint64_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);

    struct timespec deadline;
    bool has_deadline = false;
    if (timeout_ms != UINT32_MAX && timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        has_deadline = true;
    }

    while (true) {
        slot = &q->ring[RING_MASK(q, pos)];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            /* Slot is free; check other capacity limits if configured */
            if (q->has_extra_limits && zst_queue_is_full(q)) {
                if (q->cfg.mode == ZST_QUEUE_ASYNC) {
                    return ZST_ERROR;
                }
                
                if (timeout_ms == 0) return ZST_TIMEOUT;

                atomic_fetch_add_explicit(&q->waiters_push, 1, memory_order_relaxed);
                pthread_mutex_lock(&q->lock);
                if (q->has_extra_limits && zst_queue_is_full(q)) {
                    if (has_deadline) {
                        if (pthread_cond_timedwait(&q->not_full, &q->lock, &deadline) == ETIMEDOUT) {
                            pthread_mutex_unlock(&q->lock);
                            atomic_fetch_sub_explicit(&q->waiters_push, 1, memory_order_relaxed);
                            return ZST_TIMEOUT;
                        }
                    } else {
                        pthread_cond_wait(&q->not_full, &q->lock);
                    }
                }
                pthread_mutex_unlock(&q->lock);
                atomic_fetch_sub_explicit(&q->waiters_push, 1, memory_order_relaxed);

                pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
                continue;
            }

            /* Slot is free; attempt to claim the tail position */
            if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1, 
                                                       memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (diff < 0) {
            /* Queue capacity limit encountered */
            if (q->cfg.mode == ZST_QUEUE_ASYNC) {
                return ZST_ERROR; /* Instantly drop the element under async execution rules */
            }
            
            if (timeout_ms == 0) return ZST_TIMEOUT;

            atomic_fetch_add_explicit(&q->waiters_push, 1, memory_order_relaxed);
            pthread_mutex_lock(&q->lock);
            seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
            if ((int64_t)seq - (int64_t)pos < 0) {
                if (has_deadline) {
                    if (pthread_cond_timedwait(&q->not_full, &q->lock, &deadline) == ETIMEDOUT) {
                        pthread_mutex_unlock(&q->lock);
                        atomic_fetch_sub_explicit(&q->waiters_push, 1, memory_order_relaxed);
                        return ZST_TIMEOUT;
                    }
                } else {
                    pthread_cond_wait(&q->not_full, &q->lock);
                }
            }
            pthread_mutex_unlock(&q->lock);
            atomic_fetch_sub_explicit(&q->waiters_push, 1, memory_order_relaxed);

            pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        } else {
            pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        }
    }

    atomic_store_explicit(&slot->pts, buf->pts, memory_order_relaxed);
    atomic_store_explicit(&slot->duration, buf->duration, memory_order_relaxed);
    atomic_store_explicit(&slot->buf, zst_buffer_ref(buf), memory_order_relaxed);
    atomic_fetch_add_explicit(&q->approx_bytes, buf->memory.size, memory_order_relaxed);
    
    /* Release memory visibility to the tracking consumer threads */
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);

    if (atomic_load_explicit(&q->waiters_pop, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&q->lock);
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->lock);
    }

    return ZST_OK;
}

zst_result_t
zst_queue_pop(zst_queue_t* q, zst_buffer_t** out, uint32_t timeout_ms)
{
    if (!q || !out) return ZST_ERROR;

    zst_queue_slot_t* slot;
    uint64_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);

    struct timespec deadline;
    bool has_deadline = false;
    if (timeout_ms != UINT32_MAX && timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        has_deadline = true;
    }

    while (true) {
        slot = &q->ring[RING_MASK(q, pos)];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(pos + 1);

        if (diff == 0) {
            /* Slot contains valid data; attempt to claim the head index */
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1, 
                                                       memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (diff < 0) {
            /* Queue depletion encountered */
            if (timeout_ms == 0) return ZST_TIMEOUT;

            atomic_fetch_add_explicit(&q->waiters_pop, 1, memory_order_relaxed);
            pthread_mutex_lock(&q->lock);
            seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
            if ((int64_t)seq - (int64_t)(pos + 1) < 0) {
                if (has_deadline) {
                    if (pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline) == ETIMEDOUT) {
                        pthread_mutex_unlock(&q->lock);
                        atomic_fetch_sub_explicit(&q->waiters_pop, 1, memory_order_relaxed);
                        return ZST_TIMEOUT;
                    }
                } else {
                    pthread_cond_wait(&q->not_empty, &q->lock);
                }
            }
            pthread_mutex_unlock(&q->lock);
            atomic_fetch_sub_explicit(&q->waiters_pop, 1, memory_order_relaxed);

            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        } else {
            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }

    zst_buffer_t* buf = atomic_load_explicit(&slot->buf, memory_order_relaxed);
    *out = buf;
    
    if (buf) {
        atomic_fetch_sub_explicit(&q->approx_bytes, buf->memory.size, memory_order_relaxed);
    }

    /* Reset slot metadata */
    atomic_store_explicit(&slot->pts, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->duration, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->buf, NULL, memory_order_relaxed);

    /* Release the slot structure to the tracking producer loop */
    atomic_store_explicit(&slot->sequence, pos + q->capacity, memory_order_release);

    if (atomic_load_explicit(&q->waiters_push, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&q->lock);
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->lock);
    }

    return ZST_OK;
}

uint32_t
zst_queue_size(zst_queue_t* q)
{
    if (!q) return 0;
    int64_t head = (int64_t)atomic_load_explicit(&q->head, memory_order_relaxed);
    int64_t tail = (int64_t)atomic_load_explicit(&q->tail, memory_order_relaxed);
    int64_t size = tail - head;
    return size < 0 ? 0 : (uint32_t)size;
}

void
zst_queue_flush(zst_queue_t* q)
{
    if (!q) return;
    zst_buffer_t* buf;
    /* Consume all remaining pipeline data */
    while (zst_queue_pop(q, &buf, 0) == ZST_OK) {
        if (buf) zst_buffer_unref(buf);
    }
}