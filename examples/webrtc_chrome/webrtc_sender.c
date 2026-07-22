/*=============================================================================
    webrtc_sender.c — zstreamer WebRTC Sender Example

    Pipeline: videotestsrc → x264_encoder → webrtc_endpoint → Chrome/Firefox
              audiotestsrc → opus_encoder ↗

    Signaling: embedded WebSocket server on port 8080
    HTTP:      serves a minimal control page on port 8000

    Usage:
        ./webrtc_sender [--port <ws_port>] [--http-port <http_port>]
                        [--stun <stun_url>] [--codec <H264|VP8|VP9>]
                        [--width <px>] [--height <px>] [--fps <fps>]
                        [--bitrate <bps>]

    Open http://localhost:8000 in Chrome, click Connect.
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
#include "zst_log.h"
#include "zst_ws_server.h"
#include "zst_clock.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"
#include "zst_scheduler.h"

extern zst_result_t zst_register_builtin_elements(void);

/* ── Configuration ──────────────────────────────────────────────────────── */
static int          g_ws_port    = 8080;
static int          g_http_port  = 8000;
static char         g_stun[256]  = "stun:stun.l.google.com:19302";
static char         g_codec[32]  = "H264";
static int          g_width      = 640;
static int          g_height     = 480;
static int          g_fps        = 30;
static int          g_bitrate    = 2000000; /* 2 Mbps initial */

/* ── Global State ───────────────────────────────────────────────────────── */
static volatile bool    g_running    = true;
static pthread_mutex_t  g_lock       = PTHREAD_MUTEX_INITIALIZER;
static zst_ws_server_t* g_ws         = NULL;
static zst_pipeline_t*  g_pipeline   = NULL;
static zst_scheduler_t* g_scheduler  = NULL;
static zst_element_t*   g_webrtc     = NULL;
static zst_element_t*   g_venc       = NULL;
static int              g_client     = -1;
static bool             g_answered   = false;
static pthread_t        g_bus_thread;
static pthread_t        g_http_thread;

/* ── Signal handler ─────────────────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_running = false; }

/* ── Minimal JSON helpers ───────────────────────────────────────────────── */
static char* escape_json(const char* s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    char* out = malloc(len * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"')       { out[j++] = '\\'; out[j++] = '"'; }
        else if (s[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (s[i] == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (s[i] == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else                    out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

static int json_get(const char* json, const char* key, char* out, size_t max) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < max - 1) {
            if (*p == '\\' && *(p+1)) { p++; }
            out[i++] = *p++;
        }
        out[i] = '\0';
        return 1;
    }
    return 0;
}

/* ── WebSocket callbacks ────────────────────────────────────────────────── */
static void on_ws_connect(int client_id, void* ud) {
    (void)ud;
    ZST_LOG_INFO("sender", "Browser connected (client %d)", client_id);
    pthread_mutex_lock(&g_lock);
    g_client   = client_id;
    g_answered = false;
    pthread_mutex_unlock(&g_lock);
}

static void on_ws_disconnect(int client_id, void* ud) {
    (void)ud; (void)client_id;
    ZST_LOG_INFO("sender", "Browser disconnected");
    pthread_mutex_lock(&g_lock);
    g_client   = -1;
    g_answered = false;
    pthread_mutex_unlock(&g_lock);
}

static void on_ws_message(int client_id, const char* msg, size_t len, void* ud) {
    (void)ud; (void)len;
    char type[32] = {0}, sdp[16384] = {0};
    char candidate[2048] = {0}, mid[8] = {0}, mline[8] = {0};

    if (!json_get(msg, "type", type, sizeof(type))) return;

    if (strcmp(type, "offer") == 0) {
        if (!json_get(msg, "sdp", sdp, sizeof(sdp))) return;
        ZST_LOG_INFO("sender", "Received SDP offer from browser");

        pthread_mutex_lock(&g_lock);
        bool already = g_answered;
        pthread_mutex_unlock(&g_lock);
        if (already) return;

        /* Set remote description on the WebRTC element */
        pthread_mutex_lock(&g_lock);
        if (g_webrtc) zst_webrtc_set_remote_description(g_webrtc, "offer", sdp);
        pthread_mutex_unlock(&g_lock);

    } else if (strcmp(type, "candidate") == 0) {
        json_get(msg, "candidate", candidate, sizeof(candidate));
        json_get(msg, "sdpMid",    mid,       sizeof(mid));
        json_get(msg, "sdpMLineIndex", mline, sizeof(mline));
        int ml = atoi(mline);
        ZST_LOG_INFO("sender", "Received ICE candidate from browser");
        pthread_mutex_lock(&g_lock);
        if (g_webrtc) zst_webrtc_add_ice_candidate(g_webrtc, mid, ml, candidate);
        pthread_mutex_unlock(&g_lock);
    }
    (void)client_id;
}

/* ── Pipeline bus thread ────────────────────────────────────────────────── */
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
            bool is_answer = ev->as.webrtc_local_description.type &&
                             strcmp(ev->as.webrtc_local_description.type, "answer") == 0;
            if (is_answer) {
                pthread_mutex_lock(&g_lock);
                bool sent = g_answered;
                if (!sent) g_answered = true;
                pthread_mutex_unlock(&g_lock);
                if (sent) { zst_event_destroy(ev); continue; }
            }

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
                ecand,
                ev->as.webrtc_ice_candidate.mid,
                ev->as.webrtc_ice_candidate.mlineindex);
            free(ecand);

            pthread_mutex_lock(&g_lock);
            if (g_client >= 0) zst_ws_send(g_ws, g_client, json, strlen(json));
            pthread_mutex_unlock(&g_lock);

        } else if (ev->type == ZST_EVENT_WEBRTC_REMB) {
            /* GCC/TWCC bandwidth estimate — adapt encoder bitrate */
            unsigned int bps = ev->as.webrtc_remb.bitrate;
            ZST_LOG_DEBUG("sender", "GCC REMB: %u bps", bps);
            pthread_mutex_lock(&g_lock);
            if (g_venc) {
                char br[32]; snprintf(br, sizeof(br), "%u", bps);
                zst_element_set_property(g_venc, "bitrate", br);
            }
            pthread_mutex_unlock(&g_lock);
        }
        zst_event_destroy(ev);
    }
    return NULL;
}

