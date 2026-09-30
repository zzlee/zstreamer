/*=============================================================================
    zst_rtsp_server.h — RTSP server element with multi-session support

    Serves multiple RTSP sessions on a single port, each with its own
    mount point (URI path). Sessions are mapped to named sink pad pairs.

    Pipeline usage:
        rtsp_server = zst_rtsp_server_create();
        zst_rtsp_server_add_session(server, "session0");
        zst_rtsp_server_add_session(server, "session1");

        // Link sources to session sink pads
        zst_pad_link(v4l2src_video, zst_element_get_pad(server, "session0_video"));
        zst_pad_link(alsa_audio,    zst_element_get_pad(server, "session0_audio"));
        zst_pad_link(other_video,   zst_element_get_pad(server, "session1_video"));

    Client URL:
        rtsp://host:8554/session0  →  gets session0's video+audio
        rtsp://host:8554/session1  →  gets session1's video+audio
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an RTSP server element.
 * Initially has no sessions. Use zst_rtsp_server_add_session() to add
 * mount points. Each session creates a video and audio sink pad.
 */
zst_element_t* zst_rtsp_server_create(void);

/**
 * Add a session/mount point to the RTSP server.
 *
 * @param el     RTSP server element
 * @param name   Mount point name (e.g., "session0" → rtsp://host:8554/session0)
 * @return ZST_OK on success, ZST_ERROR if session already exists
 *
 * This creates two sink pads:
 *   "<name>_video" — accepts video/x-h264 or video/x-h265 packets
 *   "<name>_audio" — accepts audio/aac packets
 */
zst_result_t zst_rtsp_server_add_session(zst_element_t* el, const char* name);

/**
 * Remove a session and its sink pads.
 */
zst_result_t zst_rtsp_server_remove_session(zst_element_t* el, const char* name);

/**
 * Get the number of configured sessions.
 */
int zst_rtsp_server_session_count(zst_element_t* el);

/**
 * Callback function type for dynamic session mounting (Media-On-Demand).
 * Called when a client requests a session/mount name that is not yet configured.
 * The application can use this callback to dynamically create sources and
 * call zst_rtsp_server_add_session() and link them.
 */
typedef zst_result_t (*zst_rtsp_server_mount_cb_t)(
    zst_element_t* server,
    const char* session_name,
    void* user_data
);

/**
 * Set the dynamic mount callback for the RTSP server.
 */
zst_result_t zst_rtsp_server_set_mount_callback(
    zst_element_t* server,
    zst_rtsp_server_mount_cb_t callback,
    void* user_data
);

/**
 * Set the raw avcC/hvcC extradata for a named session's video track.
 * This data is used to populate the SDP fmtp attribute with the correct
 * profile-level-id and sprop-parameter-sets (H.264) so that clients can
 * initialise their decoder before the first IDR frame arrives.
 *
 * @param el     RTSP server element
 * @param name   Session name (mount point)
 * @param data   Raw avcC/hvcC extradata bytes (will be copied internally)
 * @param size   Length of data in bytes
 */
zst_result_t zst_rtsp_server_session_set_extradata(
    zst_element_t* el,
    const char* name,
    const uint8_t* data,
    int size
);

/** Set the AAC track clock rate and channel count used in the session SDP. */
zst_result_t zst_rtsp_server_session_set_audio_format(
    zst_element_t* el, const char* name, int sample_rate, int channels);

/**
 * Get the number of clients currently connected to a specific session.
 */
int zst_rtsp_server_session_client_count(
    zst_element_t* el,
    const char* name
);

#ifdef __cplusplus
}
#endif
