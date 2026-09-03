/*=============================================================================
    zst_types.h — Base types, result codes, struct forward decls

    @brief  Fundamental types and error codes for the zstreamer framework.
    @details
    This header defines:
    - zst_result_t — error/status codes for all framework functions
    - zst_state_t — element state machine enum (NULL→READY→PLAYING)
    - zst_time_t — nanosecond timestamp type used for PTS/DTS/clock
    - zst_media_kind_t — stream classification (video/audio/text/unknown)
    - Forward declarations for all major framework types (buffers, pads,
      elements, pipelines, queues, schedulers, buses, clocks, etc.)
    - Pad direction and presence template types
    - Buffer pool sizing configuration
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <atomic>
#ifndef _Atomic
#define _Atomic(X) std::atomic<X>
#endif
#ifndef _Alignas
#define _Alignas(X) alignas(X)
#endif
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub_explicit;
using std::atomic_load_explicit;
using std::atomic_store_explicit;
using std::memory_order_relaxed;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_acq_rel;
#else
#include <stdatomic.h>
#include <stdalign.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  zst_result_t;
typedef uint64_t zst_time_t;

#define ZST_OK          0
#define ZST_ERROR      -1
#define ZST_TIMEOUT    -2
#define ZST_AGAIN      -3
#define ZST_EOF        -4
#define ZST_ERROR_INVALID_ARGUMENT -5
#define ZST_ERROR_NOT_IMPLEMENTED  -6

typedef struct zst_allocator   zst_allocator_t;
typedef struct zst_clock       zst_clock_t;
typedef struct zst_buffer      zst_buffer_t;
typedef struct zst_buffer_pool zst_buffer_pool_t;
typedef struct zst_queue       zst_queue_t;
typedef struct zst_pad         zst_pad_t;
typedef struct zst_element      zst_element_t;
typedef struct zst_element_desc zst_element_desc_t;
typedef struct zst_pipeline     zst_pipeline_t;
typedef struct zst_scheduler   zst_scheduler_t;
typedef struct zst_plugin      zst_plugin_t;
typedef struct zst_bus         zst_bus_t;
typedef struct zst_event       zst_event_t;
typedef struct zst_stream_info zst_stream_info_t;
typedef struct zst_bin         zst_bin_t;
typedef struct zst_ghost_pad   zst_ghost_pad_t;
typedef struct zst_pad_probe   zst_pad_probe_t;
typedef struct zst_segment     zst_segment_t;

#ifdef __cplusplus
}
#endif
