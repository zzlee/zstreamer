/*=============================================================================
    zst_pipeline.c - Element container with state propagation
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* clock_gettime */

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_buffer_pool.h"
#include "zst_element_factory.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

zst_pipeline_t*
zst_pipeline_create(void)
{
    zst_pipeline_t* pipe = calloc(1, sizeof(*pipe));
    if (!pipe) return NULL;

    pipe->elements    = NULL;
    pipe->nb_elements = 0;
    pipe->capacity    = 0;
    pipe->state       = ZST_STATE_NULL;
    pipe->priv        = NULL;
    pipe->bus         = zst_bus_create();
    pipe->clock       = NULL;
    atomic_store_explicit(&pipe->graph_version, 1, memory_order_relaxed);
    atomic_store_explicit(&pipe->buffer_pool_sizing_dirty, true, memory_order_relaxed);
    atomic_store_explicit(&pipe->reconfiguration_active, false, memory_order_relaxed);
    atomic_store_explicit(&pipe->state_transition_active, false, memory_order_relaxed);
    memset(&pipe->reconfiguration_owner, 0, sizeof(pipe->reconfiguration_owner));
    pthread_rwlock_init(&pipe->elements_lock, NULL);
    pthread_mutex_init(&pipe->buffer_pool_sizing_lock, NULL);

    return pipe;
}

void
zst_pipeline_destroy(zst_pipeline_t* pipe)
{
    if (!pipe) return;

    /* Destroy all elements (in reverse order to safeguard downstream elements) */
    for (uint32_t i = pipe->nb_elements; i > 0; i--) {
        zst_element_destroy(pipe->elements[i - 1]);
    }

    if (pipe->bus) {
        zst_bus_destroy(pipe->bus);
    }

    if (pipe->clock) {
        zst_clock_unref(pipe->clock);
    }

    pthread_mutex_destroy(&pipe->buffer_pool_sizing_lock);
    pthread_rwlock_destroy(&pipe->elements_lock);

    free(pipe->elements);
    free(pipe);
}

zst_bus_t*
zst_pipeline_get_bus(zst_pipeline_t* pipe)
{
    return pipe ? pipe->bus : NULL;
}

void
zst_pipeline_set_clock(zst_pipeline_t* pipe, zst_clock_t* clock)
{
    if (!pipe) return;
    if (pipe->clock == clock) return;
    if (pipe->clock) {
        zst_clock_unref(pipe->clock);
    }
    pipe->clock = clock ? zst_clock_ref(clock) : NULL;

    /* Propagate to all elements */
    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_set_clock(pipe->elements[i], pipe->clock);
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
}

zst_clock_t*
zst_pipeline_get_clock(zst_pipeline_t* pipe)
{
    return pipe ? pipe->clock : NULL;
}

static zst_result_t
pipeline_add_locked(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    /* Amortized exponential dynamic resizing */
    if (pipe->nb_elements >= pipe->capacity) {
        uint32_t new_cap = (pipe->capacity == 0) ? 8 : pipe->capacity * 2;
        zst_element_t** els = realloc(pipe->elements, new_cap * sizeof(zst_element_t*));
        if (!els) {
            return ZST_ERROR;
        }
        pipe->elements = els;
        pipe->capacity = new_cap;
    }

    pipe->elements[pipe->nb_elements++] = el;
    el->bus = pipe->bus;
    el->pipeline = pipe;
    if (el->sched_token) {
        el->sched_token->memory.priv = el;
    }

    zst_element_set_clock(el, pipe->clock);
    zst_pipeline_mark_buffer_pool_sizing_dirty(pipe);
    return ZST_OK;
}

