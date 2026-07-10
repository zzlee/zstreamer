/*=============================================================================
    webrtc_endpoint.c — WebRTC endpoint element

    Unified WebRTC element wrapping libdatachannel (when available).
    Accepts encoded media on dynamic sink pads and outputs received
    encoded media on dynamically-created source pads.

    Phase 2 scope: real PeerConnection lifecycle, signaling, media send/recv.

    References:
      - libdatachannel C API: https://github.com/paullouisageneau/libdatachannel
      - RFC 8825 (WebRTC)
      - RFC 8826 (WebRTC Security)
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "zst_element.h"
#include "zst_bus.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_log.h"

/* ── When libdatachannel is available, pull in the C API ─────────────────── */
#ifdef HAS_WEBRTC
#include <rtc/rtc.h>
#endif

/*════════════════════════════════════════════════════════════════════════════
  Element private state
════════════════════════════════════════════════════════════════════════════*/
/* Forward declaration for the track struct */
typedef struct {
    zst_webrtc_codec_t codec;
    uint32_t           ssrc;
    uint8_t            payload_type;
    uint32_t           clock_rate;
    char               mid[32];
    int                track_id;   /* libdatachannel track handle */
    bool               active;
} webrtc_track_t;

/* Received track info (Phase 4) */
typedef struct {
    int                rtc_id;      /* libdatachannel track handle */
    zst_webrtc_codec_t codec;
    char               mid[32];
    zst_pad_t*         src_pad;     /* dynamically created source pad */
    bool               active;
} webrtc_recv_track_t;

typedef struct {
    /* ── ICE configuration ──────────────────────────────────────────────── */
    char**   stun_urls;
    uint32_t num_stun_urls;
    char**   turn_urls;
    uint32_t num_turn_urls;

    /* ── Connection state ───────────────────────────────────────────────── */
    bool     enable_data_channels;
    bool     negotiated;

    /* ── Local/remote SDP (owned strings) ───────────────────────────────── */
    char*    local_sdp;
    char*    local_sdp_type;  /* "offer" or "answer" */
    char*    remote_sdp;

    /* ── State strings ──────────────────────────────────────────────────── */
    char     ice_state[32];
    char     dtls_state[32];
    char     sctp_state[32];
    char     signalling_state[32];

    /* ── Signaling lock — serializes set_remote_description / add_ice_candidate ─ */
    pthread_mutex_t signaling_lock;

#ifdef HAS_WEBRTC
    /* ── libdatachannel objects ─────────────────────────────────────────── */
    int      pc_id;           /* PeerConnection handle (-1 if not created) */
    bool     pc_created;
    bool     pending_local_offer; /* true = element will generate an offer */
#endif

    /* ── Back-pointer to element (for posting events from callbacks) ────── */
    zst_element_t* el;

    /* ── Media tracks (Phase 3: outbound, Phase 4: inbound) ────────────── */
    #define MAX_TRACKS 4
    webrtc_track_t tracks[MAX_TRACKS];
    uint32_t num_tracks;

    /* ── Inbound tracks (Phase 4) ─────────────────────────────────────── */
    webrtc_recv_track_t recv_tracks[MAX_TRACKS];
    uint32_t num_recv_tracks;

    /* ── Inbound callback ──────────────────────────────────────────────── */
    zst_webrtc_on_track_fn on_track_fn;
    void*                  on_track_user_data;
} webrtc_endpoint_t;

/*════════════════════════════════════════════════════════════════════════════
  libdatachannel callbacks (Phase 2 — only compiled with HAS_WEBRTC)
════════════════════════════════════════════════════════════════════════════*/
#ifdef HAS_WEBRTC

/* Helper: map rtcIceState to a string */
static const char*
ice_state_str(rtcIceState state)
{
    switch (state) {
    case RTC_ICE_NEW:          return "new";
    case RTC_ICE_CHECKING:     return "checking";
    case RTC_ICE_CONNECTED:    return "connected";
    case RTC_ICE_COMPLETED:    return "completed";
    case RTC_ICE_FAILED:       return "failed";
    case RTC_ICE_DISCONNECTED: return "disconnected";
    case RTC_ICE_CLOSED:       return "closed";
    default:                   return "unknown";
    }
}

/* Helper: map rtcState to a string */
static const char*
dtls_state_str(rtcState state)
{
    switch (state) {
    case RTC_NEW:          return "new";
    case RTC_CONNECTING:   return "connecting";
    case RTC_CONNECTED:    return "connected";
    case RTC_DISCONNECTED: return "disconnected";
    case RTC_FAILED:       return "failed";
    case RTC_CLOSED:       return "closed";
    default:               return "unknown";
    }
}

/* Helper: map rtcSignalingState to a string */
static const char*
signaling_state_str(rtcSignalingState state)
{
    switch (state) {
    case RTC_SIGNALING_STABLE:            return "stable";
    case RTC_SIGNALING_HAVE_LOCAL_OFFER:  return "have-local-offer";
    case RTC_SIGNALING_HAVE_REMOTE_OFFER: return "have-remote-offer";
    case RTC_SIGNALING_HAVE_LOCAL_PRANSWER:  return "have-local-pranswer";
    case RTC_SIGNALING_HAVE_REMOTE_PRANSWER: return "have-remote-pranswer";
    default: return "unknown";
    }
}

