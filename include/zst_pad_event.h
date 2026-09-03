/*=============================================================================
    zst_pad_event.h - In-band pad signaling
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_caps.h"
#include "zst_stream.h"
#include "zst_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_PAD_EVENT_STREAM_START,
    ZST_PAD_EVENT_CAPS,
    ZST_PAD_EVENT_SEGMENT,
    ZST_PAD_EVENT_EOS,
    ZST_PAD_EVENT_FLUSH_START,
    ZST_PAD_EVENT_FLUSH_STOP,
    ZST_PAD_EVENT_GAP,
    ZST_PAD_EVENT_DISCONT,
    ZST_PAD_EVENT_FORCE_KEYFRAME
} zst_pad_event_type_t;

struct zst_pad_event {
    zst_pad_event_type_t type;
    _Atomic(int) refcount;

    union {
        struct {
            zst_stream_id_t stream_id;
        } stream_start;

        struct {
            zst_caps_t* caps;
        } caps;

        struct {
            zst_segment_t segment;
        } segment;
    } as;
};

typedef struct zst_pad_event zst_pad_event_t;

zst_pad_event_t* zst_pad_event_new_stream_start(zst_stream_id_t stream_id);
zst_pad_event_t* zst_pad_event_new_caps(const zst_caps_t* caps);
zst_pad_event_t* zst_pad_event_new_segment(const zst_segment_t* segment);
zst_pad_event_t* zst_pad_event_new_eos(void);
zst_pad_event_t* zst_pad_event_new_force_keyframe(void);

zst_pad_event_t* zst_pad_event_ref(zst_pad_event_t* ev);
void zst_pad_event_unref(zst_pad_event_t* ev);

zst_result_t zst_pad_push_event(zst_pad_t* pad, zst_pad_event_t* event);

#ifdef __cplusplus
}
#endif