static zst_result_t
pipeline_remove_locked(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    int found = 0;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            /* Shift remaining elements down */
            for (uint32_t j = i; j < pipe->nb_elements - 1; j++) {
                pipe->elements[j] = pipe->elements[j + 1];
            }
            pipe->nb_elements--;
            found = 1;
            break;
        }
    }

    if (found) {
        el->bus = NULL;
        el->pipeline = NULL;
        if (el->sched_token) {
            el->sched_token->memory.priv = NULL;
        }
        zst_pipeline_mark_buffer_pool_sizing_dirty(pipe);
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_result_t
zst_pipeline_add(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;
    pthread_rwlock_wrlock(&pipe->elements_lock);
    zst_result_t r = pipeline_add_locked(pipe, el);
    pthread_rwlock_unlock(&pipe->elements_lock);
    return r;
}

zst_result_t
zst_pipeline_remove(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;
    pthread_rwlock_wrlock(&pipe->elements_lock);
    zst_result_t r = pipeline_remove_locked(pipe, el);
    pthread_rwlock_unlock(&pipe->elements_lock);
    return r;
}

static int
pipeline_element_index(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) return (int)i;
    }
    return -1;
}

static const char*
pipeline_element_type_name(zst_element_t* el)
{
    if (!el) return NULL;
    if (el->desc && el->desc->name) return el->desc->name;
    if (el->ops && el->ops->name) return el->ops->name;
    return NULL;
}

static int
pipeline_element_is_type(zst_element_t* el, const char* type_name)
{
    const char* name = pipeline_element_type_name(el);
    return name && type_name && strcmp(name, type_name) == 0;
}

static int
pipeline_count_downstream_type_dfs(zst_pipeline_t* pipe, zst_element_t* el,
                                   const char* type_name, int* visited)
{
    int count = 0;
    if (!pipe || !el || !type_name || !visited) return 0;

    zst_pad_t** src_pads = NULL;
    uint32_t nb_src_pads = 0;
    if (zst_element_snapshot_src_pads(el, &src_pads, &nb_src_pads) != ZST_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < nb_src_pads; i++) {
        zst_pad_t* peer = zst_pad_get_peer(src_pads[i]);
        zst_element_t* child = peer ? peer->parent : NULL;
        if (peer) zst_pad_unref(peer);
        int idx = pipeline_element_index(pipe, child);
        if (idx < 0 || visited[idx]) continue;
        visited[idx] = 1;

        if (pipeline_element_is_type(child, type_name)) count++;
        count += pipeline_count_downstream_type_dfs(pipe, child, type_name, visited);
    }
    zst_element_pad_snapshot_free(src_pads, nb_src_pads);

    return count;
}

static int
zst_pipeline_count_downstream_elements_of_type(zst_pipeline_t* pipe,
                                               zst_element_t* start,
                                               const char* type_name)
{
    if (!pipe || !start || !type_name) return 0;
    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) return 0;
    int start_idx = pipeline_element_index(pipe, start);
    if (start_idx >= 0) visited[start_idx] = 1;
    int count = pipeline_count_downstream_type_dfs(pipe, start, type_name, visited);
    free(visited);
    return count;
}

static void
pool_config_default_size_for_queue_count(zst_buffer_pool_config_t* config, int n_queues)
{
    if (!config || n_queues <= 0) return;
    if (config->min_buffers < (uint32_t)(n_queues + 2)) {
        config->min_buffers = n_queues + 2;
        if (config->max_buffers < config->min_buffers) {
            config->max_buffers = config->min_buffers * 2;
        }
    }
}

static void
pipeline_cache_pool_snapshot(zst_element_t* el, zst_buffer_pool_t* pool)
{
    if (!el) return;

    atomic_store_explicit(&el->pool_sizing_seen_pool, pool, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_generation,
                          pool ? zst_buffer_pool_get_generation(pool) : 0,
                          memory_order_release);
    if (!pool) {
        atomic_store_explicit(&el->pool_sizing_seen_min_buffers, 0, memory_order_release);
        atomic_store_explicit(&el->pool_sizing_seen_max_buffers, 0, memory_order_release);
        atomic_store_explicit(&el->pool_sizing_seen_buffer_size, 0, memory_order_release);
        atomic_store_explicit(&el->pool_sizing_seen_buffer_type, 0, memory_order_release);
        return;
    }

    zst_buffer_pool_config_t cfg = zst_buffer_pool_get_config(pool);
    atomic_store_explicit(&el->pool_sizing_seen_min_buffers, cfg.min_buffers, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_max_buffers, cfg.max_buffers, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_buffer_size, cfg.buffer_size, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_buffer_type, cfg.buffer_type, memory_order_release);
}

