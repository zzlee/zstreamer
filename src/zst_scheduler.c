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
        if (zst_queue_push(p->ready_queue, token, 0) != ZST_OK) {
            zst_buffer_unref(token);
            atomic_store_explicit(&el->is_queued, false, memory_order_release);
        }
    } else {
        atomic_store_explicit(&el->is_queued, false, memory_order_release);
    }
}

static void 
execute_element_task(zst_scheduler_t* sched, zst_element_t* el, zst_pipeline_t* pipe) 
{
    if (atomic_load_explicit(&el->state, memory_order_acquire) != ZST_STATE_PLAYING) {
        atomic_store_explicit(&el->is_queued, false, memory_order_release);
        return;
    }

    bool is_source = true;
    for (uint32_t p_idx = 0; p_idx < el->nb_sink_pads; p_idx++) {
        if (el->sink_pads[p_idx] && el->sink_pads[p_idx]->peer) {
            is_source = false;
            break;
        }
    }

    if (el->ops && el->ops->process) {
        zst_buffer_t* out_buf = NULL;
        zst_result_t ret = el->ops->process(el, NULL, &out_buf);

        if (ret == ZST_OK) {
            if (out_buf) {
                if (el->nb_src_pads > 0 && el->src_pads[0]) {
                    if (pipe->clock_sync && el->clock && pipe->base_time > 0
                        && !(out_buf->flags & (ZST_BUFFER_FLAG_EOS | ZST_BUFFER_FLAG_DROP))) {
                        zst_time_t now = zst_clock_get_time(el->clock);
                        zst_time_t run_time = (now > pipe->base_time) ? (now - pipe->base_time) : 0;
                        if (out_buf->pts > run_time + 5000000ULL) {
                            zst_clock_wait(el->clock, out_buf->pts - run_time);
                        }
                    }
                    
                    zst_pad_push(el->src_pads[0], out_buf);
                    zst_buffer_unref(out_buf);
                }
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
                if (el->nb_src_pads > 0 && el->src_pads[0]) {
                    zst_pad_push(el->src_pads[0], eos_buf);
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
            zst_buffer_unref(task_buffer);
        } else {
            zst_pipeline_t* pipe = sched->pipeline;
            if (pipe) {
                pthread_rwlock_rdlock(&pipe->elements_lock);
                for (uint32_t i = 0; i < pipe->nb_elements; i++) {
                    zst_element_t* el = pipe->elements[i];
                    if (atomic_load_explicit(&el->state, memory_order_acquire) == ZST_STATE_PLAYING) {
                        bool is_src = true;
                        for (uint32_t p_idx = 0; p_idx < el->nb_sink_pads; p_idx++) {
                            if (el->sink_pads[p_idx] && el->sink_pads[p_idx]->peer) {
                                is_src = false;
                                break;
                            }
                        }
                        if (is_src) {
                            zst_scheduler_queue_task(sched, el);
                        }
                    }
                }
                pthread_rwlock_unlock(&pipe->elements_lock);
            }
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
            bool is_src = true;
            for (uint32_t p_idx = 0; p_idx < el->nb_sink_pads; p_idx++) {
                if (el->sink_pads[p_idx] && el->sink_pads[p_idx]->peer) {
                    is_src = false;
                    break;
                }
            }
            if (is_src) {
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
    return ZST_OK;
}