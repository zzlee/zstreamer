/*=============================================================================
    webrtc_receiver.c — zstreamer WebRTC Receiver Example

    Receives video/audio from a browser and decodes it.
    Pipeline (dynamic, created when browser's tracks arrive):
        webrtc_endpoint → h264_decoder (or vp8/vp9) → fake_sink
        webrtc_endpoint → opus_decoder → fake_sink

    Signaling: embedded WebSocket server on port 8080
    HTTP:      serves a sender page (browser sends video to us) on port 8000

    Usage:
        ./webrtc_receiver [--port <ws_port>] [--http-port <http_port>]
                          [--stun <stun_url>]

    Open http://localhost:8000 in Chrome, grant camera/mic, click "Send".
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_bus.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_ws_server.h"
#include "zst_clock.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"
#include "zst_scheduler.h"

extern zst_result_t zst_register_builtin_elements(void);

/* ── Configuration ──────────────────────────────────────────────────────── */
static int  g_ws_port   = 8080;
static int  g_http_port = 8000;
static char g_stun[256] = "stun:stun.l.google.com:19302";

/* ── Global State ───────────────────────────────────────────────────────── */
static volatile bool    g_running   = true;
static pthread_mutex_t  g_lock      = PTHREAD_MUTEX_INITIALIZER;
static zst_ws_server_t* g_ws        = NULL;
static zst_pipeline_t*  g_pipeline  = NULL;
static zst_scheduler_t* g_scheduler = NULL;
static zst_element_t*   g_webrtc    = NULL;
static int              g_client    = -1;
static pthread_t        g_bus_thread;
static pthread_t        g_http_thread;

static uint32_t g_video_frames = 0;
static uint32_t g_audio_frames = 0;

/* ── Signal handler ─────────────────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_running = false; }

/* ── Minimal JSON helpers ───────────────────────────────────────────────── */
static char* escape_json(const char* s) {
    if (!s) return strdup("");
    size_t n = strlen(s);
    char* o = malloc(n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if      (s[i] == '"')  { o[j++] = '\\'; o[j++] = '"'; }
        else if (s[i] == '\\') { o[j++] = '\\'; o[j++] = '\\'; }
        else if (s[i] == '\n') { o[j++] = '\\'; o[j++] = 'n'; }
        else if (s[i] == '\r') { o[j++] = '\\'; o[j++] = 'r'; }
        else                    o[j++] = s[i];
    }
    o[j] = '\0'; return o;
}

static int json_get(const char* json, const char* key, char* out, size_t max) {
    char search[128]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++; size_t i = 0;
        while (*p && *p != '"' && i < max-1) { if (*p == '\\' && *(p+1)) p++; out[i++] = *p++; }
        out[i] = '\0'; return 1;
    }
    return 0;
}

/* ── Track arrival — build decoder pipeline dynamically ────────────────── */
static void on_track_added(zst_element_t* el, zst_pad_t* src_pad, void* user_data) {
    (void)el; (void)user_data;

    /* Determine track type from pad caps name */
    const char* caps = (src_pad->caps && src_pad->caps->structs) ? src_pad->caps->structs->media_type : "";
    bool is_video = (strstr(caps, "video") != NULL);
    bool is_audio = (strstr(caps, "audio") != NULL);

    ZST_LOG_INFO("receiver", "New track: %s (caps=%s)", src_pad->name, caps ? caps : "?");

    /* Select decoder element by codec */
    const char* dec_type = NULL;
    if (is_video) {
        if (strstr(caps, "h264") || strstr(caps, "H264")) dec_type = "h264_decoder";
        else if (strstr(caps, "vp8") || strstr(caps, "VP8")) dec_type = "vp8_decoder";
        else if (strstr(caps, "vp9") || strstr(caps, "VP9")) dec_type = "vp9_decoder";
        else if (strstr(caps, "h265") || strstr(caps, "H265")) dec_type = "h265_decoder";
        else dec_type = "h264_decoder"; /* fallback */
    } else if (is_audio) {
        if (strstr(caps, "opus") || strstr(caps, "Opus")) dec_type = "opus_decoder";
        else if (strstr(caps, "aac")  || strstr(caps, "AAC"))  dec_type = "aac_decoder";
        else dec_type = "opus_decoder";
    } else {
        ZST_LOG_WARN("receiver", "Unknown track type, skipping");
        return;
    }

    /* Generate unique element names */
    static int track_counter = 0;
    char dec_name[32], sink_name[32];
    snprintf(dec_name,  sizeof(dec_name),  "dec_%d",  track_counter);
    snprintf(sink_name, sizeof(sink_name), "sink_%d", track_counter++);

    /* Create decoder + sink */
    zst_element_t* dec  = zst_element_factory_make(dec_type);
    zst_element_t* sink = zst_element_factory_make("fakesink");
    if (!dec || !sink) {
        ZST_LOG_ERROR("receiver", "Failed to create decoder '%s'", dec_type);
        return;
    }

    pthread_mutex_lock(&g_lock);
    zst_pipeline_add(g_pipeline, dec);
    zst_pipeline_add(g_pipeline, sink);

    zst_pad_link(src_pad, zst_element_get_pad(dec, "sink"));
    zst_pad_link(zst_element_get_pad(dec, "src"), zst_element_get_pad(sink, "sink"));

    zst_element_set_state(dec,  ZST_STATE_PLAYING);
    zst_element_set_state(sink, ZST_STATE_PLAYING);
    pthread_mutex_unlock(&g_lock);

    ZST_LOG_INFO("receiver", "Decoder pipeline: %s → %s connected for track %s",
                 dec_type, "fake_sink", src_pad->name);
}