static int
pipeline_element_pool_snapshot_changed(zst_element_t* el)
{
    if (!el || !el->ops || !el->ops->get_pool) return 0;

    zst_buffer_pool_t* pool = zst_element_get_pool(el);
    zst_buffer_pool_t* seen_pool = atomic_load_explicit(&el->pool_sizing_seen_pool, memory_order_acquire);
    if (!pool) {
        return seen_pool != NULL;
    }

    if (pool != seen_pool) {
        return 1;
    }

    return zst_buffer_pool_get_generation(pool) !=
           atomic_load_explicit(&el->pool_sizing_seen_generation, memory_order_acquire);
}

static void
apply_pool_config_cb(zst_element_t* el, void* user_data)
{
    zst_pipeline_t* pipe = user_data;
    zst_buffer_pool_t* pool = zst_element_get_pool(el);
    if (!pool) {
        pipeline_cache_pool_snapshot(el, NULL);
        return;
    }

    zst_buffer_pool_config_t old_config = zst_buffer_pool_get_config(pool);
    zst_buffer_pool_config_t new_config = old_config;
    int n_queues = zst_pipeline_count_downstream_elements_of_type(pipe, el, "queue");
    pool_config_default_size_for_queue_count(&new_config, n_queues);
    if (new_config.min_buffers != old_config.min_buffers ||
        new_config.max_buffers != old_config.max_buffers) {
        if (zst_buffer_pool_set_config(pool, &new_config) == ZST_OK) {
            old_config = new_config;
        } else {
            old_config = zst_buffer_pool_get_config(pool);
        }
    }

    atomic_store_explicit(&el->pool_sizing_seen_pool, pool, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_generation,
                          zst_buffer_pool_get_generation(pool),
                          memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_min_buffers, old_config.min_buffers, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_max_buffers, old_config.max_buffers, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_buffer_size, old_config.buffer_size, memory_order_release);
    atomic_store_explicit(&el->pool_sizing_seen_buffer_type, old_config.buffer_type, memory_order_release);
}

void
zst_pipeline_mark_buffer_pool_sizing_dirty(zst_pipeline_t* pipe)
{
    if (!pipe) return;
    atomic_fetch_add_explicit(&pipe->graph_version, 1, memory_order_acq_rel);
    atomic_store_explicit(&pipe->buffer_pool_sizing_dirty, true, memory_order_release);
}

void
zst_pipeline_update_buffer_pool_sizing(zst_pipeline_t* pipe)
{
    if (!pipe) return;

    pthread_mutex_lock(&pipe->buffer_pool_sizing_lock);
    atomic_store_explicit(&pipe->buffer_pool_sizing_dirty, false, memory_order_release);
    zst_pipeline_foreach_element(pipe, apply_pool_config_cb, pipe);
    pthread_mutex_unlock(&pipe->buffer_pool_sizing_lock);
}

void
zst_pipeline_update_buffer_pool_sizing_if_needed(zst_pipeline_t* pipe,
                                                 zst_element_t* changed_el)
{
    if (!pipe) return;

    int dirty = atomic_load_explicit(&pipe->buffer_pool_sizing_dirty, memory_order_acquire);
    int pool_changed = pipeline_element_pool_snapshot_changed(changed_el);
    if (!dirty && !pool_changed) return;

    pthread_mutex_lock(&pipe->buffer_pool_sizing_lock);
    dirty = atomic_load_explicit(&pipe->buffer_pool_sizing_dirty, memory_order_acquire);
    pool_changed = pipeline_element_pool_snapshot_changed(changed_el);
    if (dirty || pool_changed) {
        atomic_store_explicit(&pipe->buffer_pool_sizing_dirty, false, memory_order_release);
        zst_pipeline_foreach_element(pipe, apply_pool_config_cb, pipe);
    }
    pthread_mutex_unlock(&pipe->buffer_pool_sizing_lock);
}

