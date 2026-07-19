/*=============================================================================
    zst_pad.c — Pad creation, linking, and unlinking
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_pad.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_pad_event.h"
#include "zst_bin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>


static zst_pad_probe_return_t pad_run_probes(zst_pad_t* pad,
                                             zst_buffer_t* buf,
                                             zst_pad_probe_type_t type);

static void zst_pad_finalize(zst_pad_t* pad);
static zst_result_t zst_pad_push_event_internal(zst_pad_t* src, zst_pad_event_t* event, uint32_t depth);
static zst_result_t zst_pad_push_event_upstream_internal(zst_pad_t* sink, zst_pad_event_t* event, uint32_t depth);

static void
pad_lock_pair(zst_pad_t* a, zst_pad_t* b)
{
    if (a == b) {
        pthread_mutex_lock(&a->link_lock);
        return;
    }
    if ((uintptr_t)a < (uintptr_t)b) {
        pthread_mutex_lock(&a->link_lock);
        pthread_mutex_lock(&b->link_lock);
    } else {
        pthread_mutex_lock(&b->link_lock);
        pthread_mutex_lock(&a->link_lock);
    }
}

static void
pad_unlock_pair(zst_pad_t* a, zst_pad_t* b)
{
    if (a == b) {
        pthread_mutex_unlock(&a->link_lock);
        return;
    }
    pthread_mutex_unlock(&a->link_lock);
    pthread_mutex_unlock(&b->link_lock);
}

static int
pad_buffer_in_segment(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf) return 1;
    if (buf->flags & ZST_BUFFER_FLAG_EOS) return 1;

    pthread_mutex_lock(&pad->probe_lock);
    int has_segment = pad->has_segment;
    zst_segment_t segment = pad->segment;
    pthread_mutex_unlock(&pad->probe_lock);
    if (!has_segment) return 1;

    zst_time_t pts = buf->pts;
    zst_time_t end = buf->duration > 0 ? pts + buf->duration : pts;

    if (buf->duration > 0) {
        if (end <= segment.start) return 0;
    } else if (pts < segment.start) {
        return 0;
    }

    if (segment.stop != ZST_SEGMENT_STOP_NONE && pts >= segment.stop) {
        return 0;
    }

    return 1;
}

static zst_result_t
pad_propagate_segment(zst_pad_t* pad, const zst_segment_t* segment, uint32_t depth)
{
    if (!pad || !segment) return ZST_ERROR;
    if (depth > 256) return ZST_ERROR;

    if (pad_run_probes(pad, NULL, ZST_PAD_PROBE_PRE_EVENT) == ZST_PAD_PROBE_DROP) {
        return ZST_OK;
    }

    zst_pad_set_segment(pad, segment);

    if (pad->direction == ZST_PAD_SRC) {
        pthread_mutex_lock(&pad->link_lock);
        zst_pad_t* peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
        pthread_mutex_unlock(&pad->link_lock);
        if (peer) {
            if (pad_run_probes(peer, NULL, ZST_PAD_PROBE_PRE_EVENT) != ZST_PAD_PROBE_DROP) {
                zst_pad_set_segment(peer, segment);
                pad_run_probes(peer, NULL, ZST_PAD_PROBE_POST_EVENT);
            }
            zst_element_t* downstream = peer->parent;
            if (downstream) {
                zst_pad_t** downstream_src_pads = NULL;
                uint32_t nb_downstream_src_pads = 0;
                zst_element_snapshot_src_pads(downstream, &downstream_src_pads, &nb_downstream_src_pads);
                for (uint32_t i = 0; i < nb_downstream_src_pads; i++) {
                    pad_propagate_segment(downstream_src_pads[i], segment, depth + 1);
                }
                zst_element_pad_snapshot_free(downstream_src_pads, nb_downstream_src_pads);
            }
            zst_pad_unref(peer);
        }
    }

    pad_run_probes(pad, NULL, ZST_PAD_PROBE_POST_EVENT);
    return ZST_OK;
}

static zst_pad_probe_return_t
pad_handle_block(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type)
{
    if (!pad) return ZST_PAD_PROBE_OK;

    for (;;) {
        pthread_mutex_lock(&pad->probe_lock);
        if (!pad->blocked) {
            pad->block_callback_fired = 0;
            pthread_mutex_unlock(&pad->probe_lock);
            return ZST_PAD_PROBE_OK;
        }

        zst_pad_probe_fn callback = NULL;
        void* user_data = NULL;
        if (pad->block_callback && !pad->block_callback_fired) {
            pad->block_callback_fired = 1;
            callback = pad->block_callback;
            user_data = pad->block_user_data;
        }
        pthread_mutex_unlock(&pad->probe_lock);

        if (callback) {
            zst_pad_probe_return_t cb_ret = callback(pad, buf, type, user_data);
            if (cb_ret == ZST_PAD_PROBE_OK) {
                zst_pad_unblock(pad);
                return ZST_PAD_PROBE_OK;
            }
            if (cb_ret == ZST_PAD_PROBE_DROP) {
                return ZST_PAD_PROBE_DROP;
            }
        }

        pthread_mutex_lock(&pad->probe_lock);
        while (pad->blocked) {
            pthread_cond_wait(&pad->probe_cond, &pad->probe_lock);
        }
        pad->block_callback_fired = 0;
        pthread_mutex_unlock(&pad->probe_lock);
    }
}

static zst_pad_probe_return_t
pad_run_probes(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type)
{
    if (!pad) return ZST_PAD_PROBE_OK;

    /* Hot-path fast exit: avoid probe locking/allocation when the pad is not
     * blocked and no probes are installed. */
    if (!atomic_load_explicit(&pad->blocked, memory_order_acquire) &&
        atomic_load_explicit(&pad->has_probes, memory_order_acquire) == 0) {
        return ZST_PAD_PROBE_OK;
    }

    zst_pad_probe_return_t block_ret = pad_handle_block(pad, buf, type);
    if (block_ret != ZST_PAD_PROBE_OK) return block_ret;

    for (;;) {
        pthread_mutex_lock(&pad->probe_lock);
        uint32_t n = 0;
        for (zst_pad_probe_t* p = pad->probes; p; p = p->next) {
            if (p->callback && (p->types & (uint32_t)type)) n++;
        }

        zst_pad_probe_fn* callbacks = n ? calloc(n, sizeof(*callbacks)) : NULL;
        void** user_data = n ? calloc(n, sizeof(*user_data)) : NULL;
        if (n && (!callbacks || !user_data)) {
            free(callbacks);
            free(user_data);
            pthread_mutex_unlock(&pad->probe_lock);
            return ZST_PAD_PROBE_OK;
        }

        uint32_t idx = 0;
        for (zst_pad_probe_t* p = pad->probes; p; p = p->next) {
            if (p->callback && (p->types & (uint32_t)type)) {
                callbacks[idx] = p->callback;
                user_data[idx] = p->user_data;
                idx++;
            }
        }
        pthread_mutex_unlock(&pad->probe_lock);

        zst_pad_probe_return_t ret = ZST_PAD_PROBE_OK;
        for (uint32_t i = 0; i < n; i++) {
            zst_pad_probe_return_t cb_ret = callbacks[i](pad, buf, type, user_data[i]);
            if (cb_ret == ZST_PAD_PROBE_DROP) {
                ret = ZST_PAD_PROBE_DROP;
                break;
            }
            if (cb_ret == ZST_PAD_PROBE_BLOCK || cb_ret == ZST_PAD_PROBE_REBLOCK) {
                zst_pad_block(pad);
                ret = pad_handle_block(pad, buf, type);
                if (ret != ZST_PAD_PROBE_OK) break;
            }
        }

        free(callbacks);
        free(user_data);
        return ret;
    }
}

