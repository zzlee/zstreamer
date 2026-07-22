/*=============================================================================
    zst_bus.c — Async notification channel / thread-safe event queue
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup, clock_gettime, CLOCK_REALTIME */

#include "zst_bus.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

typedef struct zst_event_node {
    zst_event_t*           event;
    struct zst_event_node* next;
} zst_event_node_t;

struct zst_bus {
    zst_event_node_t* head;
    zst_event_node_t* tail;
    uint32_t         count;

    pthread_mutex_t  lock;
    pthread_cond_t   cond;

    volatile int     flushing;

    pthread_t        thread;
    int              has_thread;
    zst_bus_handler_t handler;
    void*            user_data;
};

static void
timespec_from_ms(struct timespec* ts, uint32_t timeout_ms)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    ts->tv_sec  = now.tv_sec  + timeout_ms / 1000;
    ts->tv_nsec = now.tv_nsec + (timeout_ms % 1000) * 1000000L;

    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void*
bus_dispatch_loop(void* arg)
{
    zst_bus_t* bus = arg;
    while (1) {
        zst_event_t* event = NULL;
        zst_result_t r = zst_bus_pop(bus, &event, UINT32_MAX);
        if (r == ZST_OK && event) {
            pthread_mutex_lock(&bus->lock);
            zst_bus_handler_t handler = bus->handler;
            void* user_data = bus->user_data;
            pthread_mutex_unlock(&bus->lock);

            if (handler) {
                handler(bus, event, user_data);
            }
            zst_event_destroy(event);
        } else {
            pthread_mutex_lock(&bus->lock);
            int exit_loop = bus->flushing;
            pthread_mutex_unlock(&bus->lock);
            if (exit_loop) {
                break;
            }
        }
    }
    return NULL;
}

zst_bus_t*
zst_bus_create(void)
{
    zst_bus_t* bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;

    pthread_mutex_init(&bus->lock, NULL);
    pthread_cond_init(&bus->cond, NULL);
    bus->flushing = 0;
    bus->has_thread = 0;

    return bus;
}

void
zst_bus_destroy(zst_bus_t* bus)
{
    if (!bus) return;

    /* Stop the background dispatch thread if it exists */
    pthread_mutex_lock(&bus->lock);
    bus->flushing = 1;
    pthread_cond_broadcast(&bus->cond);
    int has_thread = bus->has_thread;
    pthread_t thread = bus->thread;
    pthread_mutex_unlock(&bus->lock);

    if (has_thread) {
        pthread_join(thread, NULL);
    }

    /* Free all queued events */
    pthread_mutex_lock(&bus->lock);
    zst_event_node_t* node = bus->head;
    while (node) {
        zst_event_node_t* next = node->next;
        zst_event_destroy(node->event);
        free(node);
        node = next;
    }
    pthread_mutex_unlock(&bus->lock);

    pthread_mutex_destroy(&bus->lock);
    pthread_cond_destroy(&bus->cond);
    free(bus);
}

zst_result_t
zst_bus_post(zst_bus_t* bus, zst_event_t* event)
{
    if (!bus || !event) return ZST_ERROR;

    pthread_mutex_lock(&bus->lock);

    if (bus->flushing) {
        pthread_mutex_unlock(&bus->lock);
        return ZST_ERROR;
    }

    zst_event_node_t* node = malloc(sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&bus->lock);
        return ZST_ERROR;
    }
    node->event = event;
    node->next = NULL;

    if (bus->tail) {
        bus->tail->next = node;
    } else {
        bus->head = node;
    }
    bus->tail = node;
    bus->count++;

    pthread_cond_signal(&bus->cond);
    pthread_mutex_unlock(&bus->lock);
    return ZST_OK;
}

zst_result_t
zst_bus_pop(zst_bus_t* bus, zst_event_t** event, uint32_t timeout_ms)
{
    if (!bus || !event) return ZST_ERROR;

    pthread_mutex_lock(&bus->lock);

    int use_timeout = (timeout_ms != UINT32_MAX);

    while (!bus->head && !bus->flushing) {
        if (use_timeout) {
            if (timeout_ms == 0) {
                pthread_mutex_unlock(&bus->lock);
                return ZST_TIMEOUT;
            }
            struct timespec ts;
            timespec_from_ms(&ts, timeout_ms);
            int ret = pthread_cond_timedwait(&bus->cond, &bus->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&bus->lock);
                return ZST_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&bus->cond, &bus->lock);
        }
    }

    if (bus->flushing) {
        pthread_mutex_unlock(&bus->lock);
        return ZST_ERROR;
    }

    /* Dequeue */
    zst_event_node_t* node = bus->head;
    bus->head = node->next;
    if (!bus->head) bus->tail = NULL;
    bus->count--;

    *event = node->event;
    free(node);

    pthread_mutex_unlock(&bus->lock);
    return ZST_OK;
}

