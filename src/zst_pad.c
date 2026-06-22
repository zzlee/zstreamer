/*=============================================================================
    zst_pad.c — Pad creation, linking, and unlinking
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_pad.h"
#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declaration */
static bool
execute_pad_probes_and_block(zst_pad_t* pad, zst_pad_probe_type_t type, zst_buffer_t* buf);

static zst_result_t
default_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!execute_pad_probes_and_block(pad, ZST_PAD_PROBE_TYPE_PRE_BUFFER, buf)) {
        return ZST_OK; /* Dropped */
    }

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
        if (!execute_pad_probes_and_block(pad, ZST_PAD_PROBE_TYPE_POST_BUFFER, out_buf)) {
            zst_buffer_unref(out_buf);
            return ZST_OK; /* Dropped */
        }
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

    if (!execute_pad_probes_and_block(pad, ZST_PAD_PROBE_TYPE_PRE_BUFFER, NULL)) {
        return ZST_AGAIN; /* Dropped */
    }

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
        if (!execute_pad_probes_and_block(pad, ZST_PAD_PROBE_TYPE_POST_BUFFER, out_buf)) {
            if (out_buf) zst_buffer_unref(out_buf);
            *out = NULL;
            return ZST_AGAIN; /* Dropped */
        }
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

/* Helper returning true if we should continue, false if we should drop (and return ZST_OK/AGAIN from caller) */
static bool
execute_pad_probes_and_block(zst_pad_t* pad, zst_pad_probe_type_t type, zst_buffer_t* buf)
{
    pthread_mutex_lock(&pad->lock);

    while (pad->is_blocked) {
        pthread_cond_wait(&pad->cond, &pad->lock);
    }

    zst_pad_probe_t* probe = pad->probes;
    bool should_drop = false;

    while (probe) {
        zst_pad_probe_t* next_probe = probe->next;
        if ((probe->mask & type) || (probe->mask & ZST_PAD_PROBE_TYPE_BLOCK)) {
            probe->running_count++;

            zst_pad_probe_type_t actual_type = (probe->mask & type) ? type : ZST_PAD_PROBE_TYPE_BLOCK;
            zst_pad_probe_info_t info = {
                .type = actual_type,
                .pad = pad,
                .buffer = buf
            };

            zst_pad_probe_cb cb = probe->callback;
            void* user_data = probe->user_data;
            pthread_mutex_unlock(&pad->lock);

            zst_pad_probe_return_t ret = cb(pad, &info, user_data);

            pthread_mutex_lock(&pad->lock);

            probe->running_count--;

            if (probe->pending_removal && probe->running_count == 0) {
                if (probe->destroy_data) {
                    probe->destroy_data(probe->user_data);
                }
                free(probe);
            }

            if (ret == ZST_PAD_PROBE_DROP) {
                should_drop = true;
                break;
            } else if (ret == ZST_PAD_PROBE_BLOCK || ret == ZST_PAD_PROBE_REBLOCK) {
                pad->is_blocked = true;
            }
        }
        probe = next_probe;
    }

    while (pad->is_blocked) {
        pthread_cond_wait(&pad->cond, &pad->lock);
    }

    pthread_mutex_unlock(&pad->lock);
    return !should_drop;
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
    pad->peer      = NULL;
    pad->priv      = NULL;

    pthread_mutex_init(&pad->lock, NULL);
    pthread_cond_init(&pad->cond, NULL);
    pad->is_blocked = false;
    pad->probes = NULL;
    pad->next_probe_id = 1;

    return pad;
}

void
zst_pad_destroy(zst_pad_t* pad)
{
    if (!pad) return;

    /* Unlink from peer if still connected */
    if (pad->peer)
        zst_pad_unlink(pad);

    /* Free probes */
    zst_pad_probe_t* probe = pad->probes;
    while (probe) {
        zst_pad_probe_t* next = probe->next;
        /* Ignore running_count during destroy since pad should not be in use */
        if (probe->destroy_data) {
            probe->destroy_data(probe->user_data);
        }
        free(probe);
        probe = next;
    }
    pad->probes = NULL;

    pthread_mutex_destroy(&pad->lock);
    pthread_cond_destroy(&pad->cond);

    free((void*)pad->name);
    zst_caps_destroy(pad->caps);
    zst_caps_destroy(pad->template_caps);
    free(pad);
}

uint32_t
zst_pad_add_probe(zst_pad_t* pad, uint32_t mask, zst_pad_probe_cb callback, void* user_data, zst_pad_probe_destroy_cb destroy_data)
{
    if (!pad || !callback) return 0;

    zst_pad_probe_t* p = calloc(1, sizeof(*p));
    if (!p) return 0;

    p->mask = mask;
    p->callback = callback;
    p->user_data = user_data;
    p->destroy_data = destroy_data;
    p->running_count = 0;
    p->pending_removal = false;

    pthread_mutex_lock(&pad->lock);
    p->id = pad->next_probe_id++;
    p->next = pad->probes;
    pad->probes = p;
    pthread_mutex_unlock(&pad->lock);

    return p->id;
}

void
zst_pad_remove_probe(zst_pad_t* pad, uint32_t probe_id)
{
    if (!pad || probe_id == 0) return;

    pthread_mutex_lock(&pad->lock);
    zst_pad_probe_t** p = &pad->probes;
    while (*p) {
        if ((*p)->id == probe_id) {
            zst_pad_probe_t* to_remove = *p;
            *p = to_remove->next;

            if (to_remove->running_count > 0) {
                to_remove->pending_removal = true;
                pthread_mutex_unlock(&pad->lock);
            } else {
                pthread_mutex_unlock(&pad->lock);
                if (to_remove->destroy_data) {
                    to_remove->destroy_data(to_remove->user_data);
                }
                free(to_remove);
            }
            return;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&pad->lock);
}

void
zst_pad_block(zst_pad_t* pad)
{
    if (!pad) return;
    pthread_mutex_lock(&pad->lock);
    pad->is_blocked = true;
    pthread_mutex_unlock(&pad->lock);
}

void
zst_pad_unblock(zst_pad_t* pad)
{
    if (!pad) return;
    pthread_mutex_lock(&pad->lock);
    pad->is_blocked = false;
    pthread_cond_broadcast(&pad->cond);
    pthread_mutex_unlock(&pad->lock);
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

    if (pad->peer->push) {
        return pad->peer->push(pad->peer, buf);
    }

    return ZST_ERROR;
}

zst_result_t
zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    if (!pad || !out) return ZST_ERROR;
    if (pad->direction != ZST_PAD_SINK) return ZST_ERROR;
    if (!pad->peer) return ZST_ERROR;

    if (pad->peer->pull) {
        return pad->peer->pull(pad->peer, out);
    }

    return ZST_ERROR;
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