/* ── Embedded HTTP server (serves a minimal HTML page) ─────────────────── */
static const char* HTML_PAGE =
"<!DOCTYPE html><html><head><title>zstreamer WebRTC Sender</title>"
"<style>body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
"video{width:640px;height:480px;background:#000;border:2px solid #444}"
"button{padding:10px 24px;margin:5px;font-size:15px;border:none;border-radius:6px;cursor:pointer}"
"#btn-connect{background:#2ecc71;color:#000}#btn-disconnect{background:#e74c3c;color:#fff}"
"#stats{font-family:monospace;font-size:12px;margin-top:8px;color:#aaa}"
"#log{font-family:monospace;font-size:11px;white-space:pre;height:160px;"
"overflow-y:auto;background:#1a1a1a;padding:8px;margin-top:8px;border-radius:4px}"
"</style></head><body>"
"<h2>🎥 zstreamer WebRTC Sender</h2>"
"<video id='video' autoplay playsinline muted></video><br>"
"<button id='btn-connect' onclick='connect()'>Connect</button>"
"<button id='btn-disconnect' onclick='disconnect()'>Disconnect</button>"
"<div id='stats'>Not connected</div>"
"<div id='log'></div>"
"<script>"
"let pc,ws,statsTimer;"
"function log(m){const d=document.getElementById('log');"
"d.textContent+=new Date().toLocaleTimeString()+' '+m+'\\n';d.scrollTop=d.scrollHeight;}"
"async function connect(){"
"ws=new WebSocket('ws://'+location.hostname+':8080');"
"ws.onopen=async()=>{"
"log('Signaling connected');"
"pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});"
"pc.ontrack=e=>{log('Track: '+e.track.kind);"
"if(e.track.kind==='video'){"
"document.getElementById('video').srcObject=new MediaStream([e.track]);"
"}else if(e.track.kind==='audio'){"
"const a=document.createElement('audio');"
"a.srcObject=new MediaStream([e.track]);a.autoplay=true;"
"document.body.appendChild(a);"
"}};"
"pc.onicecandidate=e=>{if(e.candidate)"
"ws.send(JSON.stringify({type:'candidate',...e.candidate.toJSON(),sdpMLineIndex:e.candidate.sdpMLineIndex}));};"
"pc.oniceconnectionstatechange=()=>log('ICE: '+pc.iceConnectionState);"
"pc.addTransceiver('video',{direction:'recvonly'});"
"pc.addTransceiver('audio',{direction:'recvonly'});"
"const o=await pc.createOffer();await pc.setLocalDescription(o);"
"ws.send(JSON.stringify({type:'offer',sdp:o.sdp}));log('Offer sent');};"
"ws.onmessage=async m=>{"
"const d=JSON.parse(m.data);"
"if(d.type==='answer'){await pc.setRemoteDescription(d);log('Answer received');}"
"else if(d.type==='candidate'){await pc.addIceCandidate(d.candidate);}};"
"ws.onerror=()=>log('WS error');ws.onclose=()=>log('WS closed');"
"statsTimer=setInterval(updateStats,1000);}"
"async function updateStats(){"
"if(!pc)return;"
"const s=await pc.getStats();"
"s.forEach(r=>{"
"if(r.type==='inbound-rtp'&&r.kind==='video'){"
"document.getElementById('stats').textContent="
"'Codec: '+r.decoderImplementation+' | Frames: '+r.framesDecoded+"
"' | Lost: '+r.packetsLost+' | Jitter: '+(r.jitter*1000).toFixed(1)+'ms';}});}"
"function disconnect(){"
"clearInterval(statsTimer);"
"if(pc){pc.close();pc=null;}if(ws){ws.close();ws=null;}"
"document.getElementById('video').srcObject=null;"
"document.getElementById('stats').textContent='Disconnected';log('Disconnected');}"
"</script></body></html>\n";