/* ── Local SDP callback ──────────────────────────────────────────────────── */
static void
on_local_description(int pc, const char* sdp, const char* type, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s || !sdp || !type) return;

    pthread_mutex_lock(&s->signaling_lock);
    free(s->local_sdp);
    s->local_sdp = strdup(sdp);
    free(s->local_sdp_type);
    s->local_sdp_type = strdup(type);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint",
                 "on_local_description: type=%s, len=%zu",
                 type, strlen(sdp));

    /* Post an event to the element's bus so the application can retrieve it */
    if (s->el && s->el->bus) {
        zst_event_t* ev = zst_event_new_webrtc_local_description(
            s->el, type, sdp);
        if (ev) {
            zst_bus_post(s->el->bus, ev);
        }
    }
}

/* ── Local ICE candidate callback ────────────────────────────────────────── */
static void
on_local_candidate(int pc, const char* cand, const char* mid, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s || !cand) return;

    ZST_LOG_DEBUG("webrtc_endpoint",
                  "on_local_candidate: mid=%s, cand=%s",
                  mid ? mid : "(null)", cand);

    /* Post an event to the element's bus so the application can forward it */
    if (s->el && s->el->bus) {
        zst_event_t* ev = zst_event_new_webrtc_ice_candidate(
            s->el, mid ? mid : "", 0, cand);
        if (ev) {
            zst_bus_post(s->el->bus, ev);
        }
    }
}

/* ── ICE state change callback ───────────────────────────────────────────── */
static void
on_ice_state_change(int pc, rtcIceState state, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    const char* str = ice_state_str(state);
    pthread_mutex_lock(&s->signaling_lock);
    snprintf(s->ice_state, sizeof(s->ice_state), "%s", str);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint", "ice_state: %s", str);
}

/* ── DTLS / connection state change callback ─────────────────────────────── */
static void
on_state_change(int pc, rtcState state, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    const char* str = dtls_state_str(state);
    pthread_mutex_lock(&s->signaling_lock);
    snprintf(s->dtls_state, sizeof(s->dtls_state), "%s", str);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint", "dtls_state: %s", str);

    if (state == RTC_CONNECTED) {
        pthread_mutex_lock(&s->signaling_lock);
        s->negotiated = true;
        snprintf(s->sctp_state, sizeof(s->sctp_state), "connected");
        pthread_mutex_unlock(&s->signaling_lock);
    }
}

/* ── Signaling state change callback ─────────────────────────────────────── */
static void
on_signaling_state_change(int pc, rtcSignalingState state, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    const char* str = signaling_state_str(state);
    pthread_mutex_lock(&s->signaling_lock);
    snprintf(s->signalling_state, sizeof(s->signalling_state), "%s", str);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint", "signaling_state: %s", str);
}

/* ═══════════════════════════════════════════════════════════════════════
   Forward declarations for codec helpers (defined in Phase 3 section)
   ═══════════════════════════════════════════════════════════════════════ */
static rtcCodec      codec_to_rtc(zst_webrtc_codec_t codec);
static uint32_t      codec_clock_rate(zst_webrtc_codec_t codec);
static uint8_t       codec_default_payload_type(zst_webrtc_codec_t codec);
static const char*   codec_name(zst_webrtc_codec_t codec);

