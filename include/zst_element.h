/*=============================================================================
    zst_element.h - Cache-aligned structure-splitting & isolated state lock
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_stream.h"
#include "zst_pad.h"
#include "zst_segment.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_STATE_NULL,
    ZST_STATE_READY,
    ZST_STATE_PAUSED,
    ZST_STATE_PLAYING
} zst_state_t;

typedef struct {
    const char* name;

    zst_result_t (*open)(zst_element_t* el);
    zst_result_t (*close)(zst_element_t* el);
    zst_result_t (*start)(zst_element_t* el);
    zst_result_t (*stop)(zst_element_t* el);
    zst_result_t (*preroll)(zst_element_t* el);

    zst_result_t (*process)(
        zst_element_t* el,
        zst_buffer_t* in,
        zst_buffer_t** out);

    zst_caps_t* (*get_caps)(
        zst_element_t* el,
        zst_pad_t* pad,
        const zst_caps_t* filter);

    zst_clock_t* (*provide_clock)(zst_element_t* el);

    zst_result_t (*set_property)(
        zst_element_t* el,
        const char* name,
        const char* value);

    zst_result_t (*get_property)(
        zst_element_t* el,
        const char* name,
        char* value_out,
        size_t max_len);

    zst_buffer_pool_t* (*get_pool)(zst_element_t* el);

    zst_result_t (*event)(
        zst_element_t* el,
        zst_pad_t* sink_pad,
        zst_pad_event_t* event);

    uint32_t (*get_stream_count)(zst_element_t* el);

    zst_result_t (*get_stream_info)(
        zst_element_t* el,
        uint32_t index,
        zst_stream_info_t* info_out);

    zst_pad_t* (*get_stream_pad)(
        zst_element_t* el,
        zst_stream_id_t stream_id);
} zst_element_ops_t;

struct zst_element {
    /* ==========================================
       1. HOT PATH EXECUTION BLOCK (First Cache Lines)
       ========================================== */
    alignas(ZST_CACHE_LINE_SIZE) const zst_element_ops_t* ops;
    _Atomic(zst_state_t) state;

    zst_pad_t** src_pads;
    uint32_t nb_src_pads;

    zst_pad_t** sink_pads;
    uint32_t nb_sink_pads;

    zst_stream_info_t* stream_infos;
    zst_pad_t** stream_pads;
    uint32_t nb_streams;

    /* Fine-grained clock tracking used during hot push schedules */
    zst_time_t clock_sync_last_pts;
    zst_time_t clock_sync_last_clock;

    _Atomic(bool) is_queued;
    _Atomic(uint32_t) sched_task_refs;
    zst_buffer_t* sched_token;

    /* Downstream graph rank representation for localized dependency sorting */
    uint32_t graph_rank;

    /* ==========================================
       2. COLD SETUP & PARAMETER BLOCK
       ========================================== */
    alignas(ZST_CACHE_LINE_SIZE) void* priv;
    zst_bus_t* bus;
    zst_plugin_t* plugin;
    const zst_element_desc_t* desc;
    zst_clock_t* clock;
    zst_pipeline_t* pipeline;

    /* Last pool snapshot seen by topology-aware sizing.  This lets the
     * pipeline detect lazily-created/recreated pools without re-traversing the
     * whole graph on every buffer. */
    _Atomic(zst_buffer_pool_t*) pool_sizing_seen_pool;
    _Atomic(uint32_t) pool_sizing_seen_min_buffers;
    _Atomic(uint32_t) pool_sizing_seen_max_buffers;
    _Atomic(size_t) pool_sizing_seen_buffer_size;
    _Atomic(uint32_t) pool_sizing_seen_buffer_type;

    /* Isolated fine-grained mutex for state adjustments & property evaluations */
    pthread_mutex_t state_lock;
};

zst_element_t* zst_element_create(const zst_element_ops_t* ops, void* priv);
void zst_element_destroy(zst_element_t* el);

zst_result_t zst_element_set_state(zst_element_t* el, zst_state_t state);

zst_pad_t* zst_element_get_pad(zst_element_t* el, const char* name);
zst_result_t zst_element_add_pad(zst_element_t* el, zst_pad_t* pad);
zst_result_t zst_element_remove_pad(zst_element_t* el, zst_pad_t* pad);

zst_result_t zst_element_add_dynamic_pad(
    zst_element_t* el,
    zst_pad_t* pad,
    const zst_stream_info_t* stream_info);

zst_result_t zst_element_remove_dynamic_pad(
    zst_element_t* el,
    zst_pad_t* pad);

zst_result_t zst_element_snapshot_src_pads(
    zst_element_t* el,
    zst_pad_t*** pads_out,
    uint32_t* count_out);

zst_result_t zst_element_snapshot_sink_pads(
    zst_element_t* el,
    zst_pad_t*** pads_out,
    uint32_t* count_out);

void zst_element_pad_snapshot_free(
    zst_pad_t** pads,
    uint32_t count);

void zst_element_set_clock(zst_element_t* el, zst_clock_t* clock);

zst_result_t zst_element_set_property(
    zst_element_t* el,
    const char* name,
    const char* value);

zst_result_t zst_element_get_property(
    zst_element_t* el,
    const char* name,
    char* value_out,
    size_t max_len);

/* Helper property wrappers */
zst_result_t zst_element_set_property_string(zst_element_t* el, const char* name, const char* value);
zst_result_t zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value);
zst_result_t zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value);
zst_result_t zst_element_set_property_double(zst_element_t* el, const char* name, double value);
zst_result_t zst_element_set_property_bool(zst_element_t* el, const char* name, bool value);

zst_result_t zst_element_get_property_string(zst_element_t* el, const char* name, char* value_out, size_t max_len);
zst_result_t zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out);
zst_result_t zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out);
zst_result_t zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out);
zst_result_t zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out);

zst_buffer_pool_t* zst_element_get_pool(zst_element_t* el);
zst_result_t zst_element_seek(zst_element_t* el, double rate, const zst_segment_t* segment);

uint32_t zst_element_get_stream_count(zst_element_t* el);

zst_result_t zst_element_get_stream_info(
    zst_element_t* el,
    uint32_t index,
    zst_stream_info_t* info_out);

zst_pad_t* zst_element_get_stream_pad(
    zst_element_t* el,
    zst_stream_id_t stream_id);

#ifdef __cplusplus
}
#endif