zst_result_t
zst_pipeline_set_state(zst_pipeline_t* pipe, zst_state_t state)
{
    if (!pipe) return ZST_ERROR;

    zst_state_t old_state = pipe->state;
    if (old_state == state) return ZST_OK;

    /* Decompose multi-step transitions so they pass through all intermediate states */
    if (state > old_state && (state - old_state) > 1) {
        zst_result_t r = zst_pipeline_set_state(pipe, old_state + 1);
        if (r != ZST_OK) return r;
        r = zst_pipeline_set_state(pipe, state);
        if (r != ZST_OK) {
            zst_pipeline_set_state(pipe, old_state);
        }
        return r;
    }
    if (state < old_state && (old_state - state) > 1) {
        zst_result_t r = zst_pipeline_set_state(pipe, old_state - 1);
        if (r != ZST_OK) return r;
        r = zst_pipeline_set_state(pipe, state);
        if (r != ZST_OK) {
            zst_pipeline_set_state(pipe, old_state);
        }
        return r;
    }

    if (old_state < ZST_STATE_PLAYING && state == ZST_STATE_PLAYING) {
        zst_pipeline_update_buffer_pool_sizing(pipe);
    }

    /* Transition to PLAYING: Auto-select clock if none exists */
    if (old_state < ZST_STATE_PLAYING && state == ZST_STATE_PLAYING && !pipe->clock) {
        zst_clock_t* master_clock = NULL;
        pthread_rwlock_rdlock(&pipe->elements_lock);
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            zst_element_t* el = pipe->elements[i];
            if (el->ops && el->ops->provide_clock) {
                master_clock = el->ops->provide_clock(el);
                if (master_clock) break;
            }
        }
        pthread_rwlock_unlock(&pipe->elements_lock);

        zst_clock_t* sys_clock = zst_clock_system_create();
        if (master_clock && sys_clock) {
            zst_clock_t* slave = zst_clock_slave_create(master_clock, sys_clock);
            if (slave) {
                zst_pipeline_set_clock(pipe, slave);
                zst_clock_unref(slave);
            } else {
                zst_pipeline_set_clock(pipe, sys_clock);
            }
        } else if (sys_clock) {
            zst_pipeline_set_clock(pipe, sys_clock);
        }

        if (master_clock) zst_clock_unref(master_clock);
        if (sys_clock) zst_clock_unref(sys_clock);

        /* Record pipeline base_time: wall-clock snapshot at PLAYING entry.
         * The scheduler uses (now - base_time) as pipeline running-time and
         * compares it against each buffer's PTS for real-time pacing. */
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        pipe->base_time = (zst_time_t)_ts.tv_sec * 1000000000ULL
                        + (zst_time_t)_ts.tv_nsec;
    }

    /* Propagate state to all elements with direction awareness:
     * - Upward transitions: Propagate forward (sources first, then sinks)
     * - Downward transitions: Propagate in reverse (sinks first, then sources) */
    atomic_store_explicit(&pipe->state_transition_active, true, memory_order_release);
    pthread_rwlock_rdlock(&pipe->elements_lock);

    if (state > old_state) {
        /* Upward Transition: Forward order */
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            zst_result_t r = zst_element_set_state(pipe->elements[i], state);
            if (r != ZST_OK) {
                /* Rollback transitioned elements to previous state */
                for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                    zst_element_set_state(pipe->elements[j], old_state);
                }
                pthread_rwlock_unlock(&pipe->elements_lock);
                atomic_store_explicit(&pipe->state_transition_active, false, memory_order_release);
                if (pipe->bus) {
                    zst_event_t* ev = zst_event_new_error(pipe->elements[i], r, "Element failed upward state transition");
                    zst_bus_post(pipe->bus, ev);
                }
                return r;
            }
        }
    } else {
        /* Downward Transition: Reverse order to stop sinks first */
        for (int32_t i = (int32_t)pipe->nb_elements - 1; i >= 0; i--) {
            zst_result_t r = zst_element_set_state(pipe->elements[i], state);
            if (r != ZST_OK) {
                /* Rollback transitioned elements to previous state */
                for (uint32_t j = (uint32_t)i + 1; j < pipe->nb_elements; j++) {
                    zst_element_set_state(pipe->elements[j], old_state);
                }
                pthread_rwlock_unlock(&pipe->elements_lock);
                atomic_store_explicit(&pipe->state_transition_active, false, memory_order_release);
                if (pipe->bus) {
                    zst_event_t* ev = zst_event_new_error(pipe->elements[i], r, "Element failed downward state transition");
                    zst_bus_post(pipe->bus, ev);
                }
                return r;
            }
        }
    }

    pthread_rwlock_unlock(&pipe->elements_lock);
    atomic_store_explicit(&pipe->state_transition_active, false, memory_order_release);

    pipe->state = state;
    if (pipe->bus) {
        zst_event_t* ev = zst_event_new_state_changed(NULL, old_state, state);
        zst_bus_post(pipe->bus, ev);
    }
    return ZST_OK;
}