static void* http_thread_fn(void* arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)g_http_port),
        .sin_addr.s_addr = 0,
    };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZST_LOG_ERROR("sender", "HTTP bind failed on port %d", g_http_port);
        close(srv); return NULL;
    }
    listen(srv, 4);
    ZST_LOG_INFO("sender", "HTTP server on port %d", g_http_port);

    while (g_running) {
        struct timeval tv = {1, 0};
        fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
        if (select(srv + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) continue;
        char req[1024]; recv(cl, req, sizeof(req)-1, 0);
        const char* body = HTML_PAGE;
        size_t blen = strlen(body);
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
        send(cl, hdr, strlen(hdr), 0);
        send(cl, body, blen, 0);
        close(cl);
    }
    close(srv);
    return NULL;
}

/* ── Pipeline ────────────────────────────────────────────────────────────── */
static bool start_pipeline(void) {
    /* Codec → encoder/packetizer mapping */
    const char* enc_type =
        strcmp(g_codec, "VP8") == 0 ? "vp8_encoder" :
        strcmp(g_codec, "VP9") == 0 ? "vp9_encoder" :
        "x264_encoder";

    zst_webrtc_codec_t video_codec =
        strcmp(g_codec, "VP8") == 0 ? ZST_WEBRTC_CODEC_VP8 :
        strcmp(g_codec, "VP9") == 0 ? ZST_WEBRTC_CODEC_VP9 :
        ZST_WEBRTC_CODEC_H264;

    zst_element_t* vsrc   = zst_element_factory_make("videotestsrc");
    zst_element_t* text   = zst_element_factory_make("textoverlay");
    zst_element_t* venc   = zst_element_factory_make(enc_type);
    zst_element_t* asrc   = zst_element_factory_make("audiotestsrc");
    zst_element_t* aenc   = zst_element_factory_make("opusenc");
    zst_element_t* webrtc = zst_element_factory_make("webrtc_endpoint");
    if (!vsrc || !text || !venc || !asrc || !aenc || !webrtc) {
        ZST_LOG_ERROR("sender", "Failed to create elements");
        return false;
    }

    /* Video source */
    zst_element_set_property_int(vsrc, "width",  g_width);
    zst_element_set_property_int(vsrc, "height", g_height);
    zst_element_set_property_int(vsrc, "fps",    g_fps);
    zst_element_set_property_string(vsrc, "pattern", "bars");
    zst_element_set_property_bool(vsrc, "real-time-pacing", true);

    /* Text overlay — show codec and timestamp */
    char label[64];
    snprintf(label, sizeof(label), "zstreamer WebRTC [%s]", g_codec);
    zst_element_set_property_string(text, "text", label);
    zst_element_set_property_bool(text, "timecode", true);
    zst_element_set_property_int(text, "font-size", 28);
    zst_element_set_property_int(text, "x", 16);
    zst_element_set_property_int(text, "y", 32);

    /* Video encoder */
    zst_element_set_property_int(venc, "gop-size", g_fps);
    zst_element_set_property_int(venc, "bitrate",  g_bitrate);
    if (video_codec == ZST_WEBRTC_CODEC_H264)
        zst_element_set_property_string(venc, "profile", "baseline");

    /* Audio source */
    zst_element_set_property_int(asrc, "sample-rate", 48000);
    zst_element_set_property_int(asrc, "channels", 2);
    zst_element_set_property_string(asrc, "sample-format", "S16LE");
    zst_element_set_property_string(asrc, "wave", "sine");
    zst_element_set_property_int(asrc, "frequency", 440);
    zst_element_set_property_int(asrc, "samples-per-buffer", 960);
    zst_element_set_property_bool(asrc, "real-time-pacing", true);

    /* WebRTC endpoint */
    if (g_stun[0]) zst_element_set_property_string(webrtc, "stun-servers", g_stun);
    zst_webrtc_add_video_track(webrtc, video_codec, 96, "video0");
    zst_webrtc_add_audio_track(webrtc, ZST_WEBRTC_CODEC_OPUS, 111, "audio0");

    /* Build pipeline */
    g_pipeline = zst_pipeline_create();
    zst_pipeline_add(g_pipeline, vsrc);
    zst_pipeline_add(g_pipeline, text);
    zst_pipeline_add(g_pipeline, venc);
    zst_pipeline_add(g_pipeline, asrc);
    zst_pipeline_add(g_pipeline, aenc);
    zst_pipeline_add(g_pipeline, webrtc);

    /* Link: vsrc → text → venc → webrtc(sink_video_0) */
    zst_pad_link(zst_element_get_pad(vsrc,  "src"),          zst_element_get_pad(text,   "sink"));
    zst_pad_link(zst_element_get_pad(text,  "src"),          zst_element_get_pad(venc,   "sink"));
    zst_pad_link(zst_element_get_pad(venc,  "src"),          zst_element_get_pad(webrtc, "sink_video_0"));
    /* Link: asrc → aenc → webrtc(sink_audio_1) */
    zst_pad_link(zst_element_get_pad(asrc,  "src"),          zst_element_get_pad(aenc,   "sink"));
    zst_pad_link(zst_element_get_pad(aenc,  "src"),          zst_element_get_pad(webrtc, "sink_audio_1"));

    g_webrtc = webrtc;
    g_venc   = venc;

    /* Clock + scheduler */
    zst_clock_t* clock = zst_clock_system_create();
    zst_pipeline_set_clock(g_pipeline, clock);
    zst_clock_unref(clock);

    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 3 };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, g_pipeline);
    zst_pipeline_set_state(g_pipeline, ZST_STATE_PLAYING);
    zst_scheduler_run(g_scheduler);

    ZST_LOG_INFO("sender", "Pipeline started: %dx%d @%dfps %s", g_width, g_height, g_fps, g_codec);
    return true;
}