zst_result_t
zst_bus_set_handler(zst_bus_t* bus, zst_bus_handler_t handler, void* user_data)
{
    if (!bus) return ZST_ERROR;

    pthread_mutex_lock(&bus->lock);

    /* Stop existing dispatch thread if it exists */
    if (bus->has_thread) {
        bus->flushing = 1;
        pthread_cond_broadcast(&bus->cond);
        pthread_t old_thread = bus->thread;
        pthread_mutex_unlock(&bus->lock);

        pthread_join(old_thread, NULL);

        pthread_mutex_lock(&bus->lock);
        bus->has_thread = 0;
        bus->flushing = 0;
    }

    bus->handler = handler;
    bus->user_data = user_data;

    if (handler) {
        if (pthread_create(&bus->thread, NULL, bus_dispatch_loop, bus) != 0) {
            pthread_mutex_unlock(&bus->lock);
            return ZST_ERROR;
        }
        bus->has_thread = 1;
    }

    pthread_mutex_unlock(&bus->lock);
    return ZST_OK;
}

zst_event_t*
zst_event_new_eos(zst_element_t* src)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_EOS;
    ev->src = src;
    return ev;
}

static void
copy_stream_info(zst_stream_info_t* dest, const zst_stream_info_t* src)
{
    if (!dest || !src) return;
    *dest = *src;
    if (src->name) dest->name = strdup(src->name);
    if (src->language) dest->language = strdup(src->language);
    if (src->caps) dest->caps = zst_caps_copy(src->caps);
}

zst_event_t*
zst_event_new_stream_added(zst_element_t* src, const zst_stream_info_t* stream)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_STREAM_ADDED;
    ev->src = src;
    copy_stream_info(&ev->as.stream_status.stream, stream);
    return ev;
}

zst_event_t*
zst_event_new_stream_removed(zst_element_t* src, zst_stream_id_t stream_id)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_STREAM_REMOVED;
    ev->src = src;
    ev->as.stream_removed.stream_id = stream_id;
    return ev;
}

zst_event_t*
zst_event_new_stream_changed(zst_element_t* src, const zst_stream_info_t* stream)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_STREAM_CHANGED;
    ev->src = src;
    copy_stream_info(&ev->as.stream_status.stream, stream);
    return ev;
}

zst_event_t*
zst_event_new_stream_status(zst_element_t* src, const zst_stream_info_t* stream)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_STREAM_STATUS;
    ev->src = src;
    copy_stream_info(&ev->as.stream_status.stream, stream);
    return ev;
}

zst_event_t*
zst_event_new_no_more_pads(zst_element_t* src)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_NO_MORE_PADS;
    ev->src = src;
    return ev;
}

zst_event_t*
zst_event_new_error(zst_element_t* src, zst_result_t result, const char* message)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_ERROR;
    ev->src = src;
    ev->as.error.result = result;
    ev->as.error.message = message ? strdup(message) : NULL;
    return ev;
}

zst_event_t*
zst_event_new_state_changed(zst_element_t* src, zst_state_t old_state, zst_state_t new_state)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_STATE_CHANGED;
    ev->src = src;
    ev->as.state_changed.old_state = old_state;
    ev->as.state_changed.new_state = new_state;
    return ev;
}

zst_event_t*
zst_event_new_warning(zst_element_t* src, zst_result_t result, const char* message)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_WARNING;
    ev->src = src;
    ev->as.warning.result = result;
    ev->as.warning.message = message ? strdup(message) : NULL;
    return ev;
}

zst_event_t*
zst_event_new_segment(zst_element_t* src, const zst_segment_t* segment)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_SEGMENT;
    ev->src = src;
    if (segment) ev->as.segment = *segment;
    return ev;
}

zst_event_t*
zst_event_new_pad_added(zst_element_t* src, zst_pad_t* pad, const zst_stream_info_t* stream)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_PAD_ADDED;
    ev->src = src;
    ev->as.pad_added.pad = zst_pad_ref(pad);
    if (stream) {
        ev->as.pad_added.stream = *stream;
        if (stream->name) ev->as.pad_added.stream.name = strdup(stream->name);
        if (stream->language) ev->as.pad_added.stream.language = strdup(stream->language);
        if (stream->caps) ev->as.pad_added.stream.caps = zst_caps_copy(stream->caps);
    }
    return ev;
}

zst_event_t*
zst_event_new_pad_removed(zst_element_t* src, zst_pad_t* pad, zst_stream_id_t stream_id)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_PAD_REMOVED;
    ev->src = src;
    ev->as.pad_removed.pad = zst_pad_ref(pad);
    ev->as.pad_removed.stream_id = stream_id;
    return ev;
}

zst_event_t*
zst_event_new_caps_changed(zst_element_t* src, zst_pad_t* pad, const zst_caps_t* old_caps, const zst_caps_t* new_caps)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_CAPS_CHANGED;
    ev->src = src;
    ev->as.caps_changed.pad = zst_pad_ref(pad);
    if (old_caps) ev->as.caps_changed.old_caps = zst_caps_copy(old_caps);
    if (new_caps) ev->as.caps_changed.new_caps = zst_caps_copy(new_caps);
    return ev;
}

zst_event_t*
zst_event_new_signal_lost(zst_element_t* src)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_SIGNAL_LOST;
    ev->src = src;
    return ev;
}