/* ── Destroy callback for dynamically allocated receive buffers ──────── */
static void
recv_buf_destroy(zst_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

/* ── Helper: detect codec from track SDP description ─────────────────── */
static zst_webrtc_codec_t
codec_from_track_sdp(const char* sdp)
{
    if (!sdp) return ZST_WEBRTC_CODEC_H264;
    if (strstr(sdp, "H264")  || strstr(sdp, "h264"))  return ZST_WEBRTC_CODEC_H264;
    if (strstr(sdp, "VP8")   || strstr(sdp, "vp8"))   return ZST_WEBRTC_CODEC_VP8;
    if (strstr(sdp, "VP9")   || strstr(sdp, "vp9"))   return ZST_WEBRTC_CODEC_VP9;
    if (strstr(sdp, "H265")  || strstr(sdp, "h265"))  return ZST_WEBRTC_CODEC_H265;
    if (strstr(sdp, "AV1")   || strstr(sdp, "av1"))   return ZST_WEBRTC_CODEC_AV1;
    if (strstr(sdp, "opus")  || strstr(sdp, "OPUS"))  return ZST_WEBRTC_CODEC_OPUS;
    if (strstr(sdp, "PCMU")  || strstr(sdp, "pcmu"))  return ZST_WEBRTC_CODEC_PCMU;
    if (strstr(sdp, "PCMA")  || strstr(sdp, "pcma"))  return ZST_WEBRTC_CODEC_PCMA;
    if (strstr(sdp, "MP4A")  || strstr(sdp, "mp4a"))  return ZST_WEBRTC_CODEC_AAC;
    return ZST_WEBRTC_CODEC_H264; /* default */
}

/* ── Frame callback — receives decoded frames from libdatachannel ─────── */
static void
on_frame(int tr, const char* data, int size, const rtcFrameInfo* info, void* ptr)
{
    (void)ptr;
    webrtc_endpoint_t* s = rtcGetUserPointer(tr);
    if (!s || !data || size <= 0) return;

    /* Find the recv_track entry for this track id */
    for (uint32_t i = 0; i < s->num_recv_tracks; i++) {
        if (s->recv_tracks[i].rtc_id == tr && s->recv_tracks[i].active) {
            zst_pad_t* pad = s->recv_tracks[i].src_pad;
            if (!pad) return;

            /* Create a buffer with the decoded data */
            zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
            if (!buf) return;

            /* Allocate memory for the frame data */
            buf->memory.type = ZST_MEMORY_CPU;
            buf->memory.size = (size_t)size;
            buf->memory.data = malloc((size_t)size);
            if (!buf->memory.data) {
                zst_buffer_unref(buf);
                return;
            }
            memcpy(buf->memory.data, data, (size_t)size);
            buf->destroy = recv_buf_destroy;

            /* Set PTS from RTP timestamp */
            if (info && info->timestampSeconds >= 0) {
                buf->pts = (zst_time_t)(info->timestampSeconds * 1000000000.0);
            } else if (info && info->timestamp > 0) {
                /* Convert RTP timestamp (typically 90kHz for video) to ns */
                uint32_t clock = codec_clock_rate(s->recv_tracks[i].codec);
                if (clock > 0)
                    buf->pts = (zst_time_t)((uint64_t)info->timestamp * 1000000000ULL / clock);
            }
            buf->dts = buf->pts;
            buf->flags = 0;

            /* Push to downstream element */
            zst_pad_push(pad, buf);
            return;
        }
    }
}

/* ── Remote track callback ───────────────────────────────────────────────── */
static void
on_track(int pc, int tr, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    if (s->num_recv_tracks >= MAX_TRACKS) {
        ZST_LOG_WARN("webrtc_endpoint", "on_track: max tracks reached, ignoring track %d", tr);
        return;
    }

    /* Get the track's SDP to detect codec */
    char sdp_buf[1024] = {0};
    rtcGetTrackDescription(tr, sdp_buf, sizeof(sdp_buf));

    /* Extract mid from the SDP (a=mid: line) */
    char mid[32] = {0};
    rtcGetTrackMid(tr, mid, sizeof(mid));

    zst_webrtc_codec_t codec = codec_from_track_sdp(sdp_buf);

    /* Store the received track info */
    webrtc_recv_track_t* rt = &s->recv_tracks[s->num_recv_tracks];
    rt->rtc_id = tr;
    rt->codec = codec;
    snprintf(rt->mid, sizeof(rt->mid), "%s", mid[0] ? mid : "recv");
    rt->active = true;

    /* Create a source pad for this track */
    char pad_name[64];
    snprintf(pad_name, sizeof(pad_name), "src_%u", s->num_recv_tracks);
    zst_pad_t* src_pad = zst_pad_create(pad_name, ZST_PAD_SRC);
    if (src_pad) {
        rt->src_pad = src_pad;
        if (zst_element_add_pad(s->el, src_pad) != ZST_OK) {
            ZST_LOG_ERROR("webrtc_endpoint", "on_track: failed to add source pad %s", pad_name);
            zst_pad_destroy(src_pad);
            rt->src_pad = NULL;
        }
    }

    /* Set up frame callback to receive decoded media */
    rtcSetUserPointer(tr, s);
    rtcSetFrameCallback(tr, on_frame);

    s->num_recv_tracks++;

    ZST_LOG_INFO("webrtc_endpoint", "on_track: recv track %d, codec=%s, mid=%s, pad=%s",
                 tr, codec_name(codec), rt->mid, pad_name ? pad_name : "none");

    /* Fire user callback */
    if (s->on_track_fn) {
        s->on_track_fn(s->el, tr, codec, rt->mid, s->on_track_user_data);
    }

    /* Post a pad-added event (must be heap-allocated for the bus) */
    if (s->el && s->el->bus && src_pad) {
        zst_event_t* ev = calloc(1, sizeof(*ev));
        if (ev) {
            ev->type = ZST_EVENT_PAD_ADDED;
            ev->src = s->el;
            ev->as.pad_added.pad = zst_pad_ref(src_pad);
            zst_bus_post(s->el->bus, ev);
        }
    }
}

/* ── Data channel callback ───────────────────────────────────────────────── */
static void
on_data_channel(int pc, int dc, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    ZST_LOG_INFO("webrtc_endpoint", "on_data_channel: dc_id=%d (Phase 5 — not yet implemented)", dc);
}

#endif /* HAS_WEBRTC */

/*════════════════════════════════════════════════════════════════════════════
  Element ops callbacks
════════════════════════════════════════════════════════════════════════════*/

static zst_result_t
webrtc_open(zst_element_t* el)
{
    webrtc_endpoint_t* s = el->priv;

    ZST_LOG_INFO("webrtc_endpoint", "open: initializing WebRTC endpoint");

    /* Initialize state strings */
    snprintf(s->ice_state,      sizeof(s->ice_state),      "new");
    snprintf(s->dtls_state,     sizeof(s->dtls_state),     "new");
    snprintf(s->sctp_state,     sizeof(s->sctp_state),     "new");
    snprintf(s->signalling_state, sizeof(s->signalling_state), "stable");

    s->negotiated = false;

#ifdef HAS_WEBRTC
    /* ── Create the PeerConnection ────────────────────────────────────── */
    /* Build the ICE server configuration */
    rtcConfiguration config = {0};

    /* Combine STUN + TURN into a single iceServers array for libdatachannel */
    uint32_t total_servers = s->num_stun_urls + s->num_turn_urls;
    const char** ice_servers = NULL;
    if (total_servers > 0) {
        ice_servers = calloc(total_servers, sizeof(char*));
        if (!ice_servers) {
            ZST_LOG_ERROR("webrtc_endpoint", "open: failed to allocate ICE server array");
            return ZST_ERROR;
        }
        for (uint32_t i = 0; i < s->num_stun_urls; i++) {
            ice_servers[i] = s->stun_urls[i];
        }
        for (uint32_t i = 0; i < s->num_turn_urls; i++) {
            ice_servers[s->num_stun_urls + i] = s->turn_urls[i];
        }
        config.iceServers = ice_servers;
        config.iceServersCount = (int)total_servers;
    }

    s->pc_id = rtcCreatePeerConnection(&config);
    free(ice_servers);

    if (s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "open: rtcCreatePeerConnection failed (%d)", s->pc_id);
        return ZST_ERROR;
    }

    s->pc_created = true;

    /* Store ourselves as user pointer so callbacks can find us */
    rtcSetUserPointer(s->pc_id, s);

    /* Register callbacks */
    rtcSetLocalDescriptionCallback(s->pc_id, on_local_description);
    rtcSetLocalCandidateCallback(s->pc_id, on_local_candidate);
    rtcSetStateChangeCallback(s->pc_id, on_state_change);
    rtcSetIceStateChangeCallback(s->pc_id, on_ice_state_change);
    rtcSetSignalingStateChangeCallback(s->pc_id, on_signaling_state_change);
    rtcSetTrackCallback(s->pc_id, on_track);
    rtcSetDataChannelCallback(s->pc_id, on_data_channel);

    ZST_LOG_INFO("webrtc_endpoint",
                 "open: PeerConnection created (id=%d, %u STUN + %u TURN servers)",
                 s->pc_id, s->num_stun_urls, s->num_turn_urls);
#else
    ZST_LOG_INFO("webrtc_endpoint", "open: WebRTC stub (HAS_WEBRTC not defined)");
#endif

    return ZST_OK;
}