static zst_result_t
default_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    if (!el || !el->ops) return ZST_ERROR;

    zst_pad_t* first_src_pad = zst_element_get_first_src_pad_ref(el);

    /* Handle drop flag (propagate downstream or skip immediately) */
    if (buf && (buf->flags & ZST_BUFFER_FLAG_DROP)) {
        if (first_src_pad) {
            zst_result_t r = zst_pad_push(first_src_pad, buf);
            zst_pad_unref(first_src_pad);
            return r;
        }
        return ZST_OK;
    }

    if (buf && (buf->flags & ZST_BUFFER_FLAG_EOS)) {
        if (first_src_pad) {
            zst_result_t r = zst_pad_push(first_src_pad, buf);
            zst_pad_unref(first_src_pad);
            return r;
        }
        /* Sink element receiving EOS */
        if (el->bus) {
            zst_event_t* eos_ev = zst_event_new_eos(el);
            zst_bus_post(el->bus, eos_ev);
        }
        return ZST_OK;
    }

    /* Sink element clock synchronization and QoS dropping */
    if (!first_src_pad && el->clock && buf && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_time_t current = zst_clock_get_time(el->clock);
        if (buf->pts > current + 5000000ULL) { /* 5ms early threshold */
            if (buf->pts - current < 5000000000ULL) { /* 5s safeguard */
                zst_clock_wait(el->clock, buf->pts - current);
            }
        } else if (buf->pts < current - 100000000ULL) { /* 100ms late threshold */
            if (current - buf->pts < 5000000000ULL) { /* 5s safeguard */
                /* Drop late buffer to catch up (QoS) */
                buf->flags |= ZST_BUFFER_FLAG_DROP;
                if (el->bus) {
                    zst_event_t* qos_ev = zst_event_new_warning(el, ZST_ERROR, "QoS: Frame dropped (too late)");
                    zst_bus_post(el->bus, qos_ev);
                }
                return ZST_OK;
            }
        }
    }

    zst_buffer_t* out_buf = NULL;
    zst_result_t ret = ZST_OK;

    if (el->pipeline) {
        zst_pipeline_update_buffer_pool_sizing_if_needed(el->pipeline, el);
    }

    if (el->ops->process) {
        ret = el->ops->process(el, buf, &out_buf);
    }

    if (ret == ZST_OK && out_buf) {
        if (first_src_pad) {
            ret = zst_pad_push(first_src_pad, out_buf);
            if (out_buf != buf) {
                zst_buffer_unref(out_buf);
            } else if (out_buf->refcount > 1) {
                /* Some in-place elements return zst_buffer_ref(in) so direct
                   process() callers own an output reference.  Drop that extra
                   reference in the pad-driven path while preserving the
                   upstream caller's original reference. */
                zst_buffer_unref(out_buf);
            }
        }
    }

    zst_pad_unref(first_src_pad);

    if (ret == ZST_OK && el->pipeline) {
        zst_pipeline_update_buffer_pool_sizing_if_needed(el->pipeline, el);
    }

    if (ret != ZST_OK && ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
        if (el->bus) {
            zst_event_t* err_ev = zst_event_new_error(el, ret, "Element push processing failed");
            zst_bus_post(el->bus, err_ev);
        }
    }

    return ret;
}