zst_result_t
zst_pipeline_start(zst_pipeline_t* pipe)
{
    return zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
}

zst_result_t
zst_pipeline_stop(zst_pipeline_t* pipe)
{
    return zst_pipeline_set_state(pipe, ZST_STATE_NULL);
}

static void
dfs_sort(zst_element_t* el, zst_element_t** temp, uint32_t* temp_idx, int* visited, zst_pipeline_t* pipe)
{
    int idx = -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1 || visited[idx]) return;

    visited[idx] = 1;

    zst_pad_t** src_pads = NULL;
    uint32_t nb_src_pads = 0;
    if (zst_element_snapshot_src_pads(el, &src_pads, &nb_src_pads) == ZST_OK) {
        for (uint32_t i = 0; i < nb_src_pads; i++) {
            zst_pad_t* peer = zst_pad_get_peer(src_pads[i]);
            zst_element_t* child = peer ? peer->parent : NULL;
            if (peer) zst_pad_unref(peer);
            if (child) {
                dfs_sort(child, temp, temp_idx, visited, pipe);
            }
        }
        zst_element_pad_snapshot_free(src_pads, nb_src_pads);
    }

    temp[--(*temp_idx)] = el;
}

zst_result_t
zst_pipeline_set_clock_sync(zst_pipeline_t* pipe, int enabled)
{
    if (!pipe) return ZST_ERROR;
    pipe->clock_sync = enabled ? 1 : 0;
    return ZST_OK;
}

int
zst_pipeline_get_clock_sync(zst_pipeline_t* pipe)
{
    return pipe ? pipe->clock_sync : 0;
}


static void
update_ranks_dfs(zst_element_t* el, uint32_t current_rank, int* visited, zst_pipeline_t* pipe)
{
    int idx = -1;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i] == el) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1) return;

    if (current_rank > el->graph_rank) {
        el->graph_rank = current_rank;
    }

    if (visited[idx]) return;
    visited[idx] = 1;

    zst_pad_t** src_pads = NULL;
    uint32_t nb_src_pads = 0;
    if (zst_element_snapshot_src_pads(el, &src_pads, &nb_src_pads) == ZST_OK) {
        for (uint32_t i = 0; i < nb_src_pads; i++) {
            zst_pad_t* peer = zst_pad_get_peer(src_pads[i]);
            zst_element_t* child = peer ? peer->parent : NULL;
            if (peer) zst_pad_unref(peer);
            if (child) {
                update_ranks_dfs(child, el->graph_rank + 1, visited, pipe);
            }
        }
        zst_element_pad_snapshot_free(src_pads, nb_src_pads);
    }
    visited[idx] = 0;
}