static zst_result_t
webrtc_process(
    zst_element_t* el,
    zst_buffer_t* in,
    zst_buffer_t** out)
{
    (void)out;

    if (!in)
        return ZST_OK;

    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    /* Forward the buffer to the first active video track */
    if (s->num_tracks > 0 && in->memory.size > 0) {
        /* Try to find a track that matches the buffer content.
           For simplicity, send to the first active track.
           A future enhancement could use caps to route to specific tracks. */
        for (uint32_t i = 0; i < s->num_tracks; i++) {
            if (s->tracks[i].active && s->tracks[i].track_id >= 0) {
                int ret = rtcSendMessage(s->tracks[i].track_id,
                                        (const char*)in->memory.data,
                                        (int)in->memory.size);
                if (ret != RTC_ERR_SUCCESS) {
                    ZST_LOG_WARN("webrtc_endpoint",
                                 "process: rtcSendMessage failed (%d) on track %u",
                                 ret, i);
                } else {
                    ZST_LOG_DEBUG("webrtc_endpoint",
                                  "process: forwarded %zu bytes to track %u",
                                  in->memory.size, i);
                }
                break;  /* Send to first active track only */
            }
        }
    } else {
        ZST_LOG_DEBUG("webrtc_endpoint",
                      "process: buffer of %zu bytes (no active tracks)",
                      in->memory.size);
    }
#else
    ZST_LOG_DEBUG("webrtc_endpoint",
                  "process: received buffer of %zu bytes (no HAS_WEBRTC)",
                  in->memory.size);
#endif

    return ZST_OK;
}

