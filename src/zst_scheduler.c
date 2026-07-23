/*=============================================================================
    zst_scheduler.c - High-Performance Task-Driven Pipeline Orchestrator
=============================================================================*/

#define _POSIX_C_SOURCE 199309L

#include "zst_scheduler.h"
#include "zst_queue.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    _Atomic(bool)   running;
    pthread_t* threads;
    uint32_t        nb_threads;
    zst_queue_t* ready_queue;
    pthread_mutex_t source_cache_lock;
    zst_element_t** source_roots;
    uint32_t        nb_source_roots;
    uint32_t        source_roots_capacity;
    uint64_t        source_roots_graph_version;
} sched_priv_t;

typedef struct {
    zst_scheduler_t* sched;
    uint32_t         worker_id;
} worker_ctx_t;

zst_result_t
zst_scheduler_attach(zst_scheduler_t* sched, zst_pipeline_t* pipe)
{
    if (!sched || !pipe) return ZST_ERROR;
    sched->pipeline = pipe;
    pipe->priv = sched;
    return ZST_OK;
}

static void
zst_scheduler_queue_task(zst_scheduler_t* sched, zst_element_t* el)
{
    if (!sched || !el) return;
    sched_priv_t* p = sched->priv;
    if (!p || !atomic_load_explicit(&p->running, memory_order_acquire)) return;

    if (atomic_exchange_explicit(&el->is_queued, true, memory_order_acq_rel)) {
        return;
    }

    /* Bypassing high-frequency dynamic heap allocations by referencing the pre-allocated
     * task token (`sched_token`) embedded directly within the element structure. */
    zst_buffer_t* token = el->sched_token ? zst_buffer_ref(el->sched_token) : NULL;
    if (token) {
        atomic_fetch_add_explicit(&el->sched_task_refs, 1, memory_order_acq_rel);
        if (zst_queue_push(p->ready_queue, token, 0) == ZST_OK) {
            /* zst_queue_push() takes its own reference for the queued slot. */
            zst_buffer_unref(token);
        } else {
            atomic_fetch_sub_explicit(&el->sched_task_refs, 1, memory_order_acq_rel);
            zst_buffer_unref(token);
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
        }
    } else {
        atomic_store_explicit(&el->is_queued, false, memory_order_release);
    }
}

static void
zst_scheduler_release_task_buffer(zst_buffer_t* task_buffer, bool reset_queued)
{
    if (!task_buffer) return;
    zst_element_t* el = (zst_element_t*)task_buffer->memory.priv;
    if (el) {
        if (reset_queued) {
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
        }
        atomic_fetch_sub_explicit(&el->sched_task_refs, 1, memory_order_acq_rel);
    }
    zst_buffer_unref(task_buffer);
}

static bool
scheduler_element_has_linked_sink_pad(zst_element_t* el)
{
    uint32_t nb_sink_pads = zst_element_get_sink_pad_count(el);
    if (nb_sink_pads == 0) return false;

    if (nb_sink_pads == 1) {
        zst_pad_t* sink_pad = zst_element_get_first_sink_pad_ref(el);
        int linked = sink_pad ? zst_pad_is_linked(sink_pad) : 0;
        if (sink_pad) zst_pad_unref(sink_pad);
        return linked != 0;
    }

    zst_pad_t** sink_pads = NULL;
    if (zst_element_snapshot_sink_pads(el, &sink_pads, &nb_sink_pads) != ZST_OK) {
        return true;
    }

    bool linked = false;
    for (uint32_t i = 0; i < nb_sink_pads; i++) {
        if (sink_pads[i] && zst_pad_is_linked(sink_pads[i])) {
            linked = true;
            break;
        }
    }
    zst_element_pad_snapshot_free(sink_pads, nb_sink_pads);
    return linked;
}

static int
scheduler_refresh_source_roots_locked(zst_scheduler_t* sched, zst_pipeline_t* pipe)
{
    sched_priv_t* p = sched->priv;
    uint64_t graph_version = atomic_load_explicit(&pipe->graph_version, memory_order_acquire);

    pthread_mutex_lock(&p->source_cache_lock);
    if (p->source_roots_graph_version == graph_version) {
        pthread_mutex_unlock(&p->source_cache_lock);
        return 1;
    }

    if (p->source_roots_capacity < pipe->nb_elements) {
        zst_element_t** roots = realloc(p->source_roots,
                                        pipe->nb_elements * sizeof(*roots));
        if (!roots) {
            pthread_mutex_unlock(&p->source_cache_lock);
            return 0;
        }
        p->source_roots = roots;
        p->source_roots_capacity = pipe->nb_elements;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < pipe->nb_elements; i++) {
        zst_element_t* el = pipe->elements[i];
        if (!scheduler_element_has_linked_sink_pad(el)) {
            p->source_roots[count++] = el;
        }
    }
    p->nb_source_roots = count;
    p->source_roots_graph_version = graph_version;
    pthread_mutex_unlock(&p->source_cache_lock);
    return 1;
}