static zst_result_t
default_src_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    zst_element_t* el = pad->parent;
    if (!el || !el->ops) return ZST_ERROR;

    zst_buffer_t* out_buf = NULL;
    zst_result_t ret = ZST_OK;

    if (el->pipeline) {
        zst_pipeline_update_buffer_pool_sizing_if_needed(el->pipeline, el);
    }

    zst_pad_t* first_sink_pad = zst_element_get_first_sink_pad_ref(el);

    if (first_sink_pad) {
        zst_buffer_t* in_buf = NULL;
        ret = zst_pad_pull(first_sink_pad, &in_buf);
        if (ret != ZST_OK) {
            if (ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
                if (el->bus) {
                    zst_event_t* err_ev = zst_event_new_error(el, ret, "Upstream pull failed");
                    zst_bus_post(el->bus, err_ev);
                }
            }
            zst_pad_unref(first_sink_pad);
            return ret;
        }

        if (in_buf && (in_buf->flags & ZST_BUFFER_FLAG_EOS)) {
            *out = in_buf;
            zst_pad_unref(first_sink_pad);
            return ZST_OK;
        }

        if (el->ops->process) {
            ret = el->ops->process(el, in_buf, &out_buf);
        }
        zst_buffer_unref(in_buf);
    } else {
        if (el->ops->process) {
            ret = el->ops->process(el, NULL, &out_buf);
        }
    }

    zst_pad_unref(first_sink_pad);

    if (ret == ZST_OK) {
        *out = out_buf;
        if (el->pipeline) {
            zst_pipeline_update_buffer_pool_sizing_if_needed(el->pipeline, el);
        }
    } else {
        if (ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
            if (el->bus) {
                zst_event_t* err_ev = zst_event_new_error(el, ret, "Element pull processing failed");
                zst_bus_post(el->bus, err_ev);
            }
        }
    }

    return ret;
}

zst_pad_t*
zst_pad_create(const char* name, zst_pad_direction_t direction)
{
    zst_pad_t* pad = calloc(1, sizeof(*pad));
    if (!pad) return NULL;

    pad->name          = name ? strdup(name) : NULL;
    pad->direction     = direction;
    pad->parent        = NULL;
    pad->caps          = NULL;
    pad->template_caps = NULL;
    if (direction == ZST_PAD_SRC) {
        pad->pull = default_src_pad_pull;
        pad->push = NULL;
    } else {
        pad->push = default_sink_pad_push;
        pad->pull = NULL;
    }
    pad->peer         = NULL;
    atomic_init(&pad->linked, 0);
    pthread_mutex_init(&pad->link_lock, NULL);
    pad->priv         = NULL;
    pad->destroy_priv = NULL;
    pad->probes       = NULL;
    atomic_init(&pad->has_probes, 0);
    pad->next_probe_id = 1;
    pthread_mutex_init(&pad->probe_lock, NULL);
    pthread_cond_init(&pad->probe_cond, NULL);
    pad->blocked = 0;
    pad->block_callback_fired = 0;
    pad->block_callback = NULL;
    pad->block_user_data = NULL;
    pad->has_segment = 0;
    pad->segment = zst_segment_default();
    pad->spillover_policy = ZST_SPILLOVER_BLOCK;
    pad->unlinked_policy = (int)ZST_PAD_UNLINKED_ERROR;
    pad->max_queued = 0;
    pad->queued_count = 0;

    pad->last_transit_time   = 0;
    pad->media_jitter_ns     = 0.0;
    pad->jitter_buffer_count = 0;
    pthread_mutex_init(&pad->jitter_lock, NULL);

    atomic_init(&pad->refcount, 1);

    return pad;
}

zst_pad_t*
zst_pad_ref(zst_pad_t* pad)
{
    if (!pad) return NULL;
    atomic_fetch_add_explicit(&pad->refcount, 1, memory_order_relaxed);
    return pad;
}

void
zst_pad_unref(zst_pad_t* pad)
{
    if (!pad) return;
    if (atomic_fetch_sub_explicit(&pad->refcount, 1, memory_order_acq_rel) == 1) {
        zst_pad_finalize(pad);
    }
}

void
zst_pad_destroy(zst_pad_t* pad)
{
    zst_pad_unref(pad);
}