static zst_result_t
webrtc_close(zst_element_t* el)
{
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (s->pc_created && s->pc_id >= 0) {
        rtcClosePeerConnection(s->pc_id);
        rtcDeletePeerConnection(s->pc_id);
        s->pc_id = -1;
        s->pc_created = false;
        ZST_LOG_INFO("webrtc_endpoint", "close: PeerConnection destroyed");
    }
#endif

    /* Clean up received track source pads (Phase 4) */
    for (uint32_t i = 0; i < s->num_recv_tracks; i++) {
        if (s->recv_tracks[i].src_pad) {
            zst_pad_destroy(s->recv_tracks[i].src_pad);
            s->recv_tracks[i].src_pad = NULL;
        }
        s->recv_tracks[i].active = false;
    }
    s->num_recv_tracks = 0;

    pthread_mutex_destroy(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint", "close: WebRTC endpoint torn down");
    return ZST_OK;
}

static zst_result_t
webrtc_set_property(
    zst_element_t* el,
    const char* name,
    const char* value)
{
    webrtc_endpoint_t* s = el->priv;

    if (strcmp(name, "stun-servers") == 0) {
        /* Value is a comma-separated list of STUN URLs */
        free(s->stun_urls);
        s->stun_urls = NULL;
        s->num_stun_urls = 0;

        if (value && value[0]) {
            /* Count URLs */
            uint32_t count = 1;
            for (const char* p = value; *p; p++) {
                if (*p == ',') count++;
            }
            s->stun_urls = calloc(count, sizeof(char*));
            if (!s->stun_urls) return ZST_ERROR;

            char* buf = strdup(value);
            if (!buf) { free(s->stun_urls); s->stun_urls = NULL; return ZST_ERROR; }

            uint32_t idx = 0;
            char* saveptr = NULL;
            char* token = strtok_r(buf, ",", &saveptr);
            while (token && idx < count) {
                /* Trim leading whitespace */
                while (*token == ' ' || *token == '\t') token++;
                s->stun_urls[idx++] = strdup(token);
                token = strtok_r(NULL, ",", &saveptr);
            }
            s->num_stun_urls = idx;
            free(buf);
        }
        return ZST_OK;
    }

    if (strcmp(name, "turn-servers") == 0) {
        free(s->turn_urls);
        s->turn_urls = NULL;
        s->num_turn_urls = 0;

        if (value && value[0]) {
            uint32_t count = 1;
            for (const char* p = value; *p; p++) {
                if (*p == ',') count++;
            }
            s->turn_urls = calloc(count, sizeof(char*));
            if (!s->turn_urls) return ZST_ERROR;

            char* buf = strdup(value);
            if (!buf) { free(s->turn_urls); s->turn_urls = NULL; return ZST_ERROR; }

            uint32_t idx = 0;
            char* saveptr = NULL;
            char* token = strtok_r(buf, ",", &saveptr);
            while (token && idx < count) {
                while (*token == ' ' || *token == '\t') token++;
                s->turn_urls[idx++] = strdup(token);
                token = strtok_r(NULL, ",", &saveptr);
            }
            s->num_turn_urls = idx;
            free(buf);
        }
        return ZST_OK;
    }

    if (strcmp(name, "ice-urls") == 0) {
        /* Convenience: set both STUN and TURN URLs from a comma-separated list.
           URLs containing "turn:" or "turns:" go to turn-servers; others to stun-servers. */
        char* buf = value ? strdup(value) : NULL;
        if (!buf || !buf[0]) {
            free(buf);
            free(s->stun_urls); s->stun_urls = NULL; s->num_stun_urls = 0;
            free(s->turn_urls); s->turn_urls = NULL; s->num_turn_urls = 0;
            return ZST_OK;
        }

        uint32_t total = 1;
        for (const char* p = buf; *p; p++) { if (*p == ',') total++; }

        char** stun_tmp = calloc(total, sizeof(char*));
        char** turn_tmp = calloc(total, sizeof(char*));
        uint32_t n_stun = 0, n_turn = 0;

        char* saveptr = NULL;
        char* token = strtok_r(buf, ",", &saveptr);
        while (token) {
            while (*token == ' ' || *token == '\t') token++;
            if (strncmp(token, "turn:", 5) == 0 || strncmp(token, "turns:", 6) == 0) {
                turn_tmp[n_turn++] = strdup(token);
            } else {
                stun_tmp[n_stun++] = strdup(token);
            }
            token = strtok_r(NULL, ",", &saveptr);
        }

        free(s->stun_urls);
        s->stun_urls = stun_tmp;
        s->num_stun_urls = n_stun;
        free(s->turn_urls);
        s->turn_urls = turn_tmp;
        s->num_turn_urls = n_turn;
        free(buf);
        return ZST_OK;
    }

    if (strcmp(name, "remote-sdp") == 0) {
        free(s->remote_sdp);
        s->remote_sdp = value ? strdup(value) : NULL;
        ZST_LOG_INFO("webrtc_endpoint", "set_property: remote-sdp set (%zu bytes)",
                     value ? strlen(value) : 0);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
webrtc_get_property(
    zst_element_t* el,
    const char* name,
    char* value_out,
    size_t max_len)
{
    webrtc_endpoint_t* s = el->priv;

    pthread_mutex_lock(&s->signaling_lock);

    if (strcmp(name, "stun-servers") == 0) {
        value_out[0] = '\0';
        for (uint32_t i = 0; i < s->num_stun_urls; i++) {
            if (i > 0) strncat(value_out, ",", max_len - strlen(value_out) - 1);
            strncat(value_out, s->stun_urls[i], max_len - strlen(value_out) - 1);
        }
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "turn-servers") == 0) {
        value_out[0] = '\0';
        for (uint32_t i = 0; i < s->num_turn_urls; i++) {
            if (i > 0) strncat(value_out, ",", max_len - strlen(value_out) - 1);
            strncat(value_out, s->turn_urls[i], max_len - strlen(value_out) - 1);
        }
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "ice-state") == 0) {
        snprintf(value_out, max_len, "%s", s->ice_state);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "dtls-state") == 0) {
        snprintf(value_out, max_len, "%s", s->dtls_state);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "sctp-state") == 0) {
        snprintf(value_out, max_len, "%s", s->sctp_state);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "signalling-state") == 0) {
        snprintf(value_out, max_len, "%s", s->signalling_state);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "negotiated") == 0) {
        snprintf(value_out, max_len, "%s", s->negotiated ? "true" : "false");
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "local-sdp") == 0) {
        if (s->local_sdp) {
            snprintf(value_out, max_len, "%s", s->local_sdp);
        } else {
            value_out[0] = '\0';
        }
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "remote-sdp") == 0) {
        if (s->remote_sdp) {
            snprintf(value_out, max_len, "%s", s->remote_sdp);
        } else {
            value_out[0] = '\0';
        }
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    pthread_mutex_unlock(&s->signaling_lock);
    return ZST_ERROR;
}

/*════════════════════════════════════════════════════════════════════════════
  Element ops table
════════════════════════════════════════════════════════════════════════════*/
static zst_element_ops_t g_ops = {
    .name         = "webrtc_endpoint",
    .open         = webrtc_open,
    .process      = webrtc_process,
    .close        = webrtc_close,
    .set_property = webrtc_set_property,
    .get_property = webrtc_get_property,
};

/*════════════════════════════════════════════════════════════════════════════
  Element constructors
════════════════════════════════════════════════════════════════════════════*/
zst_element_t*
zst_webrtc_endpoint_create(void)
{
    webrtc_endpoint_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->ice_state,      sizeof(priv->ice_state),      "new");
    snprintf(priv->dtls_state,     sizeof(priv->dtls_state),     "new");
    snprintf(priv->sctp_state,     sizeof(priv->sctp_state),     "new");
    snprintf(priv->signalling_state, sizeof(priv->signalling_state), "stable");

    pthread_mutex_init(&priv->signaling_lock, NULL);

#ifdef HAS_WEBRTC
    priv->pc_id = -1;
#endif

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        pthread_mutex_destroy(&priv->signaling_lock);
        free(priv);
        return NULL;
    }

    priv->el = el;

    /*
     * Phase 1: fixed pads — "sink" (encoded video) and "src" (encoded video).
     * Phase 3: switch to dynamic pad templates (sink_%u, src_%u) to support
     *          multiple tracks via request pads.
     */
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(el, sink);

    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}

