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
#include "zstreamer/elements/zst_webrtc_twcc.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_log.h"

/* ── When libdatachannel is available, pull in the C API ─────────────────── */
#ifdef HAS_WEBRTC
#include <rtc/rtc.h>
#endif

static void on_pli(int tr, void* ptr);

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
    zst_pad_t* sink_pad; /* Cached single static sink pad */
    uint32_t num_tracks;

    /* ── Inbound tracks (Phase 4) ─────────────────────────────────────── */
    webrtc_recv_track_t recv_tracks[MAX_TRACKS];
    uint32_t num_recv_tracks;

    /* ── Inbound callback ──────────────────────────────────────────────── */
    zst_webrtc_on_track_fn on_track_fn;
    void*                  on_track_user_data;

    /* ── Data channels (Phase 5) ──────────────────────────────────────── */
    #define MAX_DATA_CHANNELS 8
    struct {
        int    dc_id;      /* libdatachannel handle */
        char   label[64];
        bool   open;
        bool   locally_created;
    } data_channels[MAX_DATA_CHANNELS];
    uint32_t num_data_channels;

    /* ── Data channel message callback ────────────────────────────────── */
    zst_webrtc_on_data_message_fn on_data_message_fn;
    void*                        on_data_message_user_data;

    /* ── Codec preference (Phase 8f) ─────────────────────────────────── */
    char*    codec_preference;     /* user-configurable, e.g. "H264,VP8,VP9" */
    char     selected_video_codec[32];  /* negotiated video codec name */
    char     selected_audio_codec[32];  /* negotiated audio codec name */

    /* ── TURN Authentication (Phase 8h) ──────────────────────────────── */
    char*    turn_username;
    char*    turn_password;

    /* ── TWCC Session (Phase 9) ────────────────────────────────────── */
    zst_webrtc_twcc_t* twcc;
} webrtc_endpoint_t;

/*════════════════════════════════════════════════════════════════════════════
  libdatachannel callbacks (Phase 2 — only compiled with HAS_WEBRTC)
════════════════════════════════════════════════════════════════════════════*/
#ifdef HAS_WEBRTC

/* ── TWCC Interceptors ───────────────────────────────────────────────────── */
static void*
on_twcc_incoming(int pc, const char *message, int size, void *ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (s && s->twcc) {
        return zst_webrtc_twcc_process_incoming(s->twcc, message, size);
    }
    return (void*)message;
}