void
zst_pipeline_update_ranks_from(zst_pipeline_t* pipe, zst_element_t* start_el)
{
    if (!pipe || !start_el) return;

    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) return;

    uint32_t start_rank = start_el->graph_rank;
    update_ranks_dfs(start_el, start_rank, visited, pipe);

    free(visited);
}

static void
pipeline_recalculate_ranks_locked(zst_pipeline_t* pipe)
{
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        pipe->elements[i]->graph_rank = 0;
    }

    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_t* el = pipe->elements[i];
        bool is_src = true;
        zst_pad_t** sink_pads = NULL;
        uint32_t nb_sink_pads = 0;
        if (zst_element_snapshot_sink_pads(el, &sink_pads, &nb_sink_pads) == ZST_OK) {
            for (uint32_t p_idx = 0; p_idx < nb_sink_pads; p_idx++) {
                if (sink_pads[p_idx] && zst_pad_is_linked(sink_pads[p_idx])) {
                    is_src = false;
                    break;
                }
            }
            zst_element_pad_snapshot_free(sink_pads, nb_sink_pads);
        }

        if (is_src) {
            zst_pipeline_update_ranks_from(pipe, el);
        }
    }
}

static void
pipeline_topological_sort_locked(zst_pipeline_t* pipe)
{
    if (!pipe || pipe->nb_elements <= 1) return;

    zst_element_t** temp = malloc(pipe->nb_elements * sizeof(zst_element_t*));
    if (!temp) {
        return;
    }

    int* visited = calloc(pipe->nb_elements, sizeof(int));
    if (!visited) {
        free(temp);
        return;
    }

    uint32_t temp_idx = pipe->nb_elements;

    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (pipe->elements[i]->nb_sink_pads == 0) {
            dfs_sort(pipe->elements[i], temp, &temp_idx, visited, pipe);
        }
    }

    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        if (!visited[i]) {
            dfs_sort(pipe->elements[i], temp, &temp_idx, visited, pipe);
        }
    }

    memcpy(pipe->elements, temp, pipe->nb_elements * sizeof(zst_element_t*));

    free(visited);
    free(temp);
}

void
zst_pipeline_topological_sort(zst_pipeline_t* pipe)
{
    if (!pipe || pipe->nb_elements <= 1) return;
    pthread_rwlock_wrlock(&pipe->elements_lock);
    pipeline_topological_sort_locked(pipe);
    pthread_rwlock_unlock(&pipe->elements_lock);
}

static int
pipeline_reconfiguration_owned_by_current_thread(zst_pipeline_t* pipe)
{
    if (!pipe) return 0;
    if (!atomic_load_explicit(&pipe->reconfiguration_active, memory_order_acquire)) {
        return 0;
    }
    return pthread_equal(pipe->reconfiguration_owner, pthread_self()) != 0;
}

static void
pipeline_wait_for_foreign_reconfiguration(zst_pipeline_t* pipe)
{
    if (!pipe) return;
    while (atomic_load_explicit(&pipe->reconfiguration_active, memory_order_acquire) &&
           !pipeline_reconfiguration_owned_by_current_thread(pipe)) {
        struct timespec req = {0, 100000};
        nanosleep(&req, NULL);
    }
}

static void
pipeline_reconfiguration_finish_locked(zst_pipeline_t* pipe)
{
    pipeline_recalculate_ranks_locked(pipe);
    pipeline_topological_sort_locked(pipe);
    zst_pipeline_mark_buffer_pool_sizing_dirty(pipe);

    pthread_rwlock_unlock(&pipe->elements_lock);

    if (pipe->priv) {
        zst_scheduler_wake((zst_scheduler_t*)pipe->priv);
    }

    zst_pipeline_update_buffer_pool_sizing(pipe);
}