zst_event_t*
zst_event_new_signal_present(zst_element_t* src)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_SIGNAL_PRESENT;
    ev->src = src;
    return ev;
}

zst_event_t*
zst_event_new_key_press(zst_element_t* src, uint32_t key_sym, uint32_t key_code, const char* key_str)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_KEY_PRESS;
    ev->src = src;
    ev->as.key_press.key_sym = key_sym;
    ev->as.key_press.key_code = key_code;
    if (key_str) {
        snprintf(ev->as.key_press.key_str, sizeof(ev->as.key_press.key_str), "%s", key_str);
    }
    return ev;
}

zst_event_t*
zst_event_new_mouse_button(zst_element_t* src, uint32_t button, int pressed, int x, int y)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_MOUSE_BUTTON;
    ev->src = src;
    ev->as.mouse_button.button = button;
    ev->as.mouse_button.pressed = pressed;
    ev->as.mouse_button.x = x;
    ev->as.mouse_button.y = y;
    return ev;
}

zst_event_t*
zst_event_new_mouse_motion(zst_element_t* src, int x, int y)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_MOUSE_MOTION;
    ev->src = src;
    ev->as.mouse_motion.x = x;
    ev->as.mouse_motion.y = y;
    return ev;
}

zst_event_t*
zst_event_new_webrtc_local_description(
    zst_element_t* src, const char* type, const char* sdp)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION;
    ev->src = src;
    ev->as.webrtc_local_description.type = type ? strdup(type) : NULL;
    ev->as.webrtc_local_description.sdp  = sdp  ? strdup(sdp)  : NULL;
    return ev;
}

zst_event_t*
zst_event_new_webrtc_ice_candidate(
    zst_element_t* src, const char* mid, int mlineindex, const char* candidate)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_WEBRTC_ICE_CANDIDATE;
    ev->src = src;
    ev->as.webrtc_ice_candidate.mid        = mid        ? strdup(mid)        : NULL;
    ev->as.webrtc_ice_candidate.mlineindex = mlineindex;
    ev->as.webrtc_ice_candidate.candidate  = candidate  ? strdup(candidate)  : NULL;
    return ev;
}

zst_event_t*
zst_event_new_webrtc_pli(zst_element_t* src, int track_id)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_WEBRTC_PLI;
    ev->src = src;
    ev->as.webrtc_pli.track_id = track_id;
    return ev;
}

zst_event_t*
zst_event_new_webrtc_remb(zst_element_t* src, int track_id, unsigned int bitrate)
{
    zst_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_EVENT_WEBRTC_REMB;
    ev->src = src;
    ev->as.webrtc_remb.track_id = track_id;
    ev->as.webrtc_remb.bitrate = bitrate;
    return ev;
}

void
zst_event_destroy(zst_event_t* event)
{
    if (!event) return;
    if (event->type == ZST_EVENT_ERROR) {
        free(event->as.error.message);
    } else if (event->type == ZST_EVENT_WARNING) {
        free(event->as.warning.message);
    } else if (event->type == ZST_EVENT_PAD_ADDED) {
        if (event->as.pad_added.pad) zst_pad_unref(event->as.pad_added.pad);
        free(event->as.pad_added.stream.name);
        free(event->as.pad_added.stream.language);
        if (event->as.pad_added.stream.caps) zst_caps_destroy(event->as.pad_added.stream.caps);
    } else if (event->type == ZST_EVENT_PAD_REMOVED) {
        if (event->as.pad_removed.pad) zst_pad_unref(event->as.pad_removed.pad);
    } else if (event->type == ZST_EVENT_CAPS_CHANGED) {
        if (event->as.caps_changed.pad) zst_pad_unref(event->as.caps_changed.pad);
        if (event->as.caps_changed.old_caps) zst_caps_destroy(event->as.caps_changed.old_caps);
        if (event->as.caps_changed.new_caps) zst_caps_destroy(event->as.caps_changed.new_caps);
    } else if (event->type == ZST_EVENT_STREAM_STATUS ||
               event->type == ZST_EVENT_STREAM_ADDED ||
               event->type == ZST_EVENT_STREAM_CHANGED) {
        free(event->as.stream_status.stream.name);
        free(event->as.stream_status.stream.language);
        if (event->as.stream_status.stream.caps) zst_caps_destroy(event->as.stream_status.stream.caps);
    } else if (event->type == ZST_EVENT_MOUSE_MOTION) {
        /* no-op: mouse_motion has no owned fields */
    } else if (event->type == ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION) {
        free(event->as.webrtc_local_description.type);
        free(event->as.webrtc_local_description.sdp);
    } else if (event->type == ZST_EVENT_WEBRTC_ICE_CANDIDATE) {
        free(event->as.webrtc_ice_candidate.mid);
        free(event->as.webrtc_ice_candidate.candidate);
    } else if (event->type == ZST_EVENT_WEBRTC_PLI ||
               event->type == ZST_EVENT_WEBRTC_REMB) {
        /* no owned fields */
    }
    free(event);
}
