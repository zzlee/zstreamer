/*=============================================================================
    zst_dante_video_coordinator.h - Public Dante H.264 video routing API
=============================================================================*/
#pragma once

#include "zst_bus.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_DANTE_VIDEO_COORDINATOR_FACTORY "dantevideocoordinator"

#define ZST_DANTE_VIDEO_COORDINATOR_PROP_HEALTH_TIMEOUT_MS "health-timeout-ms"
#define ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_WINDOW "reorder-window"
#define ZST_DANTE_VIDEO_COORDINATOR_PROP_REORDER_TIMEOUT_MS "reorder-timeout-ms"
#define ZST_DANTE_VIDEO_COORDINATOR_PROP_MULTICAST_INTERFACE_ADDRESS "multicast-interface-address"

#define ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_HEALTH_TIMEOUT_MS 1000u
#define ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_REORDER_WINDOW 64u
#define ZST_DANTE_VIDEO_COORDINATOR_DEFAULT_REORDER_TIMEOUT_MS 20u
#define ZST_DANTE_VIDEO_COORDINATOR_MAX_REORDER_WINDOW 4096u

/* Creates a coordinator. Its element factory/ops name is
 * ZST_DANTE_VIDEO_COORDINATOR_FACTORY. */
zst_element_t* zst_dante_video_coordinator_create(void);

/* The session remains owned by the caller and must outlive the attachment.
 * Pass NULL before destroying the session. RX SSRC changes after inactivity
 * restart reorder probation and retarget the depayloader; partial pre-restart
 * access units are deliberately discarded because its public API has no reset. */
zst_result_t zst_dante_video_coordinator_attach_session(
    zst_element_t* coordinator,
    zst_element_t* session);

/* One public input/output pad may be requested per channel index. The returned
 * pad is owned by the coordinator and remains valid until released. */
zst_pad_t* zst_dante_video_coordinator_request_tx_input_pad(
    zst_element_t* coordinator,
    uint32_t channel_index);
zst_result_t zst_dante_video_coordinator_release_tx_input_pad(
    zst_element_t* coordinator,
    zst_pad_t* pad);
zst_pad_t* zst_dante_video_coordinator_request_rx_output_pad(
    zst_element_t* coordinator,
    uint32_t channel_index);
zst_result_t zst_dante_video_coordinator_release_rx_output_pad(
    zst_element_t* coordinator,
    zst_pad_t* pad);

/* Flow strings are copied. Apply requires the matching channel pad and a
 * coordinator already owned by a pipeline. TX channels allow multiple flows;
 * RX channels allow one flow. */
zst_result_t zst_dante_video_coordinator_apply_flow(
    zst_element_t* coordinator,
    const zst_dante_flow_t* flow);
zst_result_t zst_dante_video_coordinator_remove_flow(
    zst_element_t* coordinator,
    const zst_dante_flow_t* flow);

/* Cleans up and removes all active routes and their dynamic elements from the pipeline.
 * Must be called when the pipeline is NOT currently inside zst_pipeline_set_state. */
zst_result_t zst_dante_video_coordinator_remove_all_flows(
    zst_element_t* coordinator);

uint32_t zst_dante_video_coordinator_get_flow_count(zst_element_t* coordinator);

/* Debug API: returns the danteudpsink element for a TX flow (for RTP stats),
 * or NULL if not found.  Caller must not free the returned pointer. */
zst_element_t*
zst_dante_video_coordinator_get_tx_udp_sink(
    zst_element_t* coordinator,
    uint32_t flow_index);

/* Debug API: returns the danteudpsrc element for an RX flow (for RTP stats),
 * or NULL if not found.  Caller must not free the returned pointer. */
zst_element_t*
zst_dante_video_coordinator_get_rx_udp_source(
    zst_element_t* coordinator,
    uint32_t flow_index);

#ifdef __cplusplus
}
#endif
