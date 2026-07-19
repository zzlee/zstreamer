/*=============================================================================
    zst_pad_event.c - In-band pad signaling implementation
=============================================================================*/

#include "zst_pad_event.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

zst_pad_event_t*
zst_pad_event_new_stream_start(zst_stream_id_t stream_id)
{
    zst_pad_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_PAD_EVENT_STREAM_START;
    atomic_init(&ev->refcount, 1);
    ev->as.stream_start.stream_id = stream_id;
    return ev;
}

zst_pad_event_t*
zst_pad_event_new_caps(const zst_caps_t* caps)
{
    zst_pad_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_PAD_EVENT_CAPS;
    atomic_init(&ev->refcount, 1);
    if (caps) ev->as.caps.caps = zst_caps_copy(caps);
    return ev;
}

zst_pad_event_t*
zst_pad_event_new_segment(const zst_segment_t* segment)
{
    zst_pad_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_PAD_EVENT_SEGMENT;
    atomic_init(&ev->refcount, 1);
    if (segment) ev->as.segment.segment = *segment;
    return ev;
}

zst_pad_event_t*
zst_pad_event_new_eos(void)
{
    zst_pad_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_PAD_EVENT_EOS;
    atomic_init(&ev->refcount, 1);
    return ev;
}

zst_pad_event_t*
zst_pad_event_new_force_keyframe(void)
{
    zst_pad_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = ZST_PAD_EVENT_FORCE_KEYFRAME;
    atomic_init(&ev->refcount, 1);
    return ev;
}

zst_pad_event_t*
zst_pad_event_ref(zst_pad_event_t* ev)
{
    if (!ev) return NULL;
    atomic_fetch_add_explicit(&ev->refcount, 1, memory_order_relaxed);
    return ev;
}

void
zst_pad_event_unref(zst_pad_event_t* ev)
{
    if (!ev) return;
    if (atomic_fetch_sub_explicit(&ev->refcount, 1, memory_order_acq_rel) == 1) {
        if (ev->type == ZST_PAD_EVENT_CAPS && ev->as.caps.caps) {
            zst_caps_destroy(ev->as.caps.caps);
        }
        free(ev);
    }
}
