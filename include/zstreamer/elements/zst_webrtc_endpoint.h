/*=============================================================================
    zst_webrtc_endpoint.h — WebRTC endpoint element public API

    Provides a unified WebRTC peer-connection element that can act as both
    sender (sink pads) and receiver (source pads).  Internally it wraps
    libdatachannel's C API to manage ICE, DTLS-SRTP, and SCTP.

    Outbound path (sender):
        video_test_src -> x264enc -> webrtc_endpoint   (dynamic sink_%u)
    Inbound path (receiver):
        webrtc_endpoint -> h264dec -> glsink           (dynamic src_%u)

    Signaling is application-driven:
      - Push remote SDP offers/answers via zst_webrtc_set_remote_description()
      - Push remote ICE candidates via zst_webrtc_add_ice_candidate()
      - Receive local SDPs and ICE candidates via element properties or
        a future callback/event mechanism (Phase 2).
=============================================================================*/
#pragma once

#include "zst_element.h"
#include "zst_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   WebRTC Signaling Lifecycle

   When a webrtc_endpoint is opened:
     1. A PeerConnection is created with ICE server config.
     2. The application calls zst_webrtc_create_offer() to start negotiation,
        or the element auto-generates an offer when opened.
     3. The element posts ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION via the
        element's bus with the local SDP.  The application should forward
        the SDP to the remote peer via its signaling channel.
     4. As ICE candidates are gathered, the element posts
        ZST_EVENT_WEBRTC_ICE_CANDIDATE.  The application should forward
        each candidate to the remote peer.
     5. The application calls zst_webrtc_set_remote_description() with the
        remote SDP, then zst_webrtc_add_ice_candidate() for each remote
        candidate.
     6. Once ICE connectivity is established, the element transitions its
        ICE state to "connected" and sets negotiated=true.

   Event types (via zst_bus_t on the element's bus):
     ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION — local SDP ready
       .as.webrtc_local_description.type  ("offer" / "answer")
       .as.webrtc_local_description.sdp   (SDP string)
     ZST_EVENT_WEBRTC_ICE_CANDIDATE — local ICE candidate ready
       .as.webrtc_ice_candidate.mid         (media stream id)
       .as.webrtc_ice_candidate.mlineindex  (media line index)
       .as.webrtc_ice_candidate.candidate   (ICE candidate string)
   ═══════════════════════════════════════════════════════════════════════════ */

#define ZST_WEBRTC_ENDPOINT_FACTORY  "webrtc_endpoint"

/* ── Property names (string literals for set/get_property) ──────────────── */
#define ZST_WEBRTC_PROPICE_SERVERS      "stun-servers"
#define ZST_WEBRTC_PROP_ICE_SERVERS     "turn-servers"
#define ZST_WEBRTC_PROP_ICE_URLS        "ice-urls"
#define ZST_WEBRTC_PROP_ICE_STATE       "ice-state"
#define ZST_WEBRTC_PROP_DTLS_STATE      "dtls-state"
#define ZST_WEBRTC_PROP_SCTP_STATE      "sctp-state"
#define ZST_WEBRTC_PROP_LOCAL_SDP       "local-sdp"
#define ZST_WEBRTC_PROP_REMOTE_SDP      "remote-sdp"
#define ZST_WEBRTC_PROP_SIGNALLING_STATE "signalling-state"
#define ZST_WEBRTC_PROP_NEGOTIATED      "negotiated"

/* ── ICE server description ─────────────────────────────────────────────── */
typedef struct {
    const char* url;           /* e.g. "stun:stun.l.google.com:19302" */
    const char* username;      /* TURN username (NULL for STUN) */
    const char* credential;    /* TURN credential (NULL for STUN) */
} zst_webrtc_ice_server_t;

/* ── Configuration for create_with_config ───────────────────────────────── */
typedef struct {
    size_t struct_size;            /* Must be >= sizeof(zst_webrtc_endpoint_config_t) */
    const zst_webrtc_ice_server_t* ice_servers;
    uint32_t                       num_ice_servers;
    bool                           enable_data_channels;
} zst_webrtc_endpoint_config_t;

/* ── Element constructors ───────────────────────────────────────────────── */

/**
 * Create a new WebRTC endpoint element with no ICE servers configured.
 * Use zst_element_set_property() to add STUN/TURN URLs before starting.
 */
zst_element_t* zst_webrtc_endpoint_create(void);

/**
 * Create a WebRTC endpoint with an explicit configuration.
 */
zst_element_t* zst_webrtc_endpoint_create_with_config(
    const zst_webrtc_endpoint_config_t* config);

/* ── Signaling API ────────────────────────────────────────────────────── */
/**
 * Create an SDP offer and start the negotiation.
 * The local SDP will be delivered via ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION
 * on the element's bus.
 */
zst_result_t zst_webrtc_create_offer(zst_element_t* el);

/**
 * Set the remote SDP description (offer or answer).
 * Call after the element is in READY or PLAYING state.
 *
 * @param type  "offer" or "answer"
 * @param sdp   The SDP text from the remote peer
 * @return ZST_OK on success
 */
zst_result_t zst_webrtc_set_remote_description(
    zst_element_t* el, const char* type, const char* sdp);

/**
 * Add a remote ICE candidate received from the remote peer.
 *
 * @param mid         Media stream identification tag
 * @param mlineindex  Media line index (0-based)
 * @param candidate   The ICE candidate string (a=candidate:...)
 * @return ZST_OK on success
 */
zst_result_t zst_webrtc_add_ice_candidate(
    zst_element_t* el, const char* mid, int mlineindex, const char* candidate);

/**
 * Create a data channel on this peer connection.
 * Must be called after the element is in READY state.
 *
 * @param label  Data channel label string
 * @return ZST_OK on success
 */
zst_result_t zst_webrtc_create_data_channel(
    zst_element_t* el, const char* label);

/* ── Media Track API (Phase 3) ────────────────────────────────────────── */

/**
 * Supported media codec identifiers for WebRTC tracks.
 */
typedef enum {
    ZST_WEBRTC_CODEC_H264  = 0,
    ZST_WEBRTC_CODEC_VP8   = 1,
    ZST_WEBRTC_CODEC_VP9   = 2,
    ZST_WEBRTC_CODEC_H265  = 3,
    ZST_WEBRTC_CODEC_AV1   = 4,
    ZST_WEBRTC_CODEC_OPUS  = 128,
    ZST_WEBRTC_CODEC_PCMU  = 129,
    ZST_WEBRTC_CODEC_PCMA  = 130,
    ZST_WEBRTC_CODEC_AAC   = 131,
} zst_webrtc_codec_t;

/**
 * Add an outbound media track to the PeerConnection.
 * Must be called BEFORE zst_webrtc_create_offer() so the track
 * is included in the SDP offer.
 *
 * @param el       The webrtc_endpoint element.
 * @param codec    The codec for this track.
 * @param ssrc     The RTP synchronization source identifier.
 * @param mid      Media identifier string (e.g., "video0"). If NULL, defaults to "video".
 * @return ZST_OK on success.
 */
zst_result_t zst_webrtc_add_video_track(
    zst_element_t* el,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid);

/**
 * Add an outbound audio track to the PeerConnection.
 * Must be called BEFORE zst_webrtc_create_offer().
 *
 * @param el       The webrtc_endpoint element.
 * @param codec    The codec for this track (e.g., ZST_WEBRTC_CODEC_OPUS).
 * @param ssrc     The RTP synchronization source identifier.
 * @param mid      Media identifier string (e.g., "audio0"). If NULL, defaults to "audio".
 * @return ZST_OK on success.
 */
zst_result_t zst_webrtc_add_audio_track(
    zst_element_t* el,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid);

/**
 * Send raw encoded media on a track.
 * The track must have been added via zst_webrtc_add_{video,audio}_track.
 * Data is RTP-packetized by the internal packetizer and sent to the remote peer.
 *
 * @param el           The webrtc_endpoint element.
 * @param track_index  Index of the track (0-based, order of zst_webrtc_add_* calls).
 * @param data         Raw encoded frame data (e.g., H.264 NALUs, Opus packets).
 * @param size         Size of the data in bytes.
 * @return ZST_OK on success.
 */
zst_result_t zst_webrtc_send_media(
    zst_element_t* el,
    uint32_t track_index,
    const void* data,
    int size);

#ifdef __cplusplus
}
#endif