static void
pipeline_wait_for_element_scheduler_tasks(zst_element_t* el)
{
    if (!el) return;
    while (atomic_load_explicit(&el->sched_task_refs, memory_order_acquire) > 0) {
        struct timespec req = {0, 100000};
        nanosleep(&req, NULL);
    }
}

zst_result_t
zst_pipeline_reconfigure_begin(zst_pipeline_t* pipe)
{
    if (!pipe) return ZST_ERROR;
    if (pipeline_reconfiguration_owned_by_current_thread(pipe)) return ZST_ERROR;
    pipeline_wait_for_foreign_reconfiguration(pipe);
    pthread_rwlock_wrlock(&pipe->elements_lock);
    pipe->reconfiguration_owner = pthread_self();
    atomic_store_explicit(&pipe->reconfiguration_active, true, memory_order_release);
    return ZST_OK;
}

zst_result_t
zst_pipeline_reconfigure_end(zst_pipeline_t* pipe)
{
    if (!pipe || !pipeline_reconfiguration_owned_by_current_thread(pipe)) return ZST_ERROR;

    atomic_store_explicit(&pipe->reconfiguration_active, false, memory_order_release);
    pipeline_reconfiguration_finish_locked(pipe);
    return ZST_OK;
}

zst_result_t
zst_pipeline_add_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    int in_transaction = pipeline_reconfiguration_owned_by_current_thread(pipe);
    if (!in_transaction) {
        pipeline_wait_for_foreign_reconfiguration(pipe);
        pthread_rwlock_wrlock(&pipe->elements_lock);
    }

    zst_result_t r = pipeline_add_locked(pipe, el);
    if (r != ZST_OK) {
        if (!in_transaction) pthread_rwlock_unlock(&pipe->elements_lock);
        return r;
    }

    zst_state_t pipe_state = pipe->state;
    if (pipe_state > ZST_STATE_NULL) {
        if (pipe_state >= ZST_STATE_READY) {
            r = zst_element_set_state(el, ZST_STATE_READY);
            if (r != ZST_OK) {
                pipeline_remove_locked(pipe, el);
                if (!in_transaction) pthread_rwlock_unlock(&pipe->elements_lock);
                return r;
            }
        }
        if (pipe_state == ZST_STATE_PLAYING) {
            r = zst_element_set_state(el, ZST_STATE_PLAYING);
            if (r != ZST_OK) {
                zst_element_set_state(el, ZST_STATE_NULL);
                pipeline_remove_locked(pipe, el);
                if (!in_transaction) pthread_rwlock_unlock(&pipe->elements_lock);
                return r;
            }
        }
    }

    if (!in_transaction) {
        pipeline_reconfiguration_finish_locked(pipe);
    }
    return ZST_OK;
}