static void*
on_twcc_outgoing(int tr, const char *message, int size, void *ptr)
{
    (void)tr;
    webrtc_endpoint_t* s = ptr;
    if (s && s->twcc) {
        return zst_webrtc_twcc_process_outgoing(s->twcc, message, size);
    }
    return (void*)message;
}

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

    ZST_LOG_INFO("webrtc_endpoint",
                 "on_local_description: RAW SDP from libdatachannel (len=%zu):\n%s",
                 strlen(sdp), sdp);

    char* compat_sdp = zst_webrtc_compat_local_sdp(sdp);
    if (!compat_sdp) {
        compat_sdp = strdup(sdp);
    }

    ZST_LOG_INFO("webrtc_endpoint",
                 "on_local_description: COMPAT SDP (len=%zu):\n%s",
                 strlen(compat_sdp), compat_sdp);

    if (s->twcc && strcmp(type, "answer") == 0) {
        size_t new_len = strlen(compat_sdp) + 2048;
        char* new_sdp = malloc(new_len);
        if (new_sdp) {
            strcpy(new_sdp, compat_sdp);
            if (zst_webrtc_twcc_inject_answer(s->twcc, new_sdp, new_len) == 0) {
                free(compat_sdp);
                compat_sdp = new_sdp;
                ZST_LOG_INFO("webrtc_endpoint", "on_local_description: Injected TWCC into answer");
            } else {
                free(new_sdp);
            }
        }
    }

    pthread_mutex_lock(&s->signaling_lock);
    free(s->local_sdp);
    s->local_sdp = compat_sdp;
    free(s->local_sdp_type);
    s->local_sdp_type = strdup(type);
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint",
                 "on_local_description: type=%s, len=%zu (compat from %zu)",
                 type, strlen(s->local_sdp), strlen(sdp));

    /* Post an event to the element's bus so the application can retrieve it */
    if (s->el && s->el->bus) {
        zst_event_t* ev = zst_event_new_webrtc_local_description(
            s->el, type, s->local_sdp);
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

/* ── Data channel open callback ──────────────────────────────────────────── */
static void
on_dc_open(int dc, void* ptr)
{
    (void)ptr;
    webrtc_endpoint_t* s = rtcGetUserPointer(dc);
    if (!s) return;

    char label_buf[64] = {0};
    rtcGetDataChannelLabel(dc, label_buf, sizeof(label_buf));

    /* Mark the channel as open */
    for (uint32_t i = 0; i < s->num_data_channels; i++) {
        if (s->data_channels[i].dc_id == dc) {
            s->data_channels[i].open = true;
            break;
        }
    }

    ZST_LOG_INFO("webrtc_endpoint", "dc_open: dc_id=%d, label=%s", dc, label_buf);

    /* Post an event */
    if (s->el && s->el->bus) {
        zst_event_t* ev = calloc(1, sizeof(*ev));
        if (ev) {
            ev->type = ZST_EVENT_STATE_CHANGED;
            ev->src = s->el;
            zst_bus_post(s->el->bus, ev);
        }
    }
}

/* ── Data channel closed callback ───────────────────────────────────────── */
static void
on_dc_closed(int dc, void* ptr)
{
    (void)ptr;
    webrtc_endpoint_t* s = rtcGetUserPointer(dc);
    if (!s) return;

    for (uint32_t i = 0; i < s->num_data_channels; i++) {
        if (s->data_channels[i].dc_id == dc) {
            s->data_channels[i].open = false;
            break;
        }
    }

    ZST_LOG_INFO("webrtc_endpoint", "dc_closed: dc_id=%d", dc);
}

/* ── Data channel message callback ──────────────────────────────────────── */
static void
on_dc_message(int dc, const char* message, int size, void* ptr)
{
    (void)ptr;
    webrtc_endpoint_t* s = rtcGetUserPointer(dc);
    if (!s) return;

    /* Find the channel label */
    char label_buf[64] = {0};
    rtcGetDataChannelLabel(dc, label_buf, sizeof(label_buf));

    ZST_LOG_DEBUG("webrtc_endpoint", "dc_message: dc_id=%d, label=%s, size=%d",
                  dc, label_buf, size);

    /* Fire user callback */
    if (s->on_data_message_fn) {
        s->on_data_message_fn(s->el, dc, label_buf, message, size,
                              s->on_data_message_user_data);
    }
}

/* ── Remote data channel callback ────────────────────────────────────────── */
static void
on_data_channel(int pc, int dc, void* ptr)
{
    (void)pc;
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    if (s->num_data_channels >= MAX_DATA_CHANNELS) {
        ZST_LOG_WARN("webrtc_endpoint", "on_data_channel: max channels reached, ignoring %d", dc);
        return;
    }

    /* Get channel label */
    char label_buf[64] = {0};
    rtcGetDataChannelLabel(dc, label_buf, sizeof(label_buf));

    /* Store the channel info */
    uint32_t idx = s->num_data_channels;
    s->data_channels[idx].dc_id = dc;
    snprintf(s->data_channels[idx].label, sizeof(s->data_channels[idx].label),
             "%s", label_buf[0] ? label_buf : "data");
    s->data_channels[idx].open = false; /* will be set to true by on_dc_open */
    s->data_channels[idx].locally_created = false;
    s->num_data_channels++;

    /* Set up callbacks for this channel */
    rtcSetUserPointer(dc, s);
    rtcSetOpenCallback(dc, on_dc_open);
    rtcSetClosedCallback(dc, on_dc_closed);
    rtcSetMessageCallback(dc, on_dc_message);

    ZST_LOG_INFO("webrtc_endpoint", "on_data_channel: dc_id=%d, label=%s, idx=%u",
                 dc, label_buf, idx);
}

/* ── RTCP PLI callback — remote requests a keyframe ─────────────────────── */
static void
on_rtcp_pli(int tr, void* ptr)
{
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    ZST_LOG_INFO("webrtc_endpoint", "rtcp_pli: track %d — remote requests keyframe", tr);

    /* Post a PLI event so upstream elements (e.g., encoder) can react */
    if (s->el && s->el->bus) {
        zst_event_t* ev = calloc(1, sizeof(*ev));
        if (ev) {
            ev->type = ZST_EVENT_WEBRTC_PLI;
            ev->src = s->el;
            ev->as.webrtc_pli.track_id = tr;
            zst_bus_post(s->el->bus, ev);
        }
    }
}

/* ── RTCP REMB callback — receiver reports estimated bitrate ────────────── */
static void
on_rtcp_remb(int tr, unsigned int bitrate, void* ptr)
{
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    ZST_LOG_DEBUG("webrtc_endpoint", "rtcp_remb: track %d, bitrate=%u bps", tr, bitrate);

    /* Post a REMB event so upstream elements can adapt bitrate */
    if (s->el && s->el->bus) {
        zst_event_t* ev = calloc(1, sizeof(*ev));
        if (ev) {
            ev->type = ZST_EVENT_WEBRTC_REMB;
            ev->src = s->el;
            ev->as.webrtc_remb.track_id = tr;
            ev->as.webrtc_remb.bitrate = bitrate;
            zst_bus_post(s->el->bus, ev);
        }
    }
}

static char*
format_turn_url(const char* url, const char* user, const char* pass)
{
    if (!url) return NULL;
    if (!user || !pass || !user[0] || !pass[0]) return strdup(url);
    if (strchr(url, '@') != NULL) return strdup(url);

    const char* protocol = "";
    const char* host_port = "";
    if (strncmp(url, "turns:", 6) == 0) {
        protocol = "turns:";
        host_port = url + 6;
    } else if (strncmp(url, "turn:", 5) == 0) {
        protocol = "turn:";
        host_port = url + 5;
    } else {
        return strdup(url);
    }

    size_t len = strlen(protocol) + strlen(user) + 1 + strlen(pass) + 1 + strlen(host_port) + 1;
    char* out = malloc(len);
    if (out) {
        snprintf(out, len, "%s%s:%s@%s", protocol, user, pass, host_port);
    }
    return out;
}

static const char**
build_ice_servers(webrtc_endpoint_t* s, uint32_t* total_servers_out)
{
    uint32_t total = s->num_stun_urls + s->num_turn_urls;
    *total_servers_out = total;
    if (total == 0) return NULL;

    const char** ice_servers = calloc(total, sizeof(char*));
    if (!ice_servers) return NULL;

    for (uint32_t i = 0; i < s->num_stun_urls; i++) {
        ice_servers[i] = s->stun_urls[i];
    }
    for (uint32_t i = 0; i < s->num_turn_urls; i++) {
        char* formatted = format_turn_url(s->turn_urls[i], s->turn_username, s->turn_password);
        ice_servers[s->num_stun_urls + i] = formatted ? formatted : s->turn_urls[i];
    }
    return ice_servers;
}

static void
free_ice_servers(webrtc_endpoint_t* s, const char** ice_servers)
{
    if (!ice_servers) return;
    for (uint32_t i = 0; i < s->num_turn_urls; i++) {
        const char* srv = ice_servers[s->num_stun_urls + i];
        if (srv != s->turn_urls[i]) {
            free((void*)srv);
        }
    }
    free(ice_servers);
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
    uint32_t total_servers = 0;
    const char** ice_servers = build_ice_servers(s, &total_servers);
    if (total_servers > 0 && !ice_servers) {
        ZST_LOG_ERROR("webrtc_endpoint", "open: failed to allocate ICE server array");
        return ZST_ERROR;
    }
    config.iceServers = ice_servers;
    config.iceServersCount = (int)total_servers;

    s->pc_id = rtcCreatePeerConnection(&config);
    free_ice_servers(s, ice_servers);

    if (s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "open: rtcCreatePeerConnection failed (%d)", s->pc_id);
        return ZST_ERROR;
    }

    s->pc_created = true;

    /* Store ourselves as user pointer so callbacks can find us */
    rtcSetUserPointer(s->pc_id, s);
    s->twcc = zst_webrtc_twcc_create(s->pc_id, s->el ? s->el->bus : NULL);
    rtcSetMediaInterceptorCallback(s->pc_id, on_twcc_incoming);

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
webrtc_sink_push(zst_pad_t* pad, zst_buffer_t* in)
{
    if (!pad || !in) return ZST_ERROR;

    zst_element_t* el = pad->parent;
    if (!el) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (in->memory.size == 0) return ZST_OK;

    int track_idx = -1;

    if (strcmp(pad->name, "sink") == 0) {
        /* Fallback routing for the original static "sink" pad */
        bool is_audio = (in->type == ZST_BUFFER_AUDIO_PACKET || in->type == ZST_BUFFER_AUDIO_FRAME);
        for (uint32_t i = 0; i < s->num_tracks; i++) {
            webrtc_track_t* t = &s->tracks[i];
            if (!t->active || t->track_id < 0) continue;
            bool track_is_audio = (t->codec == ZST_WEBRTC_CODEC_OPUS ||
                                   t->codec == ZST_WEBRTC_CODEC_PCMU ||
                                   t->codec == ZST_WEBRTC_CODEC_PCMA ||
                                   t->codec == ZST_WEBRTC_CODEC_AAC);
            if (is_audio == track_is_audio) {
                track_idx = (int)i;
                break;
            }
        }
    } else {
        /* Dynamic pads store their track index in pad->priv */
        if (pad->priv != NULL) {
            /* Using pointer-as-integer for index storage */
            track_idx = (int)(intptr_t)pad->priv - 1;
        }
    }

    if (track_idx >= 0 && track_idx < (int)s->num_tracks) {
        webrtc_track_t* t = &s->tracks[track_idx];
        if (t->active && t->track_id >= 0) {
            /* Convert PTS (nanoseconds) to RTP timestamp (based on clock rate) */
            uint32_t rtp_timestamp = (uint32_t)(in->pts * t->clock_rate / 1000000000ULL);
            rtcSetTrackRtpTimestamp(t->track_id, rtp_timestamp);

            int ret = rtcSendMessage(t->track_id,
                                    (const char*)in->memory.data,
                                    (int)in->memory.size);
            if (ret != RTC_ERR_SUCCESS) {
                ZST_LOG_DEBUG("webrtc_endpoint",
                             "process: rtcSendMessage failed (%d) on track %u",
                             ret, track_idx);
            } else {
                ZST_LOG_DEBUG("webrtc_endpoint",
                              "process: forwarded %zu bytes to track %u",
                              in->memory.size, track_idx);
            }
        }
    } else {
        ZST_LOG_WARN("webrtc_endpoint",
                     "process: no matching track for buffer of %zu bytes on pad %s",
                     in->memory.size, pad->name);
    }
#else
    ZST_LOG_DEBUG("webrtc_endpoint",
                  "process: received buffer of %zu bytes (no HAS_WEBRTC)",
                  in->memory.size);
#endif

    return ZST_OK;
}

static zst_result_t
webrtc_process(
    zst_element_t* el,
    zst_buffer_t* in,
    zst_buffer_t** out)
{
    /* Should not be called directly for active media processing anymore,
       as we override pad->push. Keeping it for interface completeness if needed. */
    (void)el; (void)in; (void)out;
    return ZST_OK;
}

static zst_result_t
webrtc_close(zst_element_t* el)
{
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (s->twcc) {
        zst_webrtc_twcc_destroy(s->twcc);
        s->twcc = NULL;
    }

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

    free(s->remote_sdp);
    s->remote_sdp = NULL;
    free(s->local_sdp);
    s->local_sdp = NULL;
    free(s->local_sdp_type);
    s->local_sdp_type = NULL;

    if (s->stun_urls) {
        for (uint32_t i = 0; i < s->num_stun_urls; i++) {
            free(s->stun_urls[i]);
        }
        free(s->stun_urls);
        s->stun_urls = NULL;
    }
    s->num_stun_urls = 0;

    if (s->turn_urls) {
        for (uint32_t i = 0; i < s->num_turn_urls; i++) {
            free(s->turn_urls[i]);
        }
        free(s->turn_urls);
        s->turn_urls = NULL;
    }
    s->num_turn_urls = 0;

    pthread_mutex_destroy(&s->signaling_lock);

    free(s->codec_preference);
    s->codec_preference = NULL;

    free(s->turn_username);
    s->turn_username = NULL;
    free(s->turn_password);
    s->turn_password = NULL;

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
        if (s->stun_urls) {
            for (uint32_t i = 0; i < s->num_stun_urls; i++) {
                free(s->stun_urls[i]);
            }
            free(s->stun_urls);
        }
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

            uint32_t idx = 0;
            const char* start = value;
            while (*start && idx < count) {
                while (*start == ' ' || *start == '\t') start++;
                if (!*start) break;

                const char* end = strchr(start, ',');
                size_t len = end ? (size_t)(end - start) : strlen(start);

            if (len > 0) {
                s->stun_urls[idx++] = strndup(start, len);
            }

                if (!end) break;
                start = end + 1;
            }
            s->num_stun_urls = idx;
        }
        return ZST_OK;
    }

    if (strcmp(name, "turn-servers") == 0) {
        if (s->turn_urls) {
            for (uint32_t i = 0; i < s->num_turn_urls; i++) {
                free(s->turn_urls[i]);
            }
            free(s->turn_urls);
        }
        s->turn_urls = NULL;
        s->num_turn_urls = 0;

        if (value && value[0]) {
            uint32_t count = 1;
            for (const char* p = value; *p; p++) {
                if (*p == ',') count++;
            }
            s->turn_urls = calloc(count, sizeof(char*));
            if (!s->turn_urls) return ZST_ERROR;

            uint32_t idx = 0;
            const char* start = value;
            while (*start && idx < count) {
                while (*start == ' ' || *start == '\t') start++;
                if (!*start) break;

                const char* end = strchr(start, ',');
                size_t len = end ? (size_t)(end - start) : strlen(start);

            if (len > 0) {
                s->turn_urls[idx++] = strndup(start, len);
            }

                if (!end) break;
                start = end + 1;
            }
            s->num_turn_urls = idx;
        }
        return ZST_OK;
    }

    if (strcmp(name, "ice-urls") == 0) {
        /* Convenience: set both STUN and TURN URLs from a comma-separated list.
           URLs containing "turn:" or "turns:" go to turn-servers; others to stun-servers. */
        if (!value || !value[0]) {
            if (s->stun_urls) {
                for (uint32_t i = 0; i < s->num_stun_urls; i++) {
                    free(s->stun_urls[i]);
                }
                free(s->stun_urls);
            }
            s->stun_urls = NULL; s->num_stun_urls = 0;
            if (s->turn_urls) {
                for (uint32_t i = 0; i < s->num_turn_urls; i++) {
                    free(s->turn_urls[i]);
                }
                free(s->turn_urls);
            }
            s->turn_urls = NULL; s->num_turn_urls = 0;
            return ZST_OK;
        }

        uint32_t total = 1;
        for (const char* p = value; *p; p++) { if (*p == ',') total++; }

        char** stun_tmp = calloc(total, sizeof(char*));
        char** turn_tmp = calloc(total, sizeof(char*));
        uint32_t n_stun = 0, n_turn = 0;

        const char* start = value;
        while (*start) {
            while (*start == ' ' || *start == '\t') start++;
            if (!*start) break;

            const char* end = strchr(start, ',');
            size_t len = end ? (size_t)(end - start) : strlen(start);

            if (len > 0) {
                if ((len >= 5 && strncmp(start, "turn:", 5) == 0) ||
                    (len >= 6 && strncmp(start, "turns:", 6) == 0)) {
                    turn_tmp[n_turn++] = strndup(start, len);
                } else {
                    stun_tmp[n_stun++] = strndup(start, len);
                }
            }

            if (!end) break;
            start = end + 1;
        }

        if (s->stun_urls) {
            for (uint32_t i = 0; i < s->num_stun_urls; i++) {
                free(s->stun_urls[i]);
            }
            free(s->stun_urls);
        }
        s->stun_urls = stun_tmp;
        s->num_stun_urls = n_stun;

        if (s->turn_urls) {
            for (uint32_t i = 0; i < s->num_turn_urls; i++) {
                free(s->turn_urls[i]);
            }
            free(s->turn_urls);
        }
        s->turn_urls = turn_tmp;
        s->num_turn_urls = n_turn;
        return ZST_OK;
    }

    if (strcmp(name, "remote-sdp") == 0) {
        free(s->remote_sdp);
        s->remote_sdp = value ? strdup(value) : NULL;
        ZST_LOG_INFO("webrtc_endpoint", "set_property: remote-sdp set (%zu bytes)",
                     value ? strlen(value) : 0);
        return ZST_OK;
    }

    if (strcmp(name, "codec-preference") == 0) {
        free(s->codec_preference);
        s->codec_preference = value ? strdup(value) : NULL;
        ZST_LOG_INFO("webrtc_endpoint", "set_property: codec-preference=%s",
                     s->codec_preference ? s->codec_preference : "(default)");
        return ZST_OK;
    }

    if (strcmp(name, "turn-username") == 0) {
        free(s->turn_username);
        s->turn_username = value ? strdup(value) : NULL;
        return ZST_OK;
    }

    if (strcmp(name, "turn-password") == 0) {
        free(s->turn_password);
        s->turn_password = value ? strdup(value) : NULL;
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
        size_t current_len = 0;
        for (uint32_t i = 0; i < s->num_stun_urls; i++) {
            if (i > 0) {
                if (max_len - current_len - 1 > 0) {
                    value_out[current_len++] = ',';
                    value_out[current_len] = '\0';
                }
            }
            size_t url_len = strlen(s->stun_urls[i]);
            size_t remain = max_len - current_len - 1;
            size_t copy_len = (remain < url_len) ? remain : url_len;
            if (copy_len > 0) {
                memcpy(value_out + current_len, s->stun_urls[i], copy_len);
                current_len += copy_len;
                value_out[current_len] = '\0';
            }
        }
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "turn-servers") == 0) {
        value_out[0] = '\0';
        size_t current_len = 0;
        for (uint32_t i = 0; i < s->num_turn_urls; i++) {
            if (i > 0) {
                if (max_len - current_len - 1 > 0) {
                    value_out[current_len++] = ',';
                    value_out[current_len] = '\0';
                }
            }
            size_t url_len = strlen(s->turn_urls[i]);
            size_t remain = max_len - current_len - 1;
            size_t copy_len = (remain < url_len) ? remain : url_len;
            if (copy_len > 0) {
                memcpy(value_out + current_len, s->turn_urls[i], copy_len);
                current_len += copy_len;
                value_out[current_len] = '\0';
            }
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

    if (strcmp(name, "codec-preference") == 0) {
        snprintf(value_out, max_len, "%s",
                 s->codec_preference ? s->codec_preference : "");
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "selected-video-codec") == 0) {
        snprintf(value_out, max_len, "%s", s->selected_video_codec);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "selected-audio-codec") == 0) {
        snprintf(value_out, max_len, "%s", s->selected_audio_codec);
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "turn-username") == 0) {
        snprintf(value_out, max_len, "%s", s->turn_username ? s->turn_username : "");
        pthread_mutex_unlock(&s->signaling_lock);
        return ZST_OK;
    }

    if (strcmp(name, "turn-password") == 0) {
        snprintf(value_out, max_len, "%s", s->turn_password ? s->turn_password : "");
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
    sink->push = webrtc_sink_push; /* custom push to override default */
    zst_element_add_pad(el, sink);
    priv->sink_pad = sink;

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

/*════════════════════════════════════════════════════════════════════════════
  Phase 8f — Receiver-Side Codec Selection

  Parses an incoming SDP offer, identifies all offered codecs per media
  section, and rewrites the SDP m= line to retain only the best-matching
  codec based on a preference list.

  Default video preference:  H264 > VP8 > VP9 > H265 > AV1
  Default audio preference:  opus > PCMU > PCMA > AAC

  The preference can be overridden via the "codec-preference" property,
  which is a comma-separated list of codec names (e.g. "VP8,H264,VP9").
  The first match wins.

  - selected_video_codec and selected_audio_codec are set on the endpoint
    struct and exposed via get_property.
  - Non-selected rtpmap / fmtp / rtcp-fb lines are stripped from the output.
════════════════════════════════════════════════════════════════════════════*/

/* Default preference order (index 0 = highest priority) */
static const char* g_default_video_prefs[] = {
    "H264", "VP8", "VP9", "H265", "AV1", NULL
};
static const char* g_default_audio_prefs[] = {
    "opus", "PCMU", "PCMA", "AAC", NULL
};

/* Max codecs per media section we track */
#define MAX_CODECS_PER_SECTION 32

typedef struct {
    int  pt;             /* payload type number */
    char name[32];       /* codec name from a=rtpmap, e.g. "H264" */
    int  prio;           /* preference rank (0 = best) */
} sdp_codec_entry_t;

/**
 * Compute the preference rank for a codec name given a preference list.
 * Returns 0 for highest priority, higher for lower priority, INT_MAX if not found.
 */
static int
codec_pref_rank(const char* codec_name, const char** pref_list)
{
    for (int i = 0; pref_list[i]; i++) {
        if (strcasecmp(codec_name, pref_list[i]) == 0) return i;
    }
    return 9999;
}

/**
 * Build a preference list from a user-provided comma-separated string.
 * Returns a NULL-terminated array of strings. Caller must free the returned
 * array and each string.  Returns NULL if preference is NULL/empty.
 */
static const char**
parse_codec_preference(const char* pref)
{
    if (!pref || !pref[0]) return NULL;

    /* Count tokens */
    uint32_t count = 1;
    for (const char* p = pref; *p; p++) {
        if (*p == ',') count++;
    }

    const char** list = calloc(count + 1, sizeof(char*));
    if (!list) return NULL;

    uint32_t idx = 0;
    const char* start = pref;
    while (*start && idx < count) {
        while (*start == ' ' || *start == '\t') start++;
        if (!*start) break;

        const char* end = strchr(start, ',');
        size_t len = end ? (size_t)(end - start) : strlen(start);

        /* Trim trailing whitespace */
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
            len--;
        }

        if (len > 0) {
            list[idx++] = strndup(start, len);
        }

        if (!end) break;
        start = end + 1;
    }
    list[idx] = NULL;
    return list;
}