static void
zst_pad_finalize(zst_pad_t* pad)
{
    if (!pad) return;

    /* Pads should be explicitly unlinked before their final owner is dropped.
     * If an inconsistent peer pointer remains, detach it defensively without
     * dropping reciprocal link refs from a zero-ref finalizer path. */
    pthread_mutex_lock(&pad->link_lock);
    zst_pad_t* stale_peer = pad->peer;
    pad->peer = NULL;
    atomic_store_explicit(&pad->linked, 0, memory_order_release);
    pthread_mutex_unlock(&pad->link_lock);
    if (stale_peer) {
        pthread_mutex_lock(&stale_peer->link_lock);
        if (stale_peer->peer == pad) {
            stale_peer->peer = NULL;
            atomic_store_explicit(&stale_peer->linked, 0, memory_order_release);
        }
        pthread_mutex_unlock(&stale_peer->link_lock);
    }

    if (pad->destroy_priv) {
        pad->destroy_priv(pad);
    }

    pthread_mutex_lock(&pad->probe_lock);
    zst_pad_probe_t* probe = pad->probes;
    pad->probes = NULL;
    pad->blocked = 0;
    pthread_cond_broadcast(&pad->probe_cond);
    pthread_mutex_unlock(&pad->probe_lock);
    while (probe) {
        zst_pad_probe_t* next = probe->next;
        free(probe);
        probe = next;
    }
    pthread_cond_destroy(&pad->probe_cond);
    pthread_mutex_destroy(&pad->probe_lock);
    pthread_mutex_destroy(&pad->jitter_lock);
    pthread_mutex_destroy(&pad->link_lock);

    if (pad->sticky_stream_start) zst_pad_event_unref(pad->sticky_stream_start);
    if (pad->sticky_caps) zst_pad_event_unref(pad->sticky_caps);
    if (pad->sticky_segment) zst_pad_event_unref(pad->sticky_segment);

    free((void*)pad->name);
    if (pad->caps) zst_caps_destroy(pad->caps);
    if (pad->template_caps) zst_caps_destroy(pad->template_caps);
    free(pad);
}

zst_result_t
zst_pad_link(zst_pad_t* src, zst_pad_t* sink)
{
    if (!src || !sink)
        return ZST_ERROR;

    if (src->direction != ZST_PAD_SRC)
        return ZST_ERROR;

    if (sink->direction != ZST_PAD_SINK)
        return ZST_ERROR;

    /* Negotiate caps first */
    zst_result_t ret = zst_pad_negotiate(src, sink);
    if (ret != ZST_OK) {
        return ret;
    }

    pad_lock_pair(src, sink);
    /* Refuse if either pad is already linked */
    if (src->peer || sink->peer) {
        pad_unlock_pair(src, sink);
        return ZST_ERROR;
    }

    src->peer = zst_pad_ref(sink);
    sink->peer = zst_pad_ref(src);
    atomic_store_explicit(&src->linked, 1, memory_order_release);
    atomic_store_explicit(&sink->linked, 1, memory_order_release);
    pad_unlock_pair(src, sink);

    /* Replay sticky events */
    if (src->sticky_stream_start) zst_pad_push_event(src, src->sticky_stream_start);
    if (src->sticky_caps) zst_pad_push_event(src, src->sticky_caps);
    if (src->sticky_segment) zst_pad_push_event(src, src->sticky_segment);

    if (src->parent && src->parent->pipeline) {
        zst_pipeline_mark_buffer_pool_sizing_dirty(src->parent->pipeline);
    }
    if (sink->parent && sink->parent->pipeline &&
        (!src->parent || sink->parent->pipeline != src->parent->pipeline)) {
        zst_pipeline_mark_buffer_pool_sizing_dirty(sink->parent->pipeline);
    }

    return ZST_OK;
}

zst_pad_t*
zst_pad_get_peer(zst_pad_t* pad)
{
    if (!pad) return NULL;
    pthread_mutex_lock(&pad->link_lock);
    zst_pad_t* peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
    pthread_mutex_unlock(&pad->link_lock);
    return peer;
}

int
zst_pad_is_linked(zst_pad_t* pad)
{
    if (!pad) return 0;
    return atomic_load_explicit(&pad->linked, memory_order_acquire) != 0;
}

void
zst_pad_unlink(zst_pad_t* pad)
{
    if (!pad) return;

    pthread_mutex_lock(&pad->link_lock);
    zst_pad_t* peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
    pthread_mutex_unlock(&pad->link_lock);
    if (!peer) return;

    zst_pipeline_t* pad_pipe = pad->parent ? pad->parent->pipeline : NULL;
    zst_pipeline_t* peer_pipe = peer->parent ? peer->parent->pipeline : NULL;
    int unlinked = 0;

    pad_lock_pair(pad, peer);
    if (pad->peer == peer) {
        pad->peer = NULL;
        atomic_store_explicit(&pad->linked, 0, memory_order_release);
        if (peer->peer == pad) {
            peer->peer = NULL;
            atomic_store_explicit(&peer->linked, 0, memory_order_release);
        }
        unlinked = 1;
    }
    pad_unlock_pair(pad, peer);

    if (unlinked) {
        if (pad_pipe) {
            zst_pipeline_mark_buffer_pool_sizing_dirty(pad_pipe);
        }
        if (peer_pipe && peer_pipe != pad_pipe) {
            zst_pipeline_mark_buffer_pool_sizing_dirty(peer_pipe);
        }
        /* Drop the reciprocal references created by zst_pad_link(). */
        zst_pad_unref(peer);
        zst_pad_unref(pad);
    }

    zst_pad_unref(peer);
}