static void
scheduler_queue_source_roots(zst_scheduler_t* sched, bool playing_only)
{
    if (!sched || !sched->pipeline) return;

    sched_priv_t* p = sched->priv;
    zst_pipeline_t* pipe = sched->pipeline;
    pthread_rwlock_rdlock(&pipe->elements_lock);

    if (scheduler_refresh_source_roots_locked(sched, pipe)) {
        pthread_mutex_lock(&p->source_cache_lock);
        for (uint32_t i = 0; i < p->nb_source_roots; i++) {
            zst_element_t* el = p->source_roots[i];
            if (!playing_only ||
                atomic_load_explicit(&el->state, memory_order_acquire) == ZST_STATE_PLAYING) {
                zst_scheduler_queue_task(sched, el);
            }
        }
        pthread_mutex_unlock(&p->source_cache_lock);
    } else {
        for (uint32_t i = 0; i < pipe->nb_elements; i++) {
            zst_element_t* el = pipe->elements[i];
            if ((!playing_only ||
                 atomic_load_explicit(&el->state, memory_order_acquire) == ZST_STATE_PLAYING) &&
                !scheduler_element_has_linked_sink_pad(el)) {
                zst_scheduler_queue_task(sched, el);
            }
        }
    }

    pthread_rwlock_unlock(&pipe->elements_lock);
}

static void 
execute_element_task(zst_scheduler_t* sched, zst_element_t* el, zst_pipeline_t* pipe) 
{
    if (atomic_load_explicit(&el->state, memory_order_acquire) != ZST_STATE_PLAYING) {
        atomic_store_explicit(&el->is_queued, false, memory_order_release);
        return;
    }

    bool is_source = (zst_element_get_sink_pad_count(el) == 0);

    if (el->ops && el->ops->process) {
        zst_buffer_t* out_buf = NULL;
        zst_pipeline_update_buffer_pool_sizing_if_needed(pipe, el);
        zst_result_t ret = el->ops->process(el, NULL, &out_buf);

        if (ret == ZST_OK) {
            zst_pipeline_update_buffer_pool_sizing_if_needed(pipe, el);
            if (out_buf) {
                zst_pad_t* first_src_pad = zst_element_get_first_src_pad_ref(el);
                if (first_src_pad) {
                    if (pipe->clock_sync && el->clock && !(out_buf->flags & (ZST_BUFFER_FLAG_EOS | ZST_BUFFER_FLAG_DROP))) {
                        zst_time_t now = zst_clock_get_time(el->clock);
                        if (el->clock->is_ptp) {
                            if (now < out_buf->pts) {
                                zst_clock_wait(el->clock, out_buf->pts - now);
                            }
                        } else if (pipe->base_time > 0) {
                            zst_time_t run_time = (now > pipe->base_time) ? (now - pipe->base_time) : 0;
                            if (out_buf->pts > run_time + 5000000ULL) {
                                zst_clock_wait(el->clock, out_buf->pts - run_time);
                            }
                        }
                    }

                    zst_pad_push(first_src_pad, out_buf);
                    zst_pad_unref(first_src_pad);
                }
                zst_buffer_unref(out_buf);
            }
            
            if (is_source) {
                atomic_store_explicit(&el->is_queued, false, memory_order_release);
                zst_scheduler_queue_task(sched, el);
            } else {
                atomic_store_explicit(&el->is_queued, false, memory_order_release);
            }
        } else if (ret == ZST_EOF) {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_t* first_src_pad = zst_element_get_first_src_pad_ref(el);
                if (first_src_pad) {
                    zst_pad_push(first_src_pad, eos_buf);
                    zst_pad_unref(first_src_pad);
                } else if (el->bus) {
                    zst_bus_post(el->bus, zst_event_new_eos(el));
                }
                zst_buffer_unref(eos_buf);
            }
            atomic_store_explicit(&el->state, ZST_STATE_READY, memory_order_release);
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
        } else if (ret == ZST_AGAIN || ret == ZST_TIMEOUT) {
            struct timespec req = {0, 100000}; 
            nanosleep(&req, NULL);
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
            zst_scheduler_queue_task(sched, el);
        } else {
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_error(el, ret, "Pipeline engine runtime fault"));
            }
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
        }
    } else {
        atomic_store_explicit(&el->is_queued, false, memory_order_release);
    }
}