static void
free_codec_preference(const char** list)
{
    if (!list) return;
    for (int i = 0; list[i]; i++) free((char*)list[i]);
    free(list);
}

char*
zst_webrtc_select_codecs(const char* sdp, const char* preference,
                         char* selected_video_out, size_t video_out_len,
                         char* selected_audio_out, size_t audio_out_len)
{
    if (!sdp) return NULL;

    /* Build preference lists */
    const char** user_prefs = parse_codec_preference(preference);

    size_t sdp_len = strlen(sdp);
    size_t out_cap = sdp_len + 512;
    char* out = malloc(out_cap);
    if (!out) { free_codec_preference(user_prefs); return NULL; }
    out[0] = '\0';
    size_t out_len = 0;

    /* We accumulate lines for each media section, then decide what to keep */
    /* Simple two-pass approach:
     *   Pass 1: Scan the SDP to find codec info per media section
     *   Pass 2: Rewrite, keeping only the selected codec's lines
     */

    /* ── Structures to hold per-section info ─────────────────────────── */
    typedef struct {
        bool    is_audio;
        int     num_codecs;
        sdp_codec_entry_t codecs[MAX_CODECS_PER_SECTION];
        int     selected_pt;    /* best codec payload type */
        char    selected_name[32];
    } media_section_t;

    #define MAX_MEDIA_SECTIONS 8
    media_section_t sections[MAX_MEDIA_SECTIONS];
    int num_sections = 0;

    /* ── Pass 1: scan for codecs ─────────────────────────────────────── */
    const char* line = sdp;
    int cur_section = -1;

    while (*line) {
        const char* next_line = line;
        while (*next_line && *next_line != '\n') next_line++;

        size_t len = (size_t)(next_line - line);
        size_t content_len = len;
        if (content_len > 0 && line[content_len - 1] == '\r') content_len--;

        if (strncmp(line, "m=", 2) == 0 && num_sections < MAX_MEDIA_SECTIONS) {
            cur_section = num_sections++;
            memset(&sections[cur_section], 0, sizeof(sections[cur_section]));
            sections[cur_section].is_audio = (strncmp(line, "m=audio", 7) == 0);
            sections[cur_section].selected_pt = -1;
        }

        /* Parse a=rtpmap:<pt> <name>/<clock> lines */
        if (cur_section >= 0 && strncmp(line, "a=rtpmap:", 9) == 0) {
            media_section_t* sec = &sections[cur_section];
            if (sec->num_codecs < MAX_CODECS_PER_SECTION) {
                int pt = 0;
                char cname[32] = {0};
                if (sscanf(line + 9, "%d %31[^/\r\n]", &pt, cname) >= 2) {
                    sdp_codec_entry_t* ce = &sec->codecs[sec->num_codecs];
                    ce->pt = pt;
                    snprintf(ce->name, sizeof(ce->name), "%s", cname);

                    /* Compute priority */
                    const char** vprefs = user_prefs ? user_prefs : g_default_video_prefs;
                    const char** aprefs = user_prefs ? user_prefs : g_default_audio_prefs;
                    ce->prio = codec_pref_rank(cname, sec->is_audio ? aprefs : vprefs);

                    sec->num_codecs++;
                }
            }
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    /* ── Select best codec per section ───────────────────────────────── */
    for (int s = 0; s < num_sections; s++) {
        media_section_t* sec = &sections[s];
        int best_prio = 10000;
        int best_idx = -1;
        for (int c = 0; c < sec->num_codecs; c++) {
            if (sec->codecs[c].prio < best_prio) {
                best_prio = sec->codecs[c].prio;
                best_idx = c;
            }
        }
        /* Fallback: if no codec matched the preference list (prio == 10000), pick the first one */
        if (best_idx < 0 && sec->num_codecs > 0) {
            best_idx = 0;
        }
        if (best_idx >= 0) {
            sec->selected_pt = sec->codecs[best_idx].pt;
            snprintf(sec->selected_name, sizeof(sec->selected_name),
                     "%s", sec->codecs[best_idx].name);
            ZST_LOG_INFO("webrtc_endpoint",
                         "codec_select: section %d (%s): selected %s (pt=%d) from %d offered codecs",
                         s, sec->is_audio ? "audio" : "video",
                         sec->selected_name, sec->selected_pt, sec->num_codecs);

            /* Report to caller */
            if (sec->is_audio && selected_audio_out) {
                snprintf(selected_audio_out, audio_out_len, "%s", sec->selected_name);
            }
            if (!sec->is_audio && selected_video_out) {
                snprintf(selected_video_out, video_out_len, "%s", sec->selected_name);
            }
        } else if (sec->num_codecs == 0) {
            ZST_LOG_DEBUG("webrtc_endpoint",
                          "codec_select: section %d has no rtpmap codecs, passing through", s);
        }
    }

    /* ── Pass 2: rewrite SDP ─────────────────────────────────────────── */
    line = sdp;
    cur_section = -1;

    while (*line) {
        const char* next_line = line;
        while (*next_line && *next_line != '\n') next_line++;

        size_t len = (size_t)(next_line - line);
        size_t content_len = len;
        if (content_len > 0 && line[content_len - 1] == '\r') content_len--;

        bool keep = true;

        if (strncmp(line, "m=", 2) == 0) {
            cur_section++;

            /* Rewrite the m= line to include only the selected payload type */
            if (cur_section >= 0 && cur_section < num_sections &&
                sections[cur_section].selected_pt >= 0 &&
                sections[cur_section].num_codecs > 1) {

                media_section_t* sec = &sections[cur_section];

                /* Extract the protocol part: "m=<type> <port> <proto>" */
                char mtype[32] = {0};
                int port = 0;
                char proto[64] = {0};

                /* Parse: "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98" */
                const char* after_m = line + 2;
                int n = sscanf(after_m, "%31s %d %63s", mtype, &port, proto);
                if (n == 3) {
                    /* Write rewritten m= line with only selected pt */
                    int written = snprintf(out + out_len, out_cap - out_len,
                                           "m=%s %d %s %d\r\n",
                                           mtype, port, proto, sec->selected_pt);
                    if (written > 0) out_len += (size_t)written;
                    keep = false;
                }
            }
        }

        /* Filter out rtpmap/fmtp/rtcp-fb lines for non-selected codecs */
        if (cur_section >= 0 && cur_section < num_sections &&
            sections[cur_section].selected_pt >= 0 &&
            sections[cur_section].num_codecs > 1) {

            int sel_pt = sections[cur_section].selected_pt;
            int line_pt = -1;

            if (strncmp(line, "a=rtpmap:", 9) == 0) {
                sscanf(line + 9, "%d", &line_pt);
            } else if (strncmp(line, "a=fmtp:", 7) == 0) {
                sscanf(line + 7, "%d", &line_pt);
            } else if (strncmp(line, "a=rtcp-fb:", 10) == 0) {
                sscanf(line + 10, "%d", &line_pt);
            }

            if (line_pt >= 0 && line_pt != sel_pt) {
                keep = false;
                ZST_LOG_DEBUG("webrtc_endpoint",
                              "codec_select: dropping line for pt=%d (selected pt=%d)",
                              line_pt, sel_pt);
            }
        }

        if (keep) {
            /* Ensure capacity */
            if (out_len + content_len + 4 > out_cap) {
                out_cap = out_cap * 2 + content_len + 4;
                char* tmp = realloc(out, out_cap);
                if (!tmp) { free(out); free_codec_preference(user_prefs); return NULL; }
                out = tmp;
            }
            if (content_len > 0) {
                memcpy(out + out_len, line, content_len);
                out_len += content_len;
            }
            out_len += (size_t)snprintf(out + out_len, out_cap - out_len, "\r\n");
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    out[out_len] = '\0';

    free_codec_preference(user_prefs);
    #undef MAX_MEDIA_SECTIONS
    return out;
}

char*
zst_webrtc_filter_sdp(const char* sdp)
{
    if (!sdp) return NULL;

    size_t sdp_len = strlen(sdp);
    char* filtered = malloc(sdp_len + 1);
    if (!filtered) return NULL;
    filtered[0] = '\0';

    size_t out_pos = 0;
    const char* line = sdp;
    while (*line) {
        const char* next_line = line;
        while (*next_line && *next_line != '\n') {
            next_line++;
        }

        size_t line_len = next_line - line;
        if (*next_line == '\n') {
            line_len++;
        }

        size_t content_len = line_len;
        while (content_len > 0 && (line[content_len - 1] == '\r' || line[content_len - 1] == '\n')) {
            content_len--;
        }

        bool keep = true;
        char* line_copy = malloc(content_len + 1);
        if (line_copy) {
            memcpy(line_copy, line, content_len);
            line_copy[content_len] = '\0';

            if (strncmp(line_copy, "a=extmap:", 9) == 0) {
                bool unsupported = false;
                if (strstr(line_copy, "transport-wide-cc-02") ||
                    strstr(line_copy, "transport-wide-cc-01") ||
                    strstr(line_copy, "transport-wide-cc") ||
                    strstr(line_copy, "abs-send-time") ||
                    strstr(line_copy, "goog-playout-delay") ||
                    strstr(line_copy, "playout-delay") ||
                    strstr(line_copy, "video-orientation") ||
                    strstr(line_copy, "ssrc-audio-level")) {
                    unsupported = true;
                }

                if (unsupported) {
                    keep = false;
                    ZST_LOG_INFO("webrtc_endpoint", "Filtered unsupported SDP extension: %s", line_copy);
                }
            } else if (strncmp(line_copy, "a=rtcp-fb:", 10) == 0) {
                if (strstr(line_copy, "transport-cc") || strstr(line_copy, "transport-wide-cc")) {
                    keep = false;
                    ZST_LOG_INFO("webrtc_endpoint", "Filtered unsupported SDP RTCP feedback: %s", line_copy);
                }
            }

            free(line_copy);
        }

        if (keep) {
            memcpy(filtered + out_pos, line, line_len);
            out_pos += line_len;
        }

        line = next_line;
        if (*line == '\n') {
            line++;
        }
    }

    filtered[out_pos] = '\0';
    return filtered;
}

char*
zst_webrtc_compat_local_sdp(const char* sdp)
{
    if (!sdp) return NULL;

    // Collect all mids
    char mids[16][64];
    int num_mids = 0;

    const char* line = sdp;
    while (*line) {
        const char* next_line = line;
        while (*next_line && *next_line != '\n') next_line++;

        size_t len = next_line - line;
        if (len > 0 && line[len - 1] == '\r') len--;

        if (strncmp(line, "a=mid:", 6) == 0 && len > 6) {
            size_t val_len = len - 6;
            if (val_len < 64 && num_mids < 16) {
                memcpy(mids[num_mids], line + 6, val_len);
                mids[num_mids][val_len] = '\0';
                num_mids++;
            }
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    size_t sdp_len = strlen(sdp);
    size_t out_cap = sdp_len + 4096;
    char* out = malloc(out_cap);
    if (!out) return NULL;
    out[0] = '\0';
    size_t out_len = 0;

    bool session_level = true;
    bool has_bundle_group = false;
    int media_section_idx = 0;

    char current_mid[64] = "";
    bool has_rtcp_mux = false;
    bool has_msid = false;
    bool has_ssrc = false;

    #define FINISH_MEDIA_SECTION() do { \
        if (!session_level) { \
            if (!has_rtcp_mux) { \
                out_len += snprintf(out + out_len, out_cap - out_len, "a=rtcp-mux\r\n"); \
            } \
            if (!has_msid && strlen(current_mid) > 0) { \
                out_len += snprintf(out + out_len, out_cap - out_len, \
                    "a=msid:zstreamer-stream zstreamer-track-%s\r\n", current_mid); \
            } \
            if (!has_ssrc && strlen(current_mid) > 0) { \
                uint32_t fallback_ssrc = 1000 + media_section_idx; \
                out_len += snprintf(out + out_len, out_cap - out_len, \
                    "a=ssrc:%u cname:zstreamer-cname\r\n", fallback_ssrc); \
            } \
        } \
    } while(0)

    line = sdp;
    while (*line) {
        const char* next_line = line;
        while (*next_line && *next_line != '\n') next_line++;

        size_t len = next_line - line;
        size_t content_len = len;
        if (content_len > 0 && line[content_len - 1] == '\r') content_len--;

        if (strncmp(line, "m=", 2) == 0) {
            media_section_idx++;
            if (session_level) {
                if (!has_bundle_group && num_mids > 0) {
                    out_len += snprintf(out + out_len, out_cap - out_len, "a=group:BUNDLE");
                    for (int i = 0; i < num_mids; i++) {
                        out_len += snprintf(out + out_len, out_cap - out_len, " %s", mids[i]);
                    }
                    out_len += snprintf(out + out_len, out_cap - out_len, "\r\n");
                }
                session_level = false;
            } else {
                FINISH_MEDIA_SECTION();
            }

            current_mid[0] = '\0';
            has_rtcp_mux = false;
            has_msid = false;
            has_ssrc = false;
        }

        bool skip_original = false;

        if (!session_level) {
            if (strncmp(line, "a=mid:", 6) == 0 && content_len > 6) {
                size_t val_len = content_len - 6;
                if (val_len < 64) {
                    memcpy(current_mid, line + 6, val_len);
                    current_mid[val_len] = '\0';
                }
            } else if (strncmp(line, "a=rtcp-mux", 10) == 0) {
                has_rtcp_mux = true;
            } else if (strncmp(line, "a=msid", 6) == 0) {
                has_msid = true;
            } else if (strncmp(line, "a=ssrc:", 7) == 0) {
                has_ssrc = true;
                char* cname_ptr = strstr((char*)line, "cname:");
                if (cname_ptr) {
                    uint32_t ssrc = 0;
                    sscanf(line + 7, "%u", &ssrc);
                    out_len += snprintf(out + out_len, out_cap - out_len, "a=ssrc:%u cname:zstreamer-cname\r\n", ssrc);
                    skip_original = true;
                }
            }
        } else {
            if (strncmp(line, "a=group:BUNDLE", 14) == 0) {
                has_bundle_group = true;
            }
        }

        if (!skip_original) {
            if (content_len > 0) {
                memcpy(out + out_len, line, content_len);
                out_len += content_len;
            }
            out_len += snprintf(out + out_len, out_cap - out_len, "\r\n");
        }

        line = next_line;
        if (*line == '\n') line++;
    }

    FINISH_MEDIA_SECTION();

    #undef FINISH_MEDIA_SECTION
    return out;
}


zst_result_t
zst_webrtc_set_remote_description(
    zst_element_t* el, const char* type, const char* sdp)
{
    if (!el || !type || !sdp) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

    /* Step 1: Filter unsupported extensions (TWCC, etc.) */
    char* filtered_sdp = zst_webrtc_filter_sdp(sdp);
    if (!filtered_sdp) {
        return ZST_ERROR;
    }

    /* Step 2: If this is an offer, apply codec selection to pick best codec */
    char* final_sdp = filtered_sdp;
    if (strcmp(type, "offer") == 0) {
        char* selected_sdp = zst_webrtc_select_codecs(
            filtered_sdp, s->codec_preference,
            s->selected_video_codec, sizeof(s->selected_video_codec),
            s->selected_audio_codec, sizeof(s->selected_audio_codec));
        if (selected_sdp) {
            free(filtered_sdp);
            final_sdp = selected_sdp;
        }
    }

    if (s->twcc && strcmp(type, "offer") == 0) {
        zst_webrtc_twcc_parse_offer(s->twcc, final_sdp);
    }

    pthread_mutex_lock(&s->signaling_lock);
    free(s->remote_sdp);
    s->remote_sdp = final_sdp;
    pthread_mutex_unlock(&s->signaling_lock);

    ZST_LOG_INFO("webrtc_endpoint",
                 "set_remote_description: type=%s, sdp_len=%zu (filtered from %zu)",
                 type, strlen(s->remote_sdp), strlen(sdp));
    
    ZST_LOG_INFO("webrtc_endpoint",
                 "set_remote_description: FILTERED SDP sent to libdatachannel:\n%s",
                 s->remote_sdp);

#ifdef HAS_WEBRTC
    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "set_remote_description: PeerConnection not created");
        return ZST_ERROR;
    }

    int ret = rtcSetRemoteDescription(s->pc_id, s->remote_sdp, type);
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
zst_webrtc_restart_ice(zst_element_t* el)
{
    if (!el) return ZST_ERROR;

    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (!s->pc_created || s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: PeerConnection not created");
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "restart_ice: starting ICE restart on PeerConnection %d", s->pc_id);

    // 1. Close and delete old PeerConnection
    rtcClosePeerConnection(s->pc_id);
    rtcDeletePeerConnection(s->pc_id);
    s->pc_id = -1;

    // Clear inbound tracks (remote will re-add them)
    s->num_recv_tracks = 0;

    // Filter data channels to retain only locally created ones
    uint32_t kept_dcs = 0;
    for (uint32_t i = 0; i < s->num_data_channels; i++) {
        if (s->data_channels[i].locally_created) {
            s->data_channels[kept_dcs] = s->data_channels[i];
            s->data_channels[kept_dcs].dc_id = -1;
            s->data_channels[kept_dcs].open = false;
            kept_dcs++;
        }
    }
    s->num_data_channels = kept_dcs;

    // 2. Build configuration and create new PeerConnection
    rtcConfiguration config = {0};
    uint32_t total_servers = 0;
    const char** ice_servers = build_ice_servers(s, &total_servers);
    if (total_servers > 0 && !ice_servers) {
        ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: failed to allocate ICE server array");
        return ZST_ERROR;
    }
    config.iceServers = ice_servers;
    config.iceServersCount = (int)total_servers;

    s->pc_id = rtcCreatePeerConnection(&config);
    free_ice_servers(s, ice_servers);

    if (s->pc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: failed to recreate PeerConnection (%d)", s->pc_id);
        return ZST_ERROR;
    }

    // 3. Register callbacks on new PC
    rtcSetUserPointer(s->pc_id, s);
    if (s->twcc) {
        zst_webrtc_twcc_destroy(s->twcc);
    }
    s->twcc = zst_webrtc_twcc_create(s->pc_id, s->el ? s->el->bus : NULL);
    rtcSetMediaInterceptorCallback(s->pc_id, on_twcc_incoming);

    rtcSetLocalDescriptionCallback(s->pc_id, on_local_description);
    rtcSetLocalCandidateCallback(s->pc_id, on_local_candidate);
    rtcSetStateChangeCallback(s->pc_id, on_state_change);
    rtcSetIceStateChangeCallback(s->pc_id, on_ice_state_change);
    rtcSetSignalingStateChangeCallback(s->pc_id, on_signaling_state_change);
    rtcSetTrackCallback(s->pc_id, on_track);
    rtcSetDataChannelCallback(s->pc_id, on_data_channel);

    // 4. Re-add outbound tracks
    for (uint32_t i = 0; i < s->num_tracks; i++) {
        webrtc_track_t* t = &s->tracks[i];
        if (!t->active) continue;

        bool is_audio = (t->codec == ZST_WEBRTC_CODEC_OPUS ||
                         t->codec == ZST_WEBRTC_CODEC_PCMU ||
                         t->codec == ZST_WEBRTC_CODEC_PCMA ||
                         t->codec == ZST_WEBRTC_CODEC_AAC);

        rtcTrackInit tinit = {0};
        tinit.direction = RTC_DIRECTION_SENDONLY;
        tinit.codec     = codec_to_rtc(t->codec);
        tinit.payloadType = t->payload_type;
        tinit.ssrc      = t->ssrc;
        tinit.mid       = t->mid;
        tinit.name      = "zstreamer";
        tinit.msid      = "stream0";

        int tr = rtcAddTrackEx(s->pc_id, &tinit);
        if (tr < 0) {
            char sdp_buf[512];
            if (is_audio) {
                snprintf(sdp_buf, sizeof(sdp_buf),
                         "m=audio 9 UDP/TLS/RTP/SAVPF %d\n"
                         "c=IN IP4 0.0.0.0\n"
                         "a=mid:%s\n"
                         "a=sendonly\n"
                         "a=rtpmap:%d opus/48000/2\n"
                         "a=ssrc:%u cname:zstreamer\n",
                         t->payload_type, t->mid, t->payload_type, t->ssrc);
            } else {
                snprintf(sdp_buf, sizeof(sdp_buf),
                         "m=video 9 UDP/TLS/RTP/SAVPF %d\n"
                         "c=IN IP4 0.0.0.0\n"
                         "a=mid:%s\n"
                         "a=sendonly\n"
                         "a=rtpmap:%d H264/90000\n"
                         "a=fmtp:%d packetization-mode=1\n"
                         "a=ssrc:%u cname:zstreamer\n",
                         t->payload_type, t->mid, t->payload_type, t->payload_type, t->ssrc);
            }
            tr = rtcAddTrack(s->pc_id, sdp_buf);
        }

        if (tr >= 0) {
            t->track_id = tr;

            rtcPacketizerInit pinit = {0};
            pinit.ssrc          = t->ssrc;
            pinit.cname         = "zstreamer";
            pinit.payloadType   = t->payload_type;
            pinit.clockRate     = t->clock_rate;
            pinit.sequenceNumber = 0;
            pinit.timestamp     = 0;

            if (is_audio) {
                rtcSetOpusPacketizer(tr, &pinit);
            } else {
                switch (t->codec) {
                case ZST_WEBRTC_CODEC_VP8:  rtcSetVP8Packetizer(tr, &pinit); break;
                case ZST_WEBRTC_CODEC_VP9:  rtcSetVP9Packetizer(tr, &pinit); break;
                case ZST_WEBRTC_CODEC_H265:
                    pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
                    rtcSetH265Packetizer(tr, &pinit);
                    break;
                case ZST_WEBRTC_CODEC_AV1:  rtcSetAV1Packetizer(tr, &pinit); break;
                default:
                    pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
                    rtcSetH264Packetizer(tr, &pinit);
                    break;
                }
                rtcChainPliHandler(tr, on_pli);
            }
            ZST_LOG_INFO("webrtc_endpoint", "restart_ice: re-added track %s (id=%d)", t->mid, tr);
        } else {
            ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: failed to re-add track %s", t->mid);
        }
    }

    // 5. Recreate local data channels
    for (uint32_t i = 0; i < s->num_data_channels; i++) {
        int dc_id = rtcCreateDataChannel(s->pc_id, s->data_channels[i].label);
        if (dc_id >= 0) {
            s->data_channels[i].dc_id = dc_id;
            rtcSetUserPointer(dc_id, s);
            rtcSetOpenCallback(dc_id, on_dc_open);
            rtcSetClosedCallback(dc_id, on_dc_closed);
            rtcSetMessageCallback(dc_id, on_dc_message);
            ZST_LOG_INFO("webrtc_endpoint", "restart_ice: re-created data channel %s (id=%d)",
                         s->data_channels[i].label, dc_id);
        } else {
            ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: failed to re-create data channel %s",
                          s->data_channels[i].label);
        }
    }

    // 6. Reset negotiation status
    s->negotiated = false;
    snprintf(s->signalling_state, sizeof(s->signalling_state), "stable");

    // 7. Create new offer
    int ret = rtcSetLocalDescription(s->pc_id, "offer");
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint", "restart_ice: rtcSetLocalDescription offer failed (%d)", ret);
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "restart_ice: new SDP offer generated for PeerConnection %d", s->pc_id);
    return ZST_OK;
#else
    (void)s;
    ZST_LOG_INFO("webrtc_endpoint", "restart_ice: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
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

    if (s->num_data_channels >= MAX_DATA_CHANNELS) {
        ZST_LOG_ERROR("webrtc_endpoint", "create_data_channel: max channels reached");
        return ZST_ERROR;
    }

    int dc_id = rtcCreateDataChannel(s->pc_id, label);
    if (dc_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "create_data_channel: rtcCreateDataChannel failed (%d)", dc_id);
        return ZST_ERROR;
    }

    /* Store channel info */
    uint32_t idx = s->num_data_channels;
    s->data_channels[idx].dc_id = dc_id;
    snprintf(s->data_channels[idx].label, sizeof(s->data_channels[idx].label),
             "%s", label);
    s->data_channels[idx].open = false; /* will be set by on_dc_open */
    s->data_channels[idx].locally_created = true;
    s->num_data_channels++;

    /* Set up callbacks */
    rtcSetUserPointer(dc_id, s);
    rtcSetOpenCallback(dc_id, on_dc_open);
    rtcSetClosedCallback(dc_id, on_dc_closed);
    rtcSetMessageCallback(dc_id, on_dc_message);

    ZST_LOG_INFO("webrtc_endpoint", "create_data_channel: label=%s, dc_id=%d, idx=%u",
                 label, dc_id, idx);
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
    /* Use PTs outside Chrome's typical dynamic range (96-125) to avoid
       conflicts when Chrome maps the same PT to a different codec.
       Chrome offers: 96=VP8, 97=rtx, 98=VP9, 99=rtx, 100=VP9, ...
       103-125 = various H264 profiles, AV1, H265.
       We use 126+ which Chrome never offers. */
    switch (codec) {
    case ZST_WEBRTC_CODEC_H264: return 126;
    case ZST_WEBRTC_CODEC_VP8:  return 127;
    case ZST_WEBRTC_CODEC_VP9:  return 96;
    case ZST_WEBRTC_CODEC_H265: return 97;
    case ZST_WEBRTC_CODEC_AV1:  return 98;
    case ZST_WEBRTC_CODEC_OPUS: return 111;
    case ZST_WEBRTC_CODEC_PCMU: return 0;
    case ZST_WEBRTC_CODEC_PCMA: return 8;
    case ZST_WEBRTC_CODEC_AAC:  return 99;
    default:                    return 126;
    }
}

static void
on_pli(int tr, void* ptr)
{
    webrtc_endpoint_t* s = ptr;
    if (!s) return;

    /* Find the track index matching this track handle */
    int track_idx = -1;
    for (uint32_t i = 0; i < s->num_tracks; i++) {
        if (s->tracks[i].active && s->tracks[i].track_id == tr) {
            track_idx = (int)i;
            break;
        }
    }

    zst_pad_t* target_pad = NULL;

    if (track_idx >= 0) {
        /* Look up the named dynamic sink pad for this track */
        char pad_name[64];
        snprintf(pad_name, sizeof(pad_name), "sink_video_%d", track_idx);
        target_pad = zst_element_get_pad(s->el, pad_name);
    }

    /* Fallback: use the legacy static sink pad */
    if (!target_pad) {
        target_pad = s->sink_pad;
    }
    if (!target_pad) {
        target_pad = zst_element_get_pad(s->el, "sink");
    }

    if (target_pad) {
        zst_pad_event_t* event = zst_pad_event_new_force_keyframe();
        if (event) {
            zst_pad_push_event_upstream(target_pad, event);
            zst_pad_event_unref(event);
        }
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
    bool is_audio,
    int override_pt)  /* -1 = use default */
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
    uint8_t pt = (override_pt >= 0) ? (uint8_t)override_pt : codec_default_payload_type(codec);
    uint32_t rate = codec_clock_rate(codec);
    const char* mid_str = mid ? mid : (is_audio ? "audio" : "video");

    char track_id_buf[64];
    snprintf(track_id_buf, sizeof(track_id_buf), "track-%s", mid_str);

    /* Build the rtcTrackInit */
    rtcTrackInit tinit = {0};
    tinit.direction = RTC_DIRECTION_SENDONLY;
    tinit.codec     = codec_to_rtc(codec);
    tinit.payloadType = pt;
    tinit.ssrc      = ssrc;
    tinit.mid       = mid_str;
    tinit.name      = "zstreamer";
    tinit.msid      = "stream0";
    tinit.trackId   = track_id_buf;

    int tr = rtcAddTrackEx(s->pc_id, &tinit);
    if (tr < 0) {
        /* Fallback: try rtcAddTrack with the SDP string directly */
        ZST_LOG_ERROR("webrtc_endpoint",
                      "add_track: rtcAddTrackEx failed (%d), trying SDP fallback", tr);
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
        ZST_LOG_INFO("webrtc_endpoint",
                     "add_track: SDP fallback for %s:\n%s",
                     is_audio ? "audio" : "video", sdp_buf);
        tr = rtcAddTrack(s->pc_id, sdp_buf);
        if (tr < 0) {
            ZST_LOG_ERROR("webrtc_endpoint",
                          "add_track: SDP fallback also failed (%d)", tr);
            return -1;
        }
    }
    
    ZST_LOG_INFO("webrtc_endpoint",
                 "add_track: SUCCESS track_id=%d, codec=%d, pt=%u, ssrc=%u, mid=%s",
                 tr, codec, pt, ssrc, mid_str);

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
        switch (codec) {
        case ZST_WEBRTC_CODEC_VP8:
            rtcSetVP8Packetizer(tr, &pinit);
            break;
        case ZST_WEBRTC_CODEC_VP9:
            rtcSetVP9Packetizer(tr, &pinit);
            break;
        case ZST_WEBRTC_CODEC_H265:
            pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
            rtcSetH265Packetizer(tr, &pinit);
            break;
        case ZST_WEBRTC_CODEC_AV1:
            rtcSetAV1Packetizer(tr, &pinit);
            break;
        default: /* H264 and others */
            pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
            rtcSetH264Packetizer(tr, &pinit);
            break;
        }
        rtcChainPliHandler(tr, on_pli);
        rtcSetTrackInterceptorCallback(tr, on_twcc_outgoing);
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

    /* Create dynamic sink pad for this track (Phase 8a) */
    char pad_name[64];
    if (is_audio) {
        snprintf(pad_name, sizeof(pad_name), "sink_audio_%u", idx);
    } else {
        snprintf(pad_name, sizeof(pad_name), "sink_video_%u", idx);
    }
    zst_pad_t* sink_pad = zst_pad_create(pad_name, ZST_PAD_SINK);
    if (sink_pad) {
        zst_caps_t* caps = NULL;
        if (is_audio) {
            caps = zst_caps_new_simple("audio/opus;audio/x-aac;audio/x-pcmu;audio/x-pcma");
        } else {
            caps = zst_caps_new_simple("video/x-h264;video/x-vp8;video/x-vp9;video/x-h265;video/x-av1");
        }
        if (caps) {
            zst_pad_set_template_caps(sink_pad, caps);
            zst_caps_destroy(caps);
        }
        sink_pad->priv = (void*)(intptr_t)(idx + 1);
        sink_pad->push = webrtc_sink_push;

        if (zst_element_add_pad(s->el, sink_pad) == ZST_OK) {
            /* Post a pad-added event */
            if (s->el && s->el->bus) {
                zst_event_t* ev = calloc(1, sizeof(*ev));
                if (ev) {
                    ev->type = ZST_EVENT_PAD_ADDED;
                    ev->src = s->el;
                    ev->as.pad_added.pad = zst_pad_ref(sink_pad);
                    zst_bus_post(s->el->bus, ev);
                }
            }
        } else {
            zst_pad_destroy(sink_pad);
        }
    }

    /* Chain RTCP handlers for QoS support (Phase 6) */
    rtcChainRtcpReceivingSession(tr);
    rtcChainRtcpSrReporter(tr);
    rtcChainRtcpNackResponder(tr, RTC_DEFAULT_MAXIMUM_PACKET_COUNT_FOR_NACK_CACHE);
    rtcChainRembHandler(tr, on_rtcp_remb);

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
    int idx = add_track_internal(s, codec, ssrc, mid, false, -1);
    return (idx >= 0) ? ZST_OK : ZST_ERROR;
#else
    (void)s; (void)codec; (void)ssrc; (void)mid;
    ZST_LOG_INFO("webrtc_endpoint", "add_video_track: stub (no HAS_WEBRTC)");
    return ZST_ERROR;
#endif
}

zst_result_t
zst_webrtc_add_video_track_with_pt(
    zst_element_t* el,
    zst_webrtc_codec_t codec,
    uint32_t ssrc,
    const char* mid,
    int payload_type)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    int idx = add_track_internal(s, codec, ssrc, mid, false, payload_type);
    return (idx >= 0) ? ZST_OK : ZST_ERROR;
#else
    (void)s; (void)codec; (void)ssrc; (void)mid; (void)payload_type;
    ZST_LOG_INFO("webrtc_endpoint", "add_video_track_with_pt: stub (no HAS_WEBRTC)");
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
    int idx = add_track_internal(s, codec, ssrc, mid, true, -1);
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
        ZST_LOG_DEBUG("webrtc_endpoint",
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

zst_result_t
zst_webrtc_set_on_data_message_callback(zst_element_t* el,
                                        zst_webrtc_on_data_message_fn fn,
                                        void* user_data)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;
    s->on_data_message_fn = fn;
    s->on_data_message_user_data = user_data;
    return ZST_OK;
}

zst_result_t
zst_webrtc_send_data(
    zst_element_t* el,
    int channel_id,
    const void* data,
    int size)
{
    if (!el || !data || size <= 0) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    bool found = false;
    for (uint32_t i = 0; i < s->num_data_channels; i++) {
        if (s->data_channels[i].dc_id == channel_id) {
            if (!s->data_channels[i].open) {
                ZST_LOG_ERROR("webrtc_endpoint",
                              "send_data: channel %d (label=%s) is not open",
                              channel_id, s->data_channels[i].label);
                return ZST_ERROR;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "send_data: unknown channel id %d", channel_id);
        return ZST_ERROR;
    }

    int ret = rtcSendMessage(channel_id, (const char*)data, size);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_DEBUG("webrtc_endpoint",
                      "send_data: rtcSendMessage failed (%d) on channel %d",
                      ret, channel_id);
        return ZST_ERROR;
    }

    return ZST_OK;
#else
    (void)s; (void)channel_id; (void)data; (void)size;
    return ZST_ERROR;
#endif
}

/*════════════════════════════════════════════════════════════════════════════
  Phase 6: RTCP QoS — keyframe and bitrate requests
════════════════════════════════════════════════════════════════════════════*/
zst_result_t
zst_webrtc_request_keyframe(
    zst_element_t* el,
    uint32_t track_index)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (track_index >= s->num_tracks) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_keyframe: invalid track index %u", track_index);
        return ZST_ERROR;
    }

    webrtc_track_t* t = &s->tracks[track_index];
    if (!t->active || t->track_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_keyframe: track %u is not active", track_index);
        return ZST_ERROR;
    }

    int ret = rtcRequestKeyframe(t->track_id);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_keyframe: rtcRequestKeyframe failed (%d)", ret);
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "request_keyframe: track %u (tr_id=%d)",
                 track_index, t->track_id);
    return ZST_OK;