zst_result_t
zst_pipeline_remove_element_dynamic(zst_pipeline_t* pipe, zst_element_t* el)
{
    if (!pipe || !el) return ZST_ERROR;

    int in_transaction = pipeline_reconfiguration_owned_by_current_thread(pipe);

    zst_element_set_state(el, ZST_STATE_NULL);
    if (in_transaction) {
        /* Do not wait for in-flight scheduler callbacks while holding the graph
         * write lock: a finishing callback may need a read lock for deferred
         * pool-sizing checks. Keep reconfiguration_active set so other dynamic
         * helpers wait until this transaction resumes. */
        pthread_rwlock_unlock(&pipe->elements_lock);
        pipeline_wait_for_element_scheduler_tasks(el);
        pthread_rwlock_wrlock(&pipe->elements_lock);
    } else {
        pipeline_wait_for_foreign_reconfiguration(pipe);
        pipeline_wait_for_element_scheduler_tasks(el);
        pthread_rwlock_wrlock(&pipe->elements_lock);
    }

    /* Unlink all pads using snapshots because unlinking may mutate pad arrays. */
    zst_pad_t** sink_pads = NULL;
    uint32_t nb_sink_pads = 0;
    if (zst_element_snapshot_sink_pads(el, &sink_pads, &nb_sink_pads) == ZST_OK) {
        for (uint32_t i = 0; i < nb_sink_pads; i++) {
            if (sink_pads[i]) zst_pad_unlink(sink_pads[i]);
        }
        zst_element_pad_snapshot_free(sink_pads, nb_sink_pads);
    }

    zst_pad_t** src_pads = NULL;
    uint32_t nb_src_pads = 0;
    if (zst_element_snapshot_src_pads(el, &src_pads, &nb_src_pads) == ZST_OK) {
        for (uint32_t i = 0; i < nb_src_pads; i++) {
            if (src_pads[i]) zst_pad_unlink(src_pads[i]);
        }
        zst_element_pad_snapshot_free(src_pads, nb_src_pads);
    }

    zst_result_t r = pipeline_remove_locked(pipe, el);
    if (!in_transaction) {
        pipeline_reconfiguration_finish_locked(pipe);
    }
    return r;
}

zst_result_t
zst_pipeline_link_pads_dynamic(zst_pipeline_t* pipe, zst_pad_t* src, zst_pad_t* sink)
{
    if (!pipe || !src || !sink) return ZST_ERROR;

    int in_transaction = pipeline_reconfiguration_owned_by_current_thread(pipe);
    if (!in_transaction) {
        pipeline_wait_for_foreign_reconfiguration(pipe);
        pthread_rwlock_wrlock(&pipe->elements_lock);
    }

    zst_result_t r = zst_pad_link(src, sink);
    if (r == ZST_OK) {
        if (src->parent) {
            zst_pipeline_update_ranks_from(pipe, src->parent);
        }
        zst_pipeline_mark_buffer_pool_sizing_dirty(pipe);
    }

    if (!in_transaction) {
        if (r == ZST_OK) pipeline_reconfiguration_finish_locked(pipe);
        else pthread_rwlock_unlock(&pipe->elements_lock);
    }
    return r;
}

zst_result_t
zst_pipeline_unlink_pads_dynamic(zst_pipeline_t* pipe, zst_pad_t* src, zst_pad_t* sink)
{
    if (!pipe || !src || !sink) return ZST_ERROR;

    int in_transaction = pipeline_reconfiguration_owned_by_current_thread(pipe);
    if (!in_transaction) {
        pipeline_wait_for_foreign_reconfiguration(pipe);
        pthread_rwlock_wrlock(&pipe->elements_lock);
    }

    zst_pad_t* peer = zst_pad_get_peer(src);
    if (peer != sink) {
        if (peer) zst_pad_unref(peer);
        if (!in_transaction) pthread_rwlock_unlock(&pipe->elements_lock);
        return ZST_ERROR;
    }
    zst_pad_unref(peer);

    zst_pad_unlink(src);
    pipeline_recalculate_ranks_locked(pipe);
    zst_pipeline_mark_buffer_pool_sizing_dirty(pipe);

    if (!in_transaction) {
        pipeline_reconfiguration_finish_locked(pipe);
    }
    return ZST_OK;
}

int
zst_pipeline_count_elements_of_type(zst_pipeline_t* pipe, const char* type_name)
{
    if (!pipe || !type_name) return 0;

    pthread_rwlock_rdlock(&pipe->elements_lock);
    int count = 0;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_t* el = pipe->elements[i];
        if (pipeline_element_is_type(el, type_name)) {
            count++;
        }
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
    return count;
}

void
zst_pipeline_foreach_element(zst_pipeline_t* pipe, void (*func)(zst_element_t*, void*), void* user_data)
{
    if (!pipe || !func) return;

    pthread_rwlock_rdlock(&pipe->elements_lock);
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        func(pipe->elements[i], user_data);
    }
    pthread_rwlock_unlock(&pipe->elements_lock);
}