static void*
worker_pool_loop(void* arg)
{
    worker_ctx_t* ctx = arg;
    zst_scheduler_t* sched = ctx->sched;
    sched_priv_t* p = sched->priv;

    while (atomic_load_explicit(&p->running, memory_order_acquire)) {
        zst_buffer_t* task_buffer = NULL;
        zst_result_t res = zst_queue_pop(p->ready_queue, &task_buffer, 2); 

        if (res == ZST_OK && task_buffer) {
            zst_element_t* el = (zst_element_t*)task_buffer->memory.priv; 
            if (el && sched->pipeline) {
                execute_element_task(sched, el, sched->pipeline);
            }
            zst_scheduler_release_task_buffer(task_buffer, false);
        } else {
            scheduler_queue_source_roots(sched, true);
        }
    }

    free(ctx);
    return NULL;
}

zst_scheduler_t*
zst_scheduler_create(const zst_scheduler_config_t* cfg)
{
    zst_scheduler_t* sched = calloc(1, sizeof(*sched));
    if (!sched) return NULL;

    if (cfg) {
        sched->config = *cfg;
    } else {
        sched->config.mode           = ZST_SCHEDULER_SINGLE_THREAD;
        sched->config.worker_threads = 1;
    }

    sched_priv_t* p = calloc(1, sizeof(*p));
    if (!p) {
        free(sched);
        return NULL;
    }

    zst_queue_config_t q_cfg = { .mode = ZST_QUEUE_SYNC, .max_buffers = 2048 };
    p->ready_queue = zst_queue_create(&q_cfg);
    pthread_mutex_init(&p->source_cache_lock, NULL);
    p->source_roots = NULL;
    p->nb_source_roots = 0;
    p->source_roots_capacity = 0;
    p->source_roots_graph_version = 0;
    atomic_store_explicit(&p->running, false, memory_order_relaxed);
    
    sched->priv = p;
    return sched;
}

void
zst_scheduler_destroy(zst_scheduler_t* sched)
{
    if (!sched) return;
    sched_priv_t* p = sched->priv;
    if (p) {
        zst_scheduler_stop(sched);
        zst_queue_destroy(p->ready_queue);
        pthread_mutex_destroy(&p->source_cache_lock);
        free(p->source_roots);
        free(p);
    }
    free(sched);
}

zst_result_t
zst_scheduler_run(zst_scheduler_t* sched)
{
    if (!sched) return ZST_ERROR;
    sched_priv_t* p = sched->priv;
    if (atomic_load_explicit(&p->running, memory_order_acquire)) return ZST_OK;

    if (sched->pipeline) {
        zst_pipeline_topological_sort(sched->pipeline);
        zst_pipeline_update_buffer_pool_sizing(sched->pipeline);
    }

    atomic_store_explicit(&p->running, true, memory_order_release);
    uint32_t n = (sched->config.mode == ZST_SCHEDULER_MULTI_THREAD) ? sched->config.worker_threads : 1;

    p->threads = calloc(n, sizeof(pthread_t));
    p->nb_threads = n;

    if (sched->pipeline) {
        pthread_rwlock_rdlock(&sched->pipeline->elements_lock);
        for (uint32_t i = 0; i < sched->pipeline->nb_elements; i++) {
            zst_element_t* el = sched->pipeline->elements[i];
            if (zst_element_get_sink_pad_count(el) == 0) {
                zst_scheduler_queue_task(sched, el);
            }
        }
        pthread_rwlock_unlock(&sched->pipeline->elements_lock);
    }

    for (uint32_t i = 0; i < n; i++) {
        worker_ctx_t* ctx = malloc(sizeof(*ctx));
        if (!ctx) continue;
        ctx->sched = sched;
        ctx->worker_id = i;
        pthread_create(&p->threads[i], NULL, worker_pool_loop, ctx);
    }

    return ZST_OK;
}

zst_result_t
zst_scheduler_stop(zst_scheduler_t* sched)
{
    if (!sched) return ZST_ERROR;
    sched_priv_t* p = sched->priv;
    if (!atomic_load_explicit(&p->running, memory_order_acquire)) return ZST_OK;

    atomic_store_explicit(&p->running, false, memory_order_release);

    if (p->threads) {
        for (uint32_t i = 0; i < p->nb_threads; i++) {
            pthread_join(p->threads[i], NULL);
        }
        free(p->threads);
        p->threads = NULL;
        p->nb_threads = 0;
    }

    /* Discard any source tasks that were still queued but not picked up before
     * workers exited, releasing the corresponding element task references. */
    zst_buffer_t* task_buffer = NULL;
    while (zst_queue_pop(p->ready_queue, &task_buffer, 0) == ZST_OK) {
        zst_scheduler_release_task_buffer(task_buffer, true);
        task_buffer = NULL;
    }
    return ZST_OK;
}

zst_result_t
zst_scheduler_wake(zst_scheduler_t* sched)
{
    if (!sched) return ZST_ERROR;
    sched_priv_t* p = sched->priv;
    if (!p || !atomic_load_explicit(&p->running, memory_order_acquire)) return ZST_OK;

    zst_pipeline_t* pipe = sched->pipeline;
    if (pipe) {
        scheduler_queue_source_roots(sched, true);
    }
    return ZST_OK;
}