/* ── WebSocket callbacks ────────────────────────────────────────────────── */
static void on_ws_connect(int id, void* ud) {
    (void)ud;
    ZST_LOG_INFO("receiver", "Browser connected (client %d)", id);
    pthread_mutex_lock(&g_lock);
    g_client = id;
    pthread_mutex_unlock(&g_lock);
}

static void on_ws_disconnect(int id, void* ud) {
    (void)ud; (void)id;
    ZST_LOG_INFO("receiver", "Browser disconnected");
    pthread_mutex_lock(&g_lock);
    g_client = -1;
    pthread_mutex_unlock(&g_lock);
}

static void on_ws_message(int client_id, const char* msg, size_t len, void* ud) {
    (void)ud; (void)len; (void)client_id;
    char type[32] = {0}, sdp[16384] = {0};
    char cand[2048] = {0}, mid[8] = {0}, mline[8] = {0};

    if (!json_get(msg, "type", type, sizeof(type))) return;

    if (strcmp(type, "offer") == 0) {
        if (!json_get(msg, "sdp", sdp, sizeof(sdp))) return;
        ZST_LOG_INFO("receiver", "Received SDP offer from browser");
        pthread_mutex_lock(&g_lock);
        if (g_webrtc) zst_webrtc_set_remote_description(g_webrtc, "offer", sdp);
        pthread_mutex_unlock(&g_lock);

    } else if (strcmp(type, "candidate") == 0) {
        json_get(msg, "candidate", cand, sizeof(cand));
        json_get(msg, "sdpMid", mid, sizeof(mid));
        json_get(msg, "sdpMLineIndex", mline, sizeof(mline));
        pthread_mutex_lock(&g_lock);
        if (g_webrtc) zst_webrtc_add_ice_candidate(g_webrtc, mid, atoi(mline), cand);
        pthread_mutex_unlock(&g_lock);
    }
}

/* ── Bus thread ─────────────────────────────────────────────────────────── */
static void* bus_thread_fn(void* arg) {
    (void)arg;
    while (g_running) {
        zst_event_t* ev = NULL;
        pthread_mutex_lock(&g_lock);
        zst_result_t r = (g_pipeline && g_pipeline->bus)
                       ? zst_bus_pop(g_pipeline->bus, &ev, 0)
                       : ZST_ERROR;
        pthread_mutex_unlock(&g_lock);
        if (r != ZST_OK || !ev) { usleep(10000); continue; }

        if (ev->type == ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION) {
            char* esdp = escape_json(ev->as.webrtc_local_description.sdp);
            char json[32768];
            snprintf(json, sizeof(json),
                "{\"type\":\"%s\",\"sdp\":\"%s\"}",
                ev->as.webrtc_local_description.type, esdp);
            free(esdp);
            pthread_mutex_lock(&g_lock);
            if (g_client >= 0) zst_ws_send(g_ws, g_client, json, strlen(json));
            pthread_mutex_unlock(&g_lock);

        } else if (ev->type == ZST_EVENT_WEBRTC_ICE_CANDIDATE) {
            char* ecand = escape_json(ev->as.webrtc_ice_candidate.candidate);
            char json[2048];
            snprintf(json, sizeof(json),
                "{\"type\":\"candidate\",\"candidate\":\"%s\","
                "\"sdpMid\":\"%s\",\"sdpMLineIndex\":%d}",
                ecand, ev->as.webrtc_ice_candidate.mid,
                ev->as.webrtc_ice_candidate.mlineindex);
            free(ecand);
            pthread_mutex_lock(&g_lock);
            if (g_client >= 0) zst_ws_send(g_ws, g_client, json, strlen(json));
            pthread_mutex_unlock(&g_lock);

        } else if (ev->type == ZST_EVENT_PAD_ADDED) {
            /* A new src pad was added by webrtc_endpoint (remote track arrived) */
            on_track_added(g_webrtc, ev->as.pad_added.pad, NULL);
        }
        zst_event_destroy(ev);
    }
    return NULL;
}

