/*=============================================================================
    zst_buffer.h - Thread-safe, lock-free refcounted buffers with alignment
=============================================================================*/
#pragma once

#include "zst_types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_CACHE_LINE_SIZE 64
#define ZST_MEM_ALIGNMENT   32  /* Required alignment for SIMD (AVX2/AVX-512) */

typedef enum {
    ZST_BUFFER_VIDEO_FRAME,
    ZST_BUFFER_AUDIO_FRAME,
    ZST_BUFFER_VIDEO_PACKET,
    ZST_BUFFER_AUDIO_PACKET,
    ZST_BUFFER_USER = 0x10000
} zst_buffer_type_t;

typedef enum {
    ZST_MEMORY_CPU,
    ZST_MEMORY_DMABUF,
    ZST_MEMORY_CUDA,
    ZST_MEMORY_VULKAN,
    ZST_MEMORY_ONEAPI
} zst_memory_type_t;

#define ZST_BUFFER_FLAG_EOS      (1 << 0)
#define ZST_BUFFER_FLAG_DROP     (1 << 1)
#define ZST_BUFFER_FLAG_KEYFRAME (1 << 2)

typedef struct {
    zst_memory_type_t type;
    void* data;
    size_t size;
    void* priv;
    void (*release)(void* priv);
} zst_memory_t;

/* Aligned for optimal cache line utilization and SIMD instruction loads using standard C11 alignment */
typedef struct {
    _Alignas(ZST_CACHE_LINE_SIZE) uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride[4];
    uint8_t* plane[4];
} zst_video_frame_t;

typedef struct {
    _Alignas(ZST_CACHE_LINE_SIZE) uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t nb_samples;
    void* data;
} zst_audio_frame_t;

struct zst_buffer {
    /* Hot path variables packed and aligned to avoid false sharing */
    _Alignas(ZST_CACHE_LINE_SIZE) _Atomic int refcount;
    uint32_t type;
    uint32_t flags;

    zst_time_t pts;
    zst_time_t dts;
    zst_time_t duration;

    zst_memory_t memory;
    void* payload;
    void* metadata;

    struct zst_buffer_pool* pool;
    void (*destroy)(zst_buffer_t* buf);
};

zst_buffer_t* zst_buffer_create(uint32_t type);

zst_buffer_t* zst_buffer_create_with_allocator(
    uint32_t type,
    zst_allocator_t* allocator,
    size_t size);

zst_buffer_t* zst_buffer_create_with_pool(
    struct zst_buffer_pool* pool);

/* High-performance inline reference counting to avoid function call overhead in hot loops */
static inline zst_buffer_t* zst_buffer_ref(zst_buffer_t* buf) {
    if (buf) {
        atomic_fetch_add_explicit(&buf->refcount, 1, memory_order_relaxed);
    }
    return buf;
}

void zst_buffer_unref(zst_buffer_t* buf);

#ifdef __cplusplus
}
#endif