zst_element_t*
zst_webrtc_endpoint_create_with_config(
    const zst_webrtc_endpoint_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_webrtc_endpoint_config_t))
        return NULL;

    zst_element_t* el = zst_element_factory_make("webrtc_endpoint");
    if (!el) return NULL;

    webrtc_endpoint_t* s = el->priv;

    s->enable_data_channels = config->enable_data_channels;

    /* Apply ICE servers */
    for (uint32_t i = 0; i < config->num_ice_servers; i++) {
        const zst_webrtc_ice_server_t* srv = &config->ice_servers[i];
        if (!srv->url) continue;

        if (strncmp(srv->url, "turn:", 5) == 0 || strncmp(srv->url, "turns:", 6) == 0) {
            uint32_t n = s->num_turn_urls;
            s->turn_urls = realloc(s->turn_urls, (n + 1) * sizeof(char*));
            s->turn_urls[n] = strdup(srv->url);
            s->num_turn_urls = n + 1;
        } else {
            uint32_t n = s->num_stun_urls;
            s->stun_urls = realloc(s->stun_urls, (n + 1) * sizeof(char*));
            s->stun_urls[n] = strdup(srv->url);
            s->num_stun_urls = n + 1;
        }
    }

    ZST_LOG_INFO("webrtc_endpoint",
                 "create_with_config: %u STUN + %u TURN servers, data_channels=%s",
                 s->num_stun_urls, s->num_turn_urls,
                 s->enable_data_channels ? "true" : "false");

    return el;
}

/*════════════════════════════════════════════════════════════════════════════
  Signaling API (Phase 2 — real implementation)
════════════════════════════════════════════════════════════════════════════*/
zst_result_t
zst_webrtc_create_offer(zst_element_t* el)
{
    if (!el) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "create_offer: PeerConnection not created");
        return ZST_ERROR;
    }

    pthread_mutex_lock(&s->signaling_lock);
    snprintf(s->signalling_state, sizeof(s->signalling_state), "have-local-offer");
    pthread_mutex_unlock(&s->signaling_lock);

    /* Generate the SDP offer — the on_local_description callback will fire */
    int ret = rtcSetLocalDescription(s->pc_id, "offer");
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint", "create_offer: rtcSetLocalDescription failed (%d)", ret);
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "create_offer: SDP offer generation started");
    return ZST_OK;
#else
    ZST_LOG_INFO("webrtc_endpoint", "create_offer: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
}

zst_result_t
zst_webrtc_set_remote_description(
    zst_element_t* el, const char* type, const char* sdp)
{
    if (!el || !type || !sdp) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

    pthread_mutex_lock(&s->signaling_lock);
    free(s->remote_sdp);
    s->remote_sdp = strdup(sdp);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint",
                 "set_remote_description: type=%s, sdp_len=%zu",
                 type, strlen(sdp));

#ifdef HAS_WEBRTC
    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "set_remote_description: PeerConnection not created");
        return ZST_ERROR;
    }

    int ret = rtcSetRemoteDescription(s->pc_id, sdp, type);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "set_remote_description: rtcSetRemoteDescription failed (%d)", ret);
        return ZST_ERROR;
    }

    /*
     * If this was an offer from the remote peer, libdatachannel will
     * automatically generate the answer and fire on_local_description.
     * We do NOT need to call rtcSetLocalDescription("answer") again.
     */
#endif

    return ZST_OK;
}

zst_result_t
zst_webrtc_add_ice_candidate(
    zst_element_t* el, const char* mid, int mlineindex, const char* candidate)
{
    if (!el || !mid || !candidate) return ZST_ERROR;

    (void)mlineindex;

    ZST_LOG_DEBUG("webrtc_endpoint",
                  "add_ice_candidate: mid=%s, candidate=%s",
                  mid, candidate);

#ifdef HAS_WEBRTC
    webrtc_endpoint_t* s = el->priv;

    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "add_ice_candidate: PeerConnection not created");
        return ZST_ERROR;
    }

    /* libdatachannel uses rtcAddRemoteCandidate(pc, candidate, mid) */
    int ret = rtcAddRemoteCandidate(s->pc_id, candidate, mid);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "add_ice_candidate: rtcAddRemoteCandidate failed (%d)", ret);
        return ZST_ERROR;
    }
#endif

    return ZST_OK;
}

zst_result_t
zst_webrtc_create_data_channel(
    zst_element_t* el, const char* label)
{
    if (!el || !label) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "create_data_channel: PeerConnection not created");
        return ZST_ERROR;
    }

    int dc_id = rtcCreateDataChannel(s->pc_id, label);
    if (dc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "create_data_channel: rtcCreateDataChannel failed (%d)", dc_id);
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "create_data_channel: label=%s, dc_id=%d", label, dc_id);
    return ZST_OK;
#else
    (void)s;
    ZST_LOG_INFO("webrtc_endpoint", "create_data_channel: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
}

/*════════════════════════════════════════════════════════════════════════════
  Media Track API (Phase 3)
════════════════════════════════════════════════════════════════════════════*/