/* ── Embedded HTTP server (sends browser's camera to us) ────────────────── */
static const char* HTML_PAGE =
"<!DOCTYPE html><html><head><title>zstreamer WebRTC Receiver</title>"
"<style>body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
"video{width:320px;height:240px;background:#222;border:2px solid #444}"
"button{padding:10px 24px;margin:5px;font-size:15px;border:none;border-radius:6px;cursor:pointer}"
"#btn-send{background:#3498db;color:#fff}#btn-stop{background:#e74c3c;color:#fff}"
"#log{font-family:monospace;font-size:11px;white-space:pre;height:160px;"
"overflow-y:auto;background:#1a1a1a;padding:8px;margin-top:8px;border-radius:4px}"
"</style></head><body>"
"<h2>📷 zstreamer WebRTC Receiver</h2>"
"<p>Your camera/mic will be sent to zstreamer for decoding.</p>"
"<video id='preview' autoplay playsinline muted></video><br>"
"<button id='btn-send' onclick='send()'>Send to zstreamer</button>"
"<button id='btn-stop' onclick='stop()'>Stop</button>"
"<div id='log'></div>"
"<script>"
"let pc,ws,localStream;"
"function log(m){const d=document.getElementById('log');"
"d.textContent+=new Date().toLocaleTimeString()+' '+m+'\\n';d.scrollTop=d.scrollHeight;}"
"async function send(){"
"localStream=await navigator.mediaDevices.getUserMedia({video:true,audio:true});"
"document.getElementById('preview').srcObject=localStream;"
"ws=new WebSocket('ws://'+location.hostname+':8080');"
"ws.onopen=async()=>{"
"log('Signaling connected');"
"pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});"
"localStream.getTracks().forEach(t=>pc.addTrack(t,localStream));"
"pc.onicecandidate=e=>{if(e.candidate)"
"ws.send(JSON.stringify({type:'candidate',...e.candidate.toJSON(),sdpMLineIndex:e.candidate.sdpMLineIndex}));};"
"pc.oniceconnectionstatechange=()=>log('ICE: '+pc.iceConnectionState);"
"pc.ontrack=e=>{"
"log('Received remote track: '+e.track.kind);"
"if(e.track.kind==='audio'){"
"const a=document.createElement('audio');"
"a.srcObject=new MediaStream([e.track]);a.autoplay=true;"
"document.body.appendChild(a);"
"}};"
"const o=await pc.createOffer();await pc.setLocalDescription(o);"
"ws.send(JSON.stringify({type:'offer',sdp:o.sdp}));log('Offer sent to zstreamer');};"
"ws.onmessage=async m=>{"
"const d=JSON.parse(m.data);"
"if(d.type==='answer'){await pc.setRemoteDescription(d);log('Answer from zstreamer: connected');}"
"else if(d.type==='candidate'){await pc.addIceCandidate(d.candidate);}};"
"ws.onerror=()=>log('WS error');ws.onclose=()=>log('WS closed');}"
"function stop(){"
"if(pc){pc.close();pc=null;}if(ws){ws.close();ws=null;}"
"if(localStream)localStream.getTracks().forEach(t=>t.stop());"
"document.getElementById('preview').srcObject=null;log('Stopped');}"
"</script></body></html>\n";

static void* http_thread_fn(void* arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family=AF_INET, .sin_port=htons((uint16_t)g_http_port), .sin_addr.s_addr=0 };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(srv); return NULL; }
    listen(srv, 4);
    ZST_LOG_INFO("receiver", "HTTP server on port %d", g_http_port);
    while (g_running) {
        struct timeval tv = {1,0}; fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
        if (select(srv+1, &rfds, NULL, NULL, &tv) <= 0) continue;
        int cl = accept(srv, NULL, NULL); if (cl < 0) continue;
        char req[1024]; recv(cl, req, sizeof(req)-1, 0);
        const char* body = HTML_PAGE; size_t blen = strlen(body);
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", blen);
        send(cl, hdr, strlen(hdr), 0); send(cl, body, blen, 0); close(cl);
    }
    close(srv); return NULL;
}