static void stop_pipeline(void) {
    pthread_mutex_lock(&g_lock);
    if (g_scheduler) {
        zst_scheduler_stop(g_scheduler);
        zst_scheduler_destroy(g_scheduler);
        g_scheduler = NULL;
    }
    if (g_pipeline) {
        zst_pipeline_set_state(g_pipeline, ZST_STATE_NULL);
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
    }
    g_webrtc = NULL; g_venc = NULL;
    pthread_mutex_unlock(&g_lock);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port")      && i+1 < argc) g_ws_port   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--http-port") && i+1 < argc) g_http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stun")     && i+1 < argc) snprintf(g_stun, sizeof(g_stun), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--codec")    && i+1 < argc) snprintf(g_codec, sizeof(g_codec), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--width")    && i+1 < argc) g_width   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height")   && i+1 < argc) g_height  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps")      && i+1 < argc) g_fps     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate")  && i+1 < argc) g_bitrate = atoi(argv[++i]);
        else {
            printf("Usage: %s [--port WS] [--http-port HTTP] [--stun URL]\n"
                   "          [--codec H264|VP8|VP9] [--width W] [--height H]\n"
                   "          [--fps F] [--bitrate BPS]\n", argv[0]);
            return 1;
        }
    }

    zst_register_builtin_elements();

    /* Start pipeline */
    if (!start_pipeline()) return 1;

    /* HTTP server for the control page */
    pthread_create(&g_http_thread, NULL, http_thread_fn, NULL);

    /* WebSocket signaling server */
    g_ws = zst_ws_server_create(g_ws_port);
    zst_ws_server_set_callbacks(g_ws, on_ws_connect, on_ws_message, on_ws_disconnect, NULL);
    if (zst_ws_server_start(g_ws) != ZST_OK) {
        ZST_LOG_ERROR("sender", "WebSocket server failed to start on port %d", g_ws_port);
        return 1;
    }

    /* Bus event handler thread */
    pthread_create(&g_bus_thread, NULL, bus_thread_fn, NULL);

    printf("\n"
           "╔══════════════════════════════════════════╗\n"
           "║  zstreamer WebRTC Sender                 ║\n"
           "╠══════════════════════════════════════════╣\n"
           "║  Control page:  http://localhost:%d      ║\n"
           "║  Signaling WS:  ws://localhost:%d        ║\n"
           "║  Codec:         %-10s                ║\n"
           "║  Resolution:    %dx%d @%dfps             ║\n"
           "║  Press Ctrl+C to stop                    ║\n"
           "╚══════════════════════════════════════════╝\n\n",
           g_http_port, g_ws_port, g_codec, g_width, g_height, g_fps);

    /* Wait for shutdown */
    while (g_running) usleep(100000);

    ZST_LOG_INFO("sender", "Shutting down...");
    zst_ws_server_stop(g_ws);
    pthread_join(g_bus_thread, NULL);
    pthread_join(g_http_thread, NULL);
    stop_pipeline();
    return 0;
}