/* Helper: map zst_webrtc_codec_t to libdatachannel rtcCodec */
#ifdef HAS_WEBRTC
static rtcCodec
codec_to_rtc(zst_webrtc_codec_t codec)
{
    switch (codec) {
    case ZST_WEBRTC_CODEC_H264: return RTC_CODEC_H264;
    case ZST_WEBRTC_CODEC_VP8:  return RTC_CODEC_VP8;
    case ZST_WEBRTC_CODEC_VP9:  return RTC_CODEC_VP9;
    case ZST_WEBRTC_CODEC_H265: return RTC_CODEC_H265;
    case ZST_WEBRTC_CODEC_AV1:  return RTC_CODEC_AV1;
    case ZST_WEBRTC_CODEC_OPUS: return RTC_CODEC_OPUS;
    case ZST_WEBRTC_CODEC_PCMU: return RTC_CODEC_PCMU;
    case ZST_WEBRTC_CODEC_PCMA: return RTC_CODEC_PCMA;
    case ZST_WEBRTC_CODEC_AAC:  return RTC_CODEC_AAC;
    default:                    return RTC_CODEC_H264;
    }
}

static uint32_t
codec_clock_rate(zst_webrtc_codec_t codec)
{
    switch (codec) {
    case ZST_WEBRTC_CODEC_H264:
    case ZST_WEBRTC_CODEC_VP8:
    case ZST_WEBRTC_CODEC_VP9:
    case ZST_WEBRTC_CODEC_H265:
    case ZST_WEBRTC_CODEC_AV1:  return 90000;
    case ZST_WEBRTC_CODEC_OPUS:
    case ZST_WEBRTC_CODEC_AAC:  return 48000;
    case ZST_WEBRTC_CODEC_PCMU:
    case ZST_WEBRTC_CODEC_PCMA: return 8000;
    default:                    return 90000;
    }
}

static uint8_t
codec_default_payload_type(zst_webrtc_codec_t codec)
{
    switch (codec) {
    case ZST_WEBRTC_CODEC_H264: return 96;
    case ZST_WEBRTC_CODEC_VP8:  return 96;
    case ZST_WEBRTC_CODEC_VP9:  return 96;
    case ZST_WEBRTC_CODEC_H265: return 96;
    case ZST_WEBRTC_CODEC_AV1:  return 96;
    case ZST_WEBRTC_CODEC_OPUS: return 111;
    case ZST_WEBRTC_CODEC_PCMU: return 0;
    case ZST_WEBRTC_CODEC_PCMA: return 8;
    case ZST_WEBRTC_CODEC_AAC:  return 96;
    default:                    return 96;
    }
}

static const char*
codec_name(zst_webrtc_codec_t codec)
{
    switch (codec) {
    case ZST_WEBRTC_CODEC_H264: return "H264";
    case ZST_WEBRTC_CODEC_VP8:  return "VP8";
    case ZST_WEBRTC_CODEC_VP9:  return "VP9";
    case ZST_WEBRTC_CODEC_H265: return "H265";
    case ZST_WEBRTC_CODEC_AV1:  return "AV1";
    case ZST_WEBRTC_CODEC_OPUS: return "Opus";
    case ZST_WEBRTC_CODEC_PCMU: return "PCMU";
    case ZST_WEBRTC_CODEC_PCMA: return "PCMA";
    case ZST_WEBRTC_CODEC_AAC:  return "AAC";
    default:                    return "unknown";
    }
}

static int
add_track_internal(
    webrtc_endpoint_t* s,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid,
    bool is_audio)
{
    if (s->num_tracks >= MAX_TRACKS) {
        ZST_LOG_ERROR("webrtc_endpoint", "add_track: max tracks (%d) reached", MAX_TRACKS);
        return -1;
    }

    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "add_track: PeerConnection not created");
        return -1;
    }

    uint32_t idx = s->num_tracks;
    uint8_t pt = codec_default_payload_type(codec);
    uint32_t rate = codec_clock_rate(codec);
    const char* mid_str = mid ? mid : (is_audio ? "audio" : "video");

    /* Build the rtcTrackInit */
    rtcTrackInit tinit = {0};
    tinit.direction = RTC_DIRECTION_SENDONLY;
    tinit.codec     = codec_to_rtc(codec);
    tinit.payloadType = pt;
    tinit.ssrc      = ssrc;
    tinit.mid       = mid_str;
    tinit.name      = "zstreamer";
    tinit.msid      = "stream0";

    int tr = rtcAddTrackEx(s->pc_id, &tinit);
    if (tr < 0) {
        /* Fallback: try rtcAddTrack with the SDP string directly */
        ZST_LOG_ERROR("webrtc_endpoint",
                      "add_track: rtcAddTrack failed (%d), trying SDP fallback", tr);
        /* Build a minimal SDP m-line */
        char sdp_buf[512];
        if (is_audio) {
            snprintf(sdp_buf, sizeof(sdp_buf),
                     "m=audio 9 UDP/TLS/RTP/SAVPF %d\n"
                     "c=IN IP4 0.0.0.0\n"
                     "a=mid:%s\n"
                     "a=sendonly\n"
                     "a=rtpmap:%d opus/48000/2\n"
                     "a=ssrc:%u cname:zstreamer\n",
                     pt, mid_str, pt, ssrc);
        } else {
            snprintf(sdp_buf, sizeof(sdp_buf),
                     "m=video 9 UDP/TLS/RTP/SAVPF %d\n"
                     "c=IN IP4 0.0.0.0\n"
                     "a=mid:%s\n"
                     "a=sendonly\n"
                     "a=rtpmap:%d H264/90000\n"
                     "a=fmtp:%d packetization-mode=1\n"
                     "a=ssrc:%u cname:zstreamer\n",
                     pt, mid_str, pt, pt, ssrc);
        }
        tr = rtcAddTrack(s->pc_id, sdp_buf);
        if (tr < 0) {
            ZST_LOG_ERROR("webrtc_endpoint",
                          "add_track: SDP fallback also failed (%d)", tr);
            return -1;
        }
    }

    /* Set up the packetizer for this track */
    rtcPacketizerInit pinit = {0};
    pinit.ssrc          = ssrc;
    pinit.cname         = "zstreamer";
    pinit.payloadType   = pt;
    pinit.clockRate     = rate;
    pinit.sequenceNumber = 0;
    pinit.timestamp     = 0;

    if (is_audio) {
        rtcSetOpusPacketizer(tr, &pinit);
    } else {
        pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
        rtcSetH264Packetizer(tr, &pinit);
    }

    /* Store track info */
    webrtc_track_t* t = &s->tracks[idx];
    t->codec        = codec;
    t->ssrc         = ssrc;
    t->payload_type = pt;
    t->clock_rate   = rate;
    snprintf(t->mid, sizeof(t->mid), "%s", mid_str);
    t->track_id     = tr;
    t->active       = true;
    s->num_tracks++;

    ZST_LOG_INFO("webrtc_endpoint",
                 "add_track: %s track #%u, ssrc=%u, pt=%d, mid=%s, tr_id=%d",
                 codec_name(codec), idx, ssrc, pt, mid_str, tr);

    return (int)idx;
}
#endif /* HAS_WEBRTC */

