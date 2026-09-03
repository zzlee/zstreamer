/*=============================================================================
    zst_pipeline.h - Incremental rank-based topological sorting
=============================================================================*/
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdalign.h>
#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zst_pipeline {
    _Alignas(ZST_CACHE_LINE_SIZE) zst_element_t** elements;
    uint32_t nb_elements;
    uint32_t capacity;

    _Atomic(zst_state_t) state;

    void* priv;
    zst_bus_t* bus;
    zst_clock_t* clock;

    int clock_sync;
    zst_time_t base_time;
    _Atomic(uint64_t) graph_version;

    /* Lock protecting structural topological modifications of the graph */
    pthread_rwlock_t elements_lock;

    /* Tracks explicit graph-reconfiguration transactions so dynamic helpers can
     * be used safely either inside begin/end or as self-contained operations. */
    _Atomic(bool) reconfiguration_active;
    pthread_t reconfiguration_owner;

    /* Tracks state transitions in progress so elements know elements_lock is held */
    _Atomic(bool) state_transition_active;

    /* Buffer-pool sizing is topology-dependent but expensive (graph walk).
     * Mark dirty on topology changes and coalesce updates to avoid O(N)
     * traversal in per-frame hot paths. */
    _Atomic(bool) buffer_pool_sizing_dirty;
    pthread_mutex_t buffer_pool_sizing_lock;
};

zst_pipeline_t* zst_pipeline_create(void);
void zst_pipeline_destroy(zst_pipeline_t* pipe);

zst_bus_t* zst_pipeline_get_bus(zst_pipeline_t* pipe);
void zst_pipeline_set_clock(zst_pipeline_t* pipe, zst_clock_t* clock);
zst_clock_t* zst_pipeline_get_clock(zst_pipeline_t* pipe);

zst_result_t zst_pipeline_add(zst_pipeline_t* pipe, zst_element_t* el);
zst_result_t zst_pipeline_remove(zst_pipeline_t* pipe, zst_element_t* el);

zst_result_t zst_pipeline_set_state(zst_pipeline_t* pipe, zst_state_t state);

zst_result_t zst_pipeline_start(zst_pipeline_t* pipe);
zst_result_t zst_pipeline_stop(zst_pipeline_t* pipe);

zst_result_t zst_pipeline_set_clock_sync(zst_pipeline_t* pipe, int enabled);
int zst_pipeline_get_clock_sync(zst_pipeline_t* pipe);

/* Incremental rank tracking - recalulates only downstream ranks of targeted node */
void zst_pipeline_update_ranks_from(zst_pipeline_t* pipe, zst_element_t* start_el);
void zst_pipeline_topological_sort(zst_pipeline_t* pipe);

int zst_pipeline_count_elements_of_type(
    zst_pipeline_t* pipe,
    const char* type_name);

void zst_pipeline_foreach_element(
    zst_pipeline_t* pipe,
    void (*func)(zst_element_t*, void*),
    void* user_data);

void zst_pipeline_mark_buffer_pool_sizing_dirty(
    zst_pipeline_t* pipe);

void zst_pipeline_update_buffer_pool_sizing(
    zst_pipeline_t* pipe);

void zst_pipeline_update_buffer_pool_sizing_if_needed(
    zst_pipeline_t* pipe,
    zst_element_t* changed_el);

zst_result_t zst_pipeline_reconfigure_begin(zst_pipeline_t* pipe);
zst_result_t zst_pipeline_reconfigure_end(zst_pipeline_t* pipe);

zst_result_t zst_pipeline_add_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el);
zst_result_t zst_pipeline_remove_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el);

zst_result_t zst_pipeline_link_pads_dynamic(
    zst_pipeline_t* pipe,
    zst_pad_t* src,
    zst_pad_t* sink);

zst_result_t zst_pipeline_unlink_pads_dynamic(
    zst_pipeline_t* pipe,
    zst_pad_t* src,
    zst_pad_t* sink);

#ifdef __cplusplus
}
#endif