zst_result_t
zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SRC) return ZST_ERROR;

    pthread_mutex_lock(&pad->link_lock);
    zst_pad_t* peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
    pthread_mutex_unlock(&pad->link_lock);
    if (!peer) {
        /* Apply unlinked-pad policy */
        switch ((zst_pad_unlinked_policy_t)pad->unlinked_policy) {
        case ZST_PAD_UNLINKED_DROP:
            return ZST_OK;
        case ZST_PAD_UNLINKED_BLOCK:
            /* Spin-wait with a small sleep for link to appear */
            {
                int retries = 100000; /* ~1 second */
                while (retries-- > 0) {
                    pthread_mutex_lock(&pad->link_lock);
                    peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
                    pthread_mutex_unlock(&pad->link_lock);
                    if (peer) break;
                    usleep(10);
                }
                if (!peer) return ZST_TIMEOUT;
            }
            break;
        case ZST_PAD_UNLINKED_QUEUE:
            if (pad->queued_count < pad->max_queued) {
                /* Accept but silently drop — queued buffers are dropped;
                 * a real queue element should be inserted by the pipeline. */
                pad->queued_count++;
                return ZST_OK;
            }
            return ZST_ERROR;
        case ZST_PAD_UNLINKED_ERROR:
        default:
            return ZST_ERROR;
        }
    }

    if (!pad_buffer_in_segment(pad, buf) || !pad_buffer_in_segment(peer, buf)) {
        zst_pad_unref(peer);
        return ZST_OK;
    }

    /* Media Jitter Tracking (RFC 3550) */
    zst_clock_t* clock = pad->parent ? pad->parent->clock : NULL;
    if (clock && buf && buf->pts != (zst_time_t)-1 && buf->pts != 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_time_t R = zst_clock_get_time(clock);
        zst_time_t S = buf->pts;
        pthread_mutex_lock(&pad->jitter_lock);
        if (pad->jitter_buffer_count > 0) {
            int64_t current_transit = (int64_t)R - (int64_t)S;
            int64_t diff = current_transit - pad->last_transit_time;
            if (diff < 0) diff = -diff;
            pad->media_jitter_ns = pad->media_jitter_ns + ((double)diff - pad->media_jitter_ns) / 16.0;
            pad->last_transit_time = current_transit;
        } else {
            pad->last_transit_time = (int64_t)R - (int64_t)S;
        }
        pad->jitter_buffer_count++;
        pthread_mutex_unlock(&pad->jitter_lock);
    }

    if (pad_run_probes(pad, buf, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP) {
        zst_pad_unref(peer);
        return ZST_OK;
    }
    if (pad_run_probes(peer, buf, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP) {
        zst_pad_unref(peer);
        return ZST_OK;
    }

    zst_result_t ret = ZST_ERROR;
    if (peer->push) {
        ret = peer->push(peer, buf);
    }

    if (ret == ZST_OK) {
        pad_run_probes(peer, buf, ZST_PAD_PROBE_POST_BUFFER);
        pad_run_probes(pad, buf, ZST_PAD_PROBE_POST_BUFFER);
    }

    zst_pad_unref(peer);
    return ret;
}

zst_result_t
zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    if (!pad || !out) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SINK) return ZST_ERROR;

    pthread_mutex_lock(&pad->link_lock);
    zst_pad_t* peer = pad->peer ? zst_pad_ref(pad->peer) : NULL;
    pthread_mutex_unlock(&pad->link_lock);
    if (!peer) return ZST_ERROR;

    if (!peer->pull) {
        zst_pad_unref(peer);
        return ZST_ERROR;
    }

    zst_result_t ret = peer->pull(peer, out);
    if (ret != ZST_OK || !out || !*out) {
        zst_pad_unref(peer);
        return ret;
    }

    if (!pad_buffer_in_segment(peer, *out) || !pad_buffer_in_segment(pad, *out) ||
        pad_run_probes(peer, *out, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(pad, *out, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(pad, *out, ZST_PAD_PROBE_POST_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(peer, *out, ZST_PAD_PROBE_POST_BUFFER) == ZST_PAD_PROBE_DROP) {
        zst_buffer_unref(*out);
        *out = NULL;
        zst_pad_unref(peer);
        return ZST_AGAIN;
    }

    zst_pad_unref(peer);
    return ret;
}

void
zst_pad_reset_callbacks(zst_pad_t* pad)
{
    if (!pad) return;
    if (pad->direction == ZST_PAD_SRC) {
        pad->pull = default_src_pad_pull;
        pad->push = NULL;
    } else {
        pad->push = default_sink_pad_push;
        pad->pull = NULL;
    }
}

zst_result_t
zst_pad_set_caps(zst_pad_t* pad, const zst_caps_t* caps)
{
    if (!pad) return ZST_ERROR;

    if (pad->caps) {
        zst_caps_destroy(pad->caps);
        pad->caps = NULL;
    }

    if (caps) {
        pad->caps = zst_caps_copy(caps);
        if (!pad->caps) return ZST_ERROR;
    }

    return ZST_OK;
}

zst_caps_t*
zst_pad_get_caps(zst_pad_t* pad)
{
    if (!pad) return NULL;

    if (pad->caps) {
        return zst_caps_copy(pad->caps);
    }

    zst_element_t* el = pad->parent;
    if (el && el->ops && el->ops->get_caps) {
        zst_caps_t* caps = el->ops->get_caps(el, pad, NULL);
        if (caps) return caps;
    }

    if (pad->template_caps) {
        return zst_caps_copy(pad->template_caps);
    }

    return NULL;
}

zst_result_t
zst_pad_set_template_caps(zst_pad_t* pad, const zst_caps_t* caps)
{
    if (!pad) return ZST_ERROR;

    if (pad->template_caps) {
        zst_caps_destroy(pad->template_caps);
        pad->template_caps = NULL;
    }

    if (caps) {
        pad->template_caps = zst_caps_copy(caps);
        if (!pad->template_caps) return ZST_ERROR;
    }

    return ZST_OK;
}

zst_result_t
zst_pad_negotiate(zst_pad_t* src, zst_pad_t* sink)
{
    if (!src || !sink) return ZST_ERROR;

    zst_caps_t* src_caps = zst_pad_get_caps(src);
    zst_caps_t* sink_caps = zst_pad_get_caps(sink);

    if (!src_caps || !sink_caps) {
        if (src_caps) zst_caps_destroy(src_caps);
        if (sink_caps) zst_caps_destroy(sink_caps);
        return ZST_OK;
    }

    zst_caps_t* intersect = zst_caps_intersect(src_caps, sink_caps);
    zst_caps_destroy(src_caps);
    zst_caps_destroy(sink_caps);

    if (!intersect) {
        return ZST_ERROR;
    }

    zst_result_t ret = ZST_OK;
    if (!zst_caps_is_fixed(intersect)) {
        ret = zst_caps_fixate(intersect);
        if (ret != ZST_OK) {
            zst_caps_destroy(intersect);
            return ZST_ERROR;
        }
    }

    ret = zst_pad_set_caps(src, intersect);
    if (ret == ZST_OK) {
        ret = zst_pad_set_caps(sink, intersect);
    }

    zst_caps_destroy(intersect);
    return ret;
}

uint64_t
zst_pad_add_probe(zst_pad_t* pad, uint32_t types,
                  zst_pad_probe_fn callback, void* user_data)
{
    if (!pad || !callback || types == 0) return 0;

    zst_pad_probe_t* probe = calloc(1, sizeof(*probe));
    if (!probe) return 0;
    probe->types = types;
    probe->callback = callback;
    probe->user_data = user_data;

    pthread_mutex_lock(&pad->probe_lock);
    probe->id = pad->next_probe_id++;
    if (probe->id == 0) probe->id = pad->next_probe_id++;
    probe->next = pad->probes;
    pad->probes = probe;
    uint64_t id = probe->id;
    atomic_fetch_add_explicit(&pad->has_probes, 1, memory_order_release);
    pthread_mutex_unlock(&pad->probe_lock);

    return id;
}

zst_result_t
zst_pad_remove_probe(zst_pad_t* pad, uint64_t probe_id)
{
    if (!pad || probe_id == 0) return ZST_ERROR;

    pthread_mutex_lock(&pad->probe_lock);
    zst_pad_probe_t** cur = &pad->probes;
    while (*cur) {
        if ((*cur)->id == probe_id) {
            zst_pad_probe_t* removed = *cur;
            *cur = removed->next;
            atomic_fetch_sub_explicit(&pad->has_probes, 1, memory_order_release);
            pthread_mutex_unlock(&pad->probe_lock);
            free(removed);
            return ZST_OK;
        }
        cur = &(*cur)->next;
    }
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_ERROR;
}

zst_result_t
zst_pad_block(zst_pad_t* pad)
{
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    pad->blocked = 1;
    pad->block_callback_fired = 0;
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

zst_result_t
zst_pad_unblock(zst_pad_t* pad)
{
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    pad->blocked = 0;
    pad->block_callback_fired = 0;
    pthread_cond_broadcast(&pad->probe_cond);
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

int
zst_pad_is_blocked(zst_pad_t* pad)
{
    if (!pad) return 0;
    pthread_mutex_lock(&pad->probe_lock);
    int blocked = pad->blocked;
    pthread_mutex_unlock(&pad->probe_lock);
    return blocked;
}

zst_result_t
zst_pad_set_block_callback(zst_pad_t* pad,
                           zst_pad_probe_fn callback,
                           void* user_data)
{
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    pad->block_callback = callback;
    pad->block_user_data = user_data;
    pad->block_callback_fired = 0;
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

zst_result_t
zst_pad_set_segment(zst_pad_t* pad, const zst_segment_t* segment)
{
    if (!pad || !segment) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    pad->segment = *segment;
    if (pad->segment.rate == 0.0) pad->segment.rate = 1.0;
    if (pad->segment.stop != ZST_SEGMENT_STOP_NONE &&
        pad->segment.stop < pad->segment.start) {
        pthread_mutex_unlock(&pad->probe_lock);
        return ZST_ERROR;
    }
    pad->has_segment = 1;
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

zst_result_t
zst_pad_get_segment(zst_pad_t* pad, zst_segment_t* segment_out)
{
    if (!pad || !segment_out) return ZST_ERROR;
    pthread_mutex_lock(&pad->probe_lock);
    if (!pad->has_segment) {
        pthread_mutex_unlock(&pad->probe_lock);
        return ZST_ERROR;
    }
    *segment_out = pad->segment;
    pthread_mutex_unlock(&pad->probe_lock);
    return ZST_OK;
}

void
zst_pad_clear_segment(zst_pad_t* pad)
{
    if (!pad) return;
    pthread_mutex_lock(&pad->probe_lock);
    pad->has_segment = 0;
    pad->segment = zst_segment_default();
    pad->spillover_policy = ZST_SPILLOVER_BLOCK;
    pthread_mutex_unlock(&pad->probe_lock);
}

zst_result_t
zst_pad_push_segment(zst_pad_t* src, const zst_segment_t* segment)
{
    if (!src || !segment || src->direction != ZST_PAD_SRC) return ZST_ERROR;
    return pad_propagate_segment(src, segment, 0);
}

zst_result_t
zst_pad_get_media_jitter(zst_pad_t* pad, double* jitter_ns_out)
{
    if (!pad) return ZST_ERROR;
    pthread_mutex_lock(&pad->jitter_lock);
    if (jitter_ns_out) *jitter_ns_out = pad->media_jitter_ns;
    pthread_mutex_unlock(&pad->jitter_lock);
    return ZST_OK;
}

static zst_result_t
zst_pad_push_event_internal(zst_pad_t* src, zst_pad_event_t* event, uint32_t depth)
{
    if (!src || !event || src->direction != ZST_PAD_SRC) return ZST_ERROR;
    if (depth > 256) return ZST_ERROR;

    /* Update sticky events on the source pad. */
    pthread_mutex_lock(&src->probe_lock);
    if (event->type == ZST_PAD_EVENT_STREAM_START) {
        zst_pad_event_t* old = src->sticky_stream_start;
        src->sticky_stream_start = zst_pad_event_ref(event);
        if (old) zst_pad_event_unref(old);
    } else if (event->type == ZST_PAD_EVENT_CAPS) {
        zst_pad_event_t* old = src->sticky_caps;
        src->sticky_caps = zst_pad_event_ref(event);
        if (old) zst_pad_event_unref(old);
        if (event->as.caps.caps) {
            zst_pad_set_caps(src, event->as.caps.caps);
        }
    } else if (event->type == ZST_PAD_EVENT_SEGMENT) {
        zst_pad_event_t* old = src->sticky_segment;
        src->sticky_segment = zst_pad_event_ref(event);
        if (old) zst_pad_event_unref(old);
        src->segment = event->as.segment.segment;
        src->has_segment = 1;
    }
    pthread_mutex_unlock(&src->probe_lock);

    if (pad_run_probes(src, NULL, ZST_PAD_PROBE_PRE_EVENT) == ZST_PAD_PROBE_DROP) {
        return ZST_OK;
    }

    pthread_mutex_lock(&src->link_lock);
    zst_pad_t* peer = src->peer ? zst_pad_ref(src->peer) : NULL;
    pthread_mutex_unlock(&src->link_lock);

    if (peer) {
        if (!peer->parent && peer->name && strcmp(peer->name, "ghost-proxy-sink") == 0) {
            zst_pad_t* ghost = (zst_pad_t*)peer->priv;
            if (ghost) {
                zst_pad_unref(peer);
                return zst_pad_push_event_internal(ghost, event, depth + 1);
            }
        }

        /* Resolve ghost pads and set caps/segment along the way */
        zst_pad_t* current = peer;
        while (current) {
            if (event->type == ZST_PAD_EVENT_CAPS && event->as.caps.caps) {
                zst_pad_set_caps(current, event->as.caps.caps);
            } else if (event->type == ZST_PAD_EVENT_SEGMENT) {
                pthread_mutex_lock(&current->probe_lock);
                current->segment = event->as.segment.segment;
                current->has_segment = 1;
                pthread_mutex_unlock(&current->probe_lock);
            }

            zst_pad_t* target = zst_ghost_pad_get_target(current);
            if (!target) break;
            zst_pad_ref(target);
            zst_pad_unref(current);
            current = target;
        }
        peer = current;

        if (pad_run_probes(peer, NULL, ZST_PAD_PROBE_PRE_EVENT) != ZST_PAD_PROBE_DROP) {
            zst_result_t ret = ZST_OK;
            zst_element_t* downstream = peer->parent;
            if (downstream && downstream->ops && downstream->ops->event) {
                ret = downstream->ops->event(downstream, peer, event);
            } else if (downstream) {
                zst_pad_t** downstream_src_pads = NULL;
                uint32_t nb_downstream_src_pads = 0;
                if (zst_element_snapshot_src_pads(downstream, &downstream_src_pads,
                                                  &nb_downstream_src_pads) == ZST_OK) {
                    for (uint32_t i = 0; i < nb_downstream_src_pads; i++) {
                        zst_pad_push_event_internal(downstream_src_pads[i], event, depth + 1);
                    }
                    zst_element_pad_snapshot_free(downstream_src_pads, nb_downstream_src_pads);
                }
            }
            pad_run_probes(peer, NULL, ZST_PAD_PROBE_POST_EVENT);
            zst_pad_unref(peer);
            if (ret != ZST_OK) return ret;
        } else {
            zst_pad_unref(peer);
        }
    }

    pad_run_probes(src, NULL, ZST_PAD_PROBE_POST_EVENT);
    return ZST_OK;
}

zst_result_t
zst_pad_push_event(zst_pad_t* src, zst_pad_event_t* event)
{
    return zst_pad_push_event_internal(src, event, 0);
}

static zst_result_t
zst_pad_push_event_upstream_internal(zst_pad_t* sink, zst_pad_event_t* event, uint32_t depth)
{
    if (!sink || !event || sink->direction != ZST_PAD_SINK) return ZST_ERROR;
    if (depth > 256) return ZST_ERROR;

    if (pad_run_probes(sink, NULL, ZST_PAD_PROBE_PRE_EVENT) == ZST_PAD_PROBE_DROP) {
        return ZST_OK;
    }

    pthread_mutex_lock(&sink->link_lock);
    zst_pad_t* peer = sink->peer ? zst_pad_ref(sink->peer) : NULL;
    pthread_mutex_unlock(&sink->link_lock);

    if (!peer && sink->parent && sink->parent->parent_bin) {
        zst_element_t* bin = sink->parent->parent_bin;
        for (uint32_t i = 0; i < bin->nb_sink_pads; i++) {
            zst_pad_t* gp = bin->sink_pads[i];
            if (zst_ghost_pad_get_target(gp) == sink) {
                return zst_pad_push_event_upstream_internal(gp, event, depth + 1);
            }
        }
    }

    if (peer) {
        /* Resolve ghost pads */
        zst_pad_t* current = peer;
        while (current) {
            zst_pad_t* target = zst_ghost_pad_get_target(current);
            if (!target) break;
            zst_pad_ref(target);
            zst_pad_unref(current);
            current = target;
        }
        peer = current;

        if (pad_run_probes(peer, NULL, ZST_PAD_PROBE_PRE_EVENT) != ZST_PAD_PROBE_DROP) {
            zst_result_t ret = ZST_ERROR;
            zst_element_t* upstream = peer->parent;
            if (upstream && upstream->ops && upstream->ops->event) {
                ret = upstream->ops->event(upstream, peer, event);
            }
            if (ret != ZST_OK && upstream) {
                zst_pad_t** upstream_sink_pads = NULL;
                uint32_t nb_upstream_sink_pads = 0;
                if (zst_element_snapshot_sink_pads(upstream, &upstream_sink_pads,
                                                   &nb_upstream_sink_pads) == ZST_OK) {
                    for (uint32_t i = 0; i < nb_upstream_sink_pads; i++) {
                        zst_pad_push_event_upstream_internal(upstream_sink_pads[i], event, depth + 1);
                    }
                    zst_element_pad_snapshot_free(upstream_sink_pads, nb_upstream_sink_pads);
                }
                ret = ZST_OK;
            }
            pad_run_probes(peer, NULL, ZST_PAD_PROBE_POST_EVENT);
            zst_pad_unref(peer);
            if (ret != ZST_OK) return ret;
        } else {
            zst_pad_unref(peer);
        }
    }

    pad_run_probes(sink, NULL, ZST_PAD_PROBE_POST_EVENT);
    return ZST_OK;
}

zst_result_t
zst_pad_push_event_upstream(zst_pad_t* sink, zst_pad_event_t* event)
{
    return zst_pad_push_event_upstream_internal(sink, event, 0);
}

zst_result_t
zst_pad_set_unlinked_policy(
    zst_pad_t* pad,
    zst_pad_unlinked_policy_t policy,
    uint32_t max_queued_buffers)
{
    if (!pad) return ZST_ERROR;
    pad->unlinked_policy = (int)policy;
    pad->max_queued = max_queued_buffers;
    pad->queued_count = 0;
    return ZST_OK;
}

zst_result_t
zst_pad_push_sticky_events(zst_pad_t* pad)
{
    if (!pad) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SRC) return ZST_ERROR;

    if (!pad->peer) return ZST_ERROR;

    /* Replay sticky events in order: STREAM_START, CAPS, SEGMENT */
    if (pad->sticky_stream_start) {
        zst_pad_push_event_internal(pad, pad->sticky_stream_start, 1);
    }
    if (pad->sticky_caps) {
        zst_pad_push_event_internal(pad, pad->sticky_caps, 1);
    }
    if (pad->sticky_segment) {
        zst_pad_push_event_internal(pad, pad->sticky_segment, 1);
    }

    return ZST_OK;
}