/* ── Pipeline ────────────────────────────────────────────────────────────── */
static bool start_pipeline(void) {
    zst_element_t* webrtc = zst_element_factory_make("webrtc_endpoint");
    if (!webrtc) { ZST_LOG_ERROR("receiver", "Failed to create webrtc_endpoint"); return false; }

    if (g_stun[0]) zst_element_set_property_string(webrtc, "stun-servers", g_stun);

    zst_element_t* asrc = zst_element_factory_make("audiotestsrc");
    zst_element_t* aenc = zst_element_factory_make("opusenc");
    if (asrc && aenc) {
        zst_element_set_property_int(asrc, "sample-rate", 48000);
        zst_element_set_property_int(asrc, "channels", 2);
        zst_element_set_property_string(asrc, "sample-format", "S16LE");
        zst_element_set_property_string(asrc, "wave", "sine");
        zst_element_set_property_int(asrc, "frequency", 440);
        zst_element_set_property_int(asrc, "samples-per-buffer", 960);
        zst_element_set_property_bool(asrc, "real-time-pacing", true);
        
        zst_webrtc_add_audio_track(webrtc, ZST_WEBRTC_CODEC_OPUS, 111, "audio0");
    }

    g_pipeline = zst_pipeline_create();
    zst_pipeline_add(g_pipeline, webrtc);

    if (asrc && aenc) {
        zst_pipeline_add(g_pipeline, asrc);
        zst_pipeline_add(g_pipeline, aenc);
        zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(aenc, "sink"));
        zst_pad_link(zst_element_get_pad(aenc, "src"), zst_element_get_pad(webrtc, "sink_audio_0"));
    }
    g_webrtc = webrtc;

    /* ZST_EVENT_PAD_ADDED is handled in the bus thread to dynamically link tracks */

    zst_clock_t* clock = zst_clock_system_create();
    zst_pipeline_set_clock(g_pipeline, clock);
    zst_clock_unref(clock);

    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 2 };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, g_pipeline);
    zst_pipeline_set_state(g_pipeline, ZST_STATE_PLAYING);
    zst_scheduler_run(g_scheduler);

    ZST_LOG_INFO("receiver", "Receiver pipeline started (waiting for browser connection)");
    return true;
}

static void stop_pipeline(void) {
    pthread_mutex_lock(&g_lock);
    if (g_scheduler) { zst_scheduler_stop(g_scheduler); zst_scheduler_destroy(g_scheduler); g_scheduler = NULL; }
    if (g_pipeline)  { zst_pipeline_set_state(g_pipeline, ZST_STATE_NULL); zst_pipeline_destroy(g_pipeline); g_pipeline = NULL; }
    g_webrtc = NULL;
    pthread_mutex_unlock(&g_lock);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--port")      && i+1 < argc) g_ws_port   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--http-port") && i+1 < argc) g_http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stun")      && i+1 < argc) snprintf(g_stun, sizeof(g_stun), "%s", argv[++i]);
        else {
            printf("Usage: %s [--port WS] [--http-port HTTP] [--stun URL]\n", argv[0]);
            return 1;
        }
    }

    zst_register_builtin_elements();

    if (!start_pipeline()) return 1;
    pthread_create(&g_http_thread, NULL, http_thread_fn, NULL);

    g_ws = zst_ws_server_create(g_ws_port);
    zst_ws_server_set_callbacks(g_ws, on_ws_connect, on_ws_message, on_ws_disconnect, NULL);
    if (zst_ws_server_start(g_ws) != ZST_OK) {
        ZST_LOG_ERROR("receiver", "WebSocket server failed on port %d", g_ws_port);
        return 1;
    }

    pthread_create(&g_bus_thread, NULL, bus_thread_fn, NULL);

    printf("\n"
           "╔══════════════════════════════════════════╗\n"
           "║  zstreamer WebRTC Receiver               ║\n"
           "╠══════════════════════════════════════════╣\n"
           "║  Sender page:   http://localhost:%d      ║\n"
           "║  Signaling WS:  ws://localhost:%d        ║\n"
           "║  Open page → grant camera/mic → Send     ║\n"
           "║  Decoded frames logged to console        ║\n"
           "║  Press Ctrl+C to stop                    ║\n"
           "╚══════════════════════════════════════════╝\n\n",
           g_http_port, g_ws_port);

    while (g_running) usleep(100000);

    ZST_LOG_INFO("receiver", "Shutting down...");
    zst_ws_server_stop(g_ws);
    pthread_join(g_bus_thread, NULL);
    pthread_join(g_http_thread, NULL);
    stop_pipeline();
    return 0;
}