#else
    (void)s; (void)track_index;
    return ZST_ERROR;
#endif
}

zst_result_t
zst_webrtc_request_bitrate(
    zst_element_t* el,
    uint32_t track_index,
    unsigned int bitrate)
{
    if (!el) return ZST_ERROR;
    webrtc_endpoint_t* s = el->priv;

#ifdef HAS_WEBRTC
    if (track_index >= s->num_tracks) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_bitrate: invalid track index %u", track_index);
        return ZST_ERROR;
    }

    webrtc_track_t* t = &s->tracks[track_index];
    if (!t->active || t->track_id < 0) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_bitrate: track %u is not active", track_index);
        return ZST_ERROR;
    }

    int ret = rtcRequestBitrate(t->track_id, bitrate);
    if (ret != RTC_ERR_SUCCESS) {
        ZST_LOG_ERROR("webrtc_endpoint",
                      "request_bitrate: rtcRequestBitrate failed (%d)", ret);
        return ZST_ERROR;
    }

    ZST_LOG_INFO("webrtc_endpoint", "request_bitrate: track %u, %u bps",
                 track_index, bitrate);
    return ZST_OK;
#else
    (void)s; (void)track_index; (void)bitrate;
    return ZST_ERROR;
#endif
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
      ZST_PROPERTY_READABLE, "", "Local SDP offer or answer (read-only)" },
    { "codec-preference",   ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Preferred codecs hierarchy ranking list" },
    { "selected-video-codec", ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "", "Negotiated video codec" },
    { "selected-audio-codec", ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE, "", "Negotiated audio codec" },
    { "turn-username",      ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "TURN server authentication username" },
    { "turn-password",      ZST_PROPERTY_STRING,
      ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "TURN server authentication password" }
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
