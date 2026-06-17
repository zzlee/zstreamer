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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


static zst_pad_probe_return_t pad_run_probes(zst_pad_t* pad,
                                             zst_buffer_t* buf,
                                             zst_pad_probe_type_t type);

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
        if (pad->peer) {
            if (pad_run_probes(pad->peer, NULL, ZST_PAD_PROBE_PRE_EVENT) != ZST_PAD_PROBE_DROP) {
                zst_pad_set_segment(pad->peer, segment);
                pad_run_probes(pad->peer, NULL, ZST_PAD_PROBE_POST_EVENT);
            }
            zst_element_t* downstream = pad->peer->parent;
            if (downstream) {
                for (uint32_t i = 0; i < downstream->nb_src_pads; i++) {
                    pad_propagate_segment(downstream->src_pads[i], segment, depth + 1);
                }
            }
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

    /* Handle drop flag (propagate downstream or skip immediately) */
    if (buf && (buf->flags & ZST_BUFFER_FLAG_DROP)) {
        if (el->nb_src_pads > 0) {
            return zst_pad_push(el->src_pads[0], buf);
        }
        return ZST_OK;
    }

    if (buf && (buf->flags & ZST_BUFFER_FLAG_EOS)) {
        if (el->nb_src_pads > 0) {
            return zst_pad_push(el->src_pads[0], buf);
        }
        /* Sink element receiving EOS */
        if (el->bus) {
            zst_event_t* eos_ev = zst_event_new_eos(el);
            zst_bus_post(el->bus, eos_ev);
        }
        return ZST_OK;
    }

    /* Sink element clock synchronization and QoS dropping */
    if (el->nb_src_pads == 0 && el->clock && buf && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
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

    if (el->ops->process) {
        ret = el->ops->process(el, buf, &out_buf);
    }

    if (ret == ZST_OK && out_buf) {
        if (el->nb_src_pads > 0) {
            ret = zst_pad_push(el->src_pads[0], out_buf);
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

    if (el->nb_sink_pads > 0) {
        zst_buffer_t* in_buf = NULL;
        ret = zst_pad_pull(el->sink_pads[0], &in_buf);
        if (ret != ZST_OK) {
            if (ret != ZST_EOF && ret != ZST_TIMEOUT && ret != ZST_AGAIN) {
                if (el->bus) {
                    zst_event_t* err_ev = zst_event_new_error(el, ret, "Upstream pull failed");
                    zst_bus_post(el->bus, err_ev);
                }
            }
            return ret;
        }

        if (in_buf && (in_buf->flags & ZST_BUFFER_FLAG_EOS)) {
            *out = in_buf;
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

    if (ret == ZST_OK) {
        *out = out_buf;
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

    return pad;
}

void
zst_pad_destroy(zst_pad_t* pad)
{
    if (!pad) return;

    /* Unlink from peer if still connected */
    if (pad->peer)
        zst_pad_unlink(pad);

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

    free((void*)pad->name);
    zst_caps_destroy(pad->caps);
    zst_caps_destroy(pad->template_caps);
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

    /* Refuse if either pad is already linked */
    if (src->peer || sink->peer)
        return ZST_ERROR;

    /* Negotiate caps first */
    zst_result_t ret = zst_pad_negotiate(src, sink);
    if (ret != ZST_OK) {
        return ret;
    }

    src->peer = sink;
    sink->peer = src;

    return ZST_OK;
}

void
zst_pad_unlink(zst_pad_t* pad)
{
    if (!pad) return;

    zst_pad_t* peer = pad->peer;
    if (!peer) return;

    /* Break the link from both sides */
    pad->peer = NULL;
    peer->peer = NULL;
}

zst_result_t
zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SRC) return ZST_ERROR;
    if (!pad->peer) return ZST_ERROR;

    if (!pad_buffer_in_segment(pad, buf) || !pad_buffer_in_segment(pad->peer, buf)) {
        return ZST_OK;
    }

    if (pad_run_probes(pad, buf, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP) {
        return ZST_OK;
    }
    if (pad_run_probes(pad->peer, buf, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP) {
        return ZST_OK;
    }

    zst_result_t ret = ZST_ERROR;
    if (pad->peer->push) {
        ret = pad->peer->push(pad->peer, buf);
    }

    if (ret == ZST_OK) {
        pad_run_probes(pad->peer, buf, ZST_PAD_PROBE_POST_BUFFER);
        pad_run_probes(pad, buf, ZST_PAD_PROBE_POST_BUFFER);
    }

    return ret;
}

zst_result_t
zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    if (!pad || !out) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SINK) return ZST_ERROR;
    if (!pad->peer) return ZST_ERROR;

    if (!pad->peer->pull) return ZST_ERROR;

    zst_result_t ret = pad->peer->pull(pad->peer, out);
    if (ret != ZST_OK || !out || !*out) return ret;

    if (!pad_buffer_in_segment(pad->peer, *out) || !pad_buffer_in_segment(pad, *out) ||
        pad_run_probes(pad->peer, *out, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(pad, *out, ZST_PAD_PROBE_PRE_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(pad, *out, ZST_PAD_PROBE_POST_BUFFER) == ZST_PAD_PROBE_DROP ||
        pad_run_probes(pad->peer, *out, ZST_PAD_PROBE_POST_BUFFER) == ZST_PAD_PROBE_DROP) {
        zst_buffer_unref(*out);
        *out = NULL;
        return ZST_AGAIN;
    }

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

    if (!intersect->structs) {
        zst_caps_destroy(intersect);
        return ZST_ERROR;
    }

    zst_result_t ret = zst_caps_fixate(intersect);
    if (ret != ZST_OK) {
        zst_caps_destroy(intersect);
        return ZST_ERROR;
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
