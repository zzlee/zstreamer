/*=============================================================================
    @file zst_bus.h
    @brief Async event bus for out-of-band notifications

    zst_bus_t provides an asynchronous notification channel decoupled from
    the data path.  Elements and the pipeline post events; applications
    consume them via polling (zst_bus_pop) or a registered callback.

    Supported event types:
    - EOS, ERROR, STATE_CHANGED, WARNING, SEGMENT
    - PAD_ADDED / PAD_REMOVED — dynamic pad lifecycle
    - STREAM_ADDED / STREAM_REMOVED / STREAM_CHANGED — stream tracking
    - CAPS_CHANGED — negotiated caps update
    - SIGNAL_PRESENT / SIGNAL_LOST — signal detection events
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"
#include "zst_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_EVENT_EOS,
    ZST_EVENT_ERROR,
    ZST_EVENT_STATE_CHANGED,
    ZST_EVENT_WARNING,
    ZST_EVENT_SEGMENT,

    ZST_EVENT_PAD_ADDED,
    ZST_EVENT_PAD_REMOVED,
    ZST_EVENT_STREAM_ADDED,
    ZST_EVENT_STREAM_REMOVED,
    ZST_EVENT_STREAM_CHANGED,
    ZST_EVENT_STREAM_STATUS,
    ZST_EVENT_CAPS_CHANGED,
    ZST_EVENT_NO_MORE_PADS,

    ZST_EVENT_REDUNDANCY_FAILOVER,

    ZST_EVENT_SIGNAL_PRESENT,
    ZST_EVENT_SIGNAL_LOST,
    ZST_EVENT_KEY_PRESS,
    ZST_EVENT_MOUSE_BUTTON,
    ZST_EVENT_MOUSE_MOTION,

    /* WebRTC signaling events */
    ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION,
    ZST_EVENT_WEBRTC_ICE_CANDIDATE,
    ZST_EVENT_WEBRTC_PLI,          /* Picture Loss Indication — request keyframe */
    ZST_EVENT_WEBRTC_REMB,         /* Receiver Estimated Maximum Bitrate */
} zst_event_type_t;

struct zst_event {
    zst_event_type_t type;
    zst_element_t* src;

    union {
        struct {
            zst_result_t result;
            char* message;
        } error;

        struct {
            zst_state_t old_state;
            zst_state_t new_state;
        } state_changed;

        struct {
            zst_result_t result;
            char* message;
        } warning;

        zst_segment_t segment;

        struct {
            zst_pad_t* pad;
            zst_stream_info_t stream;
        } pad_added;

        struct {
            zst_pad_t* pad;
            zst_stream_id_t stream_id;
        } pad_removed;

        struct {
            zst_pad_t* pad;
            zst_caps_t* old_caps;
            zst_caps_t* new_caps;
        } caps_changed;

        struct {
            zst_stream_info_t stream;
        } stream_status;

        struct {
            zst_stream_id_t stream_id;
        } stream_removed;

        struct {
            uint32_t key_sym;
            uint32_t key_code;
            char key_str[16];
        } key_press;

        struct {
            uint32_t button;
            int pressed;
            int x;
            int y;
        } mouse_button;

        struct {
            int x;
            int y;
        } mouse_motion;

        /* WebRTC signaling */
        struct {
            char* type;   /* "offer" or "answer" */
            char* sdp;    /* owned SDP string */
        } webrtc_local_description;

        struct {
            char* mid;        /* media stream id */
            int   mlineindex; /* media line index */
            char* candidate;  /* owned ICE candidate string */
        } webrtc_ice_candidate;

        /* WebRTC QoS */
        struct {
            int track_id;     /* libdatachannel track handle */
        } webrtc_pli;

        struct {
            int track_id;     /* libdatachannel track handle */
            unsigned int bitrate; /* estimated bitrate in bps */
        } webrtc_remb;
    } as;
};

typedef void (*zst_bus_handler_t)(
    zst_bus_t* bus,
    zst_event_t* event,
    void* user_data);

zst_bus_t* zst_bus_create(void);

void zst_bus_destroy(
    zst_bus_t* bus);

zst_result_t zst_bus_post(
    zst_bus_t* bus,
    zst_event_t* event);

zst_result_t zst_bus_pop(
    zst_bus_t* bus,
    zst_event_t** event,
    uint32_t timeout_ms);

zst_result_t zst_bus_set_handler(
    zst_bus_t* bus,
    zst_bus_handler_t handler,
    void* user_data);

zst_event_t* zst_event_new_eos(
    zst_element_t* src);

zst_event_t* zst_event_new_error(
    zst_element_t* src,
    zst_result_t result,
    const char* message);

zst_event_t* zst_event_new_state_changed(
    zst_element_t* src,
    zst_state_t old_state,
    zst_state_t new_state);

zst_event_t* zst_event_new_warning(
    zst_element_t* src,
    zst_result_t result,
    const char* message);

zst_event_t* zst_event_new_segment(
    zst_element_t* src,
    const zst_segment_t* segment);

zst_event_t* zst_event_new_pad_added(
    zst_element_t* src,
    zst_pad_t* pad,
    const zst_stream_info_t* stream);

zst_event_t* zst_event_new_pad_removed(
    zst_element_t* src,
    zst_pad_t* pad,
    zst_stream_id_t stream_id);

zst_event_t* zst_event_new_caps_changed(
    zst_element_t* src,
    zst_pad_t* pad,
    const zst_caps_t* old_caps,
    const zst_caps_t* new_caps);

zst_event_t* zst_event_new_stream_added(
    zst_element_t* src,
    const zst_stream_info_t* stream);

zst_event_t* zst_event_new_stream_removed(
    zst_element_t* src,
    zst_stream_id_t stream_id);

zst_event_t* zst_event_new_stream_changed(
    zst_element_t* src,
    const zst_stream_info_t* stream);

zst_event_t* zst_event_new_stream_status(
    zst_element_t* src,
    const zst_stream_info_t* stream);

zst_event_t* zst_event_new_no_more_pads(
    zst_element_t* src);

zst_event_t* zst_event_new_signal_lost(zst_element_t* src);
zst_event_t* zst_event_new_signal_present(zst_element_t* src);
zst_event_t* zst_event_new_key_press(
    zst_element_t* src,
    uint32_t key_sym,
    uint32_t key_code,
    const char* key_str);
zst_event_t* zst_event_new_mouse_button(
    zst_element_t* src,
    uint32_t button,
    int pressed,
    int x,
    int y);
zst_event_t* zst_event_new_mouse_motion(
    zst_element_t* src,
    int x,
    int y);

zst_event_t* zst_event_new_webrtc_local_description(
    zst_element_t* src,
    const char* type,
    const char* sdp);

zst_event_t* zst_event_new_webrtc_ice_candidate(
    zst_element_t* src,
    const char* mid,
    int mlineindex,
    const char* candidate);

zst_event_t* zst_event_new_webrtc_pli(
    zst_element_t* src,
    int track_id);

zst_event_t* zst_event_new_webrtc_remb(
    zst_element_t* src,
    int track_id,
    unsigned int bitrate);

void zst_event_destroy(
    zst_event_t* event);

#ifdef __cplusplus
}
#endif