zst_result_t
zst_webrtc_add_video_track(
    zst_element_t* el,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    int idx = add_track_internal(s, codec, ssrc, mid, false);
    return (idx >= 0) ? ZST_OK : ZST_ERROR;
#else
    (void)s; (void)codec; (void)ssrc; (void)mid;
    ZST_LOG_INFO("webrtc_endpoint", "add_video_track: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
}

zst_result_t
zst_webrtc_add_audio_track(
    zst_element_t* el,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    int idx = add_track_internal(s, codec, ssrc, mid, true);
    return (idx >= 0) ? ZST_OK : ZST_ERROR;
#else
    (void)s; (void)codec; (void)ssrc; (void)mid;
    ZST_LOG_INFO("webrtc_endpoint", "add_audio_track: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
}

zst_result_t
zst_webrtc_send_media(
    zst_element_t* el,
    uint32_t track_index,
    const void* data,
    int size)
{
    if (!el || !data || size <= 0) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (track_index >= s->num_tracks) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "send_media: invalid track index %u (num_tracks=%u)",
                      track_index, s->num_tracks);
        return ZST_ERROR;
    }

    webrtc_track_t* t = &s->tracks[track_index];
    if (!t->active || t->track_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "send_media: track %u is not active", track_index);
        return ZST_ERROR;
    }

    /* Send raw encoded frame to the packetizer, which converts to RTP */
    int ret = rtcSendMessage(t->track_id, (const char*)data, size);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "send_media: rtcSendMessage failed (%d) on track %u",
                      ret, track_index);
        return ZST_ERROR;
    }

    return ZST_OK;
#else
    (void)s; (void)track_index; (void)data; (void)size;
    return ZST_ERROR;
#endif
}

/*════════════════════════════════════════════════════════════════════════════
  Phase 4: Inbound track callback registration
════════════════════════════════════════════════════════════════════════════*/
zst_result_t
zst_webrtc_set_on_track_callback(zst_element_t* el,
                                 zst_webrtc_on_track_fn fn,
                                 void* user_data)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;
    s->on_track_fn = fn;
    s->on_track_user_data = user_data;
    return ZST_OK;
}

/*════════════════════════════════════════════════════════════════════════════
  Plugin entry points (for BUILDING_PLUGIN / dlopen mode)
════════════════════════════════════════════════════════════════════════════*/
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "webrtc_endpoint") == 0) {
        return zst_webrtc_endpoint_create();
    }
    return NULL;
}

static const zst_property_spec_t g_webrtc_properties[] = {
    { "stun-servers",       ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Comma-separated list of STUN server URLs" },
    { "turn-servers",       ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Comma-separated list of TURN server URLs" },
    { "ice-urls",           ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Combined comma-separated ICE URLs (auto-detects STUN vs TURN)" },
    { "remote-sdp",         ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Remote SDP offer or answer" },
    { "ice-state",          ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "new", "ICE connection state" },
    { "dtls-state",         ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "new", "DTLS handshake state" },
    { "sctp-state",         ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "new", "SCTP association state" },
    { "signalling-state",   ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "stable", "SDP signalling state" },
    { "negotiated",         ZST_PROPERTY_BOOL,
      ZST_PROPERTY_READABLE, "false", "Whether SDP negotiation has completed" },
    { "local-sdp",          ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "", "Local SDP offer or answer (read-only)" }
};

static const zst_pad_template_t g_webrtc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS,
      "video/x-h264;video/x-h265;audio/x-aac;audio/opus;application/octet-stream" },
    { "src",  ZST_PAD_SRC,  ZST_PAD_ALWAYS,
      "video/x-h264;video/x-h265;audio/x-aac;audio/opus;application/octet-stream" }
};

static const zst_element_desc_t g_webrtc_elements[] = {
    {
        .name        = "webrtc_endpoint",
        .long_name   = "WebRTC Endpoint",
        .category    = "Network/WebRTC",
        .description = "Unified WebRTC peer connection (sender + receiver)",
        .author      = "zstreamer",
        .properties  = g_webrtc_properties,
        .nb_properties = sizeof(g_webrtc_properties) / sizeof(g_webrtc_properties[0]),
        .pads        = g_webrtc_pads,
        .nb_pads     = sizeof(g_webrtc_pads) / sizeof(g_webrtc_pads[0]),
        .create      = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name    = "webrtc_plugin",
        .author  = "zstreamer",
        .version = "1.0.0",
        .init    = NULL,
        .deinit  = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_webrtc_elements) / sizeof(g_webrtc_elements[0]);
    }
    return g_webrtc_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
