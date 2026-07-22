/*=============================================================================
    server.c — WebRTC Chrome Demo Server

    Runs a real-time pipeline (videotestsrc → x264enc → webrtc_endpoint)
    and serves index.html via an embedded HTTP server, relaying SDP and
    ICE candidates via WebSockets.
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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

/* ── Fallback HTML Page ─────────────────────────────────────────────────── */
static const char* g_fallback_html = 
"<!DOCTYPE html>\n<html>\n<head>\n<title>zstreamer Fallback</title>\n"
"</head>\n<body>\n<h1>zstreamer WebRTC Control Panel Fallback</h1>\n"
"<p>Please open examples/webrtc_chrome/index.html in your browser.</p>\n"
"</body>\n</html>\n";

/* ── Global State ───────────────────────────────────────────────────────── */
static zst_ws_server_t* g_ws_server = NULL;
static int              g_ws_port = 8080;
static int              g_http_port = 8000;
static char             g_stun_server[256] = "stun:stun.l.google.com:19302";
static char             g_turn_server[256] = "";
static char             g_turn_username[128] = "";
static char             g_turn_password[128] = "";

static pthread_t        g_http_thread;
static pthread_t        g_bus_thread;
static volatile bool    g_running = true;

static pthread_mutex_t  g_pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static zst_pipeline_t*  g_pipeline = NULL;
static zst_scheduler_t* g_scheduler = NULL;
static zst_element_t*   g_webrtc_el = NULL;
static int              g_active_client = -1;
static bool             g_answer_sent = false;

/* ── Lightweight JSON Helper ────────────────────────────────────────────── */
static int get_json_string(const char* json, const char* key, char* out, size_t max_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return 0;
    
    p = strchr(p + strlen(search), ':');
    if (!p) return 0;
    p++;
    
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    
    if (*p == '"') {
        p++;
        const char* end = p;
        while (*end && *end != '"') {
            if (*end == '\\') end += 2;
            else end++;
        }
        size_t len = end - p;
        if (len >= max_len) len = max_len - 1;
        
        size_t out_idx = 0;
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '\\' && i + 1 < len) {
                if (p[i+1] == 'n') out[out_idx++] = '\n';
                else if (p[i+1] == 'r') out[out_idx++] = '\r';
                else if (p[i+1] == 't') out[out_idx++] = '\t';
                else out[out_idx++] = p[i+1];
                i++;
            } else {
                out[out_idx++] = p[i];
            }
        }
        out[out_idx] = '\0';
        return 1;
    } else {
        const char* end = p;
        while (*end && *end != ',' && *end != '}' && *end != ']') end++;
        size_t len = end - p;
        if (len >= max_len) len = max_len - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        char* trim = out + len - 1;
        while (trim >= out && (*trim == ' ' || *trim == '\t' || *trim == '\r' || *trim == '\n')) {
            *trim = '\0';
            trim--;
        }
        return 1;
    }
}

/*
 * sdp_patch_h264_fmtp — inject/replace a=fmtp lines in the SDP answer.
 *
 * libdatachannel may omit a=fmtp for H264 or include an incomplete one.
 * Chrome requires profile-level-id=42e01e (Constrained Baseline 3.1)
 * to match the encoder. Without it Chrome silently rejects every decoded frame.
 *
 * Strategy: copy lines as-is; when we see an H264 rtpmap, peek ahead to see
 * if a matching fmtp follows. If not, inject one. If it does, replace it
 * if it's missing profile-level-id.
 *
 * Returns a newly malloc'd patched SDP string (caller must free), or NULL.
 */
static char*
sdp_patch_h264_fmtp(const char* sdp)
{
    if (!sdp) return NULL;

    ZST_LOG_INFO("server", "sdp_patch_h264_fmtp: input SDP length=%zu", strlen(sdp));

    size_t in_len  = strlen(sdp);
    size_t out_cap = in_len * 2 + 1024;
    char*  out     = malloc(out_cap);
    if (!out) return NULL;

    size_t pos = 0;
    const char* p = sdp;

    while (*p) {
        /* Find end of current line. */
        const char* nl = strpbrk(p, "\r\n");
        size_t line_len, nl_len;
        if (nl) {
            line_len = (size_t)(nl - p);
            nl_len   = (nl[0] == '\r' && nl[1] == '\n') ? 2 : 1;
        } else {
            line_len = strlen(p);
            nl_len   = 0;
        }

        /* Helper: append raw bytes to output */
        #define APPEND_RAW(ptr, len) do {                           \
            while (pos + (len) + 16 >= out_cap) {                   \
                out_cap *= 2;                                       \
                char* _tmp = realloc(out, out_cap);                 \
                if (!_tmp) { free(out); return NULL; }              \
                out = _tmp;                                         \
            }                                                      \
            memcpy(out + pos, (ptr), (len));                        \
            pos += (len);                                          \
        } while(0)

        /* Detect H264 rtpmap line */
        unsigned int pt = 0;
        if (sscanf(p, "a=rtpmap:%u", &pt) == 1 && strstr(p, "H264/90000") != NULL) {
            /* Copy the rtpmap line */
            APPEND_RAW(p, line_len + nl_len);

            /*
             * Peek ahead: is the NEXT non-empty line an fmtp for this same pt?
             */
            const char* peek = p + line_len + nl_len;

            /* Skip empty lines */
            while (*peek == '\r' || *peek == '\n') peek++;
            if (*peek == '\0') {
                /* No more lines — just fall through to inject fmtp */
            }

            /* Compute peek line length */
            size_t peek_len = 0;
            const char* peek_nl = strpbrk(peek, "\r\n");
            if (peek_nl) {
                peek_len = (size_t)(peek_nl - peek);
            } else {
                peek_len = strlen(peek);
            }

            unsigned int peek_pt = 0;
            bool peek_is_fmtp = (sscanf(peek, "a=fmtp:%u", &peek_pt) == 1 &&
                                 peek_pt == pt);

            if (peek_is_fmtp && strstr(peek, "profile-level-id=42e01e")) {
                /* fmtp already correct — it will be copied in the next iteration */
                ZST_LOG_INFO("server", "sdp_patch_h264_fmtp: fmtp for pt=%u already has profile-level-id", pt);
            } else if (peek_is_fmtp) {
                /* fmtp exists but needs fixing — replace it */
                size_t peek_nl_len = 0;
                if (peek_nl) {
                    peek_nl_len = (peek_nl[0] == '\r' && peek_nl[1] == '\n') ? 2 : 1;
                }
                char fmtp[128];
                int n = snprintf(fmtp, sizeof(fmtp),
                    "a=fmtp:%u profile-level-id=42e01e;"
                    "packetization-mode=1;"
                    "level-asymmetry-allowed=1",
                    pt);
                APPEND_RAW(fmtp, (size_t)n);
                if (peek_nl_len > 0) {
                    APPEND_RAW(peek + peek_len, peek_nl_len);
                } else {
                    out[pos++] = '\n';
                }
                ZST_LOG_INFO("server", "sdp_patch_h264_fmtp: replaced fmtp for pt=%u", pt);
                /* Skip past the old fmtp line in the input */
                p = peek + peek_len;
                if (peek_nl_len > 0) p += peek_nl_len;
                continue;
            } else {
                /* No fmtp follows — inject one */
                char fmtp[128];
                int n = snprintf(fmtp, sizeof(fmtp),
                    "a=fmtp:%u profile-level-id=42e01e;"
                    "packetization-mode=1;"
                    "level-asymmetry-allowed=1\r\n",
                    pt);
                APPEND_RAW(fmtp, (size_t)n);
                ZST_LOG_INFO("server", "sdp_patch_h264_fmtp: injected fmtp for pt=%u", pt);
            }
        } else {
            /* Copy this line verbatim */
            APPEND_RAW(p, line_len + nl_len);
        }

        #undef APPEND_RAW

        if (!nl) break;
        p = nl + nl_len;
    }

    out[pos] = '\0';
    return out;
}

static char* escape_json_string(const char* src) {
    if (!src) return strdup("");
    size_t len = strlen(src);
    char* dst = malloc(len * 2 + 1);
    if (!dst) return NULL;
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\') {
            dst[j++] = '\\'; dst[j++] = '\\';
        } else if (src[i] == '"') {
            dst[j++] = '\\'; dst[j++] = '"';
        } else if (src[i] == '\n') {
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (src[i] == '\r') {
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (src[i] == '\t') {
            dst[j++] = '\\'; dst[j++] = 't';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return dst;
}

/* ── HTTP Server Thread ─────────────────────────────────────────────────── */
static void* http_server_thread_fn(void* arg) {
    int port = *(int*)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        ZST_LOG_ERROR("http_server", "Socket creation failed: %s", strerror(errno));
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZST_LOG_ERROR("http_server", "Bind to port %d failed: %s", port, strerror(errno));
        close(server_fd);
        return NULL;
    }
    
    if (listen(server_fd, 10) < 0) {
        ZST_LOG_ERROR("http_server", "Listen failed: %s", strerror(errno));
        close(server_fd);
        return NULL;
    }
    
    ZST_LOG_INFO("http_server", "Server listening on port %d", port);
    
    while (g_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int ret = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0 && errno != EINTR) {
            break;
        }
        if (ret <= 0 || !FD_ISSET(server_fd, &readfds)) {
            continue;
        }

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        char buf[2048];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            
            if (strncmp(buf, "GET", 3) == 0) {
                FILE* f = fopen("examples/webrtc_chrome/index.html", "r");
                if (!f) f = fopen("../examples/webrtc_chrome/index.html", "r");
                if (!f) f = fopen("/workspace/examples/webrtc_chrome/index.html", "r");
                if (!f) f = fopen("index.html", "r");
                
                char* content = NULL;
                size_t size = 0;
                
                if (f) {
                    fseek(f, 0, SEEK_END);
                    size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    content = malloc(size + 1);
                    if (content) {
                        size_t read_bytes = fread(content, 1, size, f);
                        content[read_bytes] = '\0';
                        size = read_bytes;
                    }
                    fclose(f);
                }
                
                if (!content) {
                    content = strdup(g_fallback_html);
                    size = strlen(content);
                }
                
                char resp_hdr[512];
                snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html\r\n"
                         "Content-Length: %zu\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n\r\n",
                         size);
                         
                send(client_fd, resp_hdr, strlen(resp_hdr), 0);
                send(client_fd, content, size, 0);
                free(content);
            } else {
                const char* not_found = 
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send(client_fd, not_found, strlen(not_found), 0);
            }
        }
        close(client_fd);
    }
    
    close(server_fd);
    return NULL;
}

/* ── Pipeline Event Loop / Bus Handler ──────────────────────────────────── */
static void* bus_thread_fn(void* arg) {
    (void)arg;
    ZST_LOG_INFO("bus_thread", "Pipeline event handler thread started");
    
    while (g_running) {
        zst_event_t* ev = NULL;
        zst_result_t r = ZST_ERROR;

        pthread_mutex_lock(&g_pipe_lock);
        if (g_pipeline && g_pipeline->bus) {
            r = zst_bus_pop(g_pipeline->bus, &ev, 0);
        }
        pthread_mutex_unlock(&g_pipe_lock);
        
        if (r != ZST_OK || !ev) {
            usleep(50000);
            continue;
        }

        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION) {
                if (ev->as.webrtc_local_description.type && strcmp(ev->as.webrtc_local_description.type, "answer") == 0) {
                    pthread_mutex_lock(&g_pipe_lock);
                    if (g_answer_sent) {
                        ZST_LOG_INFO("bus_thread", "SDP answer already sent, skipping duplicate");
                        pthread_mutex_unlock(&g_pipe_lock);
                        zst_event_destroy(ev);
                        continue;
                    }
                    g_answer_sent = true;
                    pthread_mutex_unlock(&g_pipe_lock);
                }
                
                ZST_LOG_INFO("bus_thread", "Received local SDP answer from WebRTC endpoint");
                printf("--- LOCAL SDP ANSWER (before patch) ---\n%s\n----------------------------------------\n", ev->as.webrtc_local_description.sdp);
                fflush(stdout);
                
                char selected_video[32] = {0};
                char selected_audio[32] = {0};
                
                pthread_mutex_lock(&g_pipe_lock);
                if (g_webrtc_el) {
                    zst_element_get_property(g_webrtc_el, "selected-video-codec", selected_video, sizeof(selected_video));
                    zst_element_get_property(g_webrtc_el, "selected-audio-codec", selected_audio, sizeof(selected_audio));
                }
                pthread_mutex_unlock(&g_pipe_lock);
                
                char* patched_sdp = sdp_patch_h264_fmtp(ev->as.webrtc_local_description.sdp);
                const char* sdp_to_send = patched_sdp ? patched_sdp : ev->as.webrtc_local_description.sdp;
                ZST_LOG_INFO("bus_thread", "Patched SDP:");
                printf("--- PATCHED SDP (sent to browser) ---\n%s\n-------------------------------------\n", sdp_to_send);
                fflush(stdout);
                char* escaped_sdp = escape_json_string(sdp_to_send);
                free(patched_sdp);
                if (escaped_sdp) {
                    size_t len = strlen(escaped_sdp) + 512;
                    char* json = malloc(len);
                    if (json) {
                        snprintf(json, len,
                                 "{\"type\":\"answer\",\"sdp\":\"%s\","
                                 "\"selected_video_codec\":\"%s\","
                                 "\"selected_audio_codec\":\"%s\"}",
                                 escaped_sdp, selected_video, selected_audio);
                        
                        pthread_mutex_lock(&g_pipe_lock);
                        if (g_active_client >= 0) {
                            zst_ws_send(g_ws_server, g_active_client, json, strlen(json));
                        }
                        pthread_mutex_unlock(&g_pipe_lock);
                        free(json);
                    }
                    free(escaped_sdp);
                }
            } else if (ev->type == ZST_EVENT_WEBRTC_ICE_CANDIDATE) {
                ZST_LOG_INFO("bus_thread", "Received local ICE candidate");
                
                char* escaped_cand = escape_json_string(ev->as.webrtc_ice_candidate.candidate);
                if (escaped_cand) {
                    char json[2048];
                    snprintf(json, sizeof(json),
                             "{\"type\":\"candidate\",\"candidate\":{"
                             "\"candidate\":\"%s\","
                             "\"sdpMid\":\"%s\","
                             "\"sdpMLineIndex\":%d"
                             "}}",
                             escaped_cand,
                             ev->as.webrtc_ice_candidate.mid,
                             ev->as.webrtc_ice_candidate.mlineindex);
                    
                    pthread_mutex_lock(&g_pipe_lock);
                    if (g_active_client >= 0) {
                        zst_ws_send(g_ws_server, g_active_client, json, strlen(json));
                    }
                    pthread_mutex_unlock(&g_pipe_lock);
                    free(escaped_cand);
                }
            }
            zst_event_destroy(ev);
        }
    }
    return NULL;
}

/* ── Pipeline Creation / Teardown ───────────────────────────────────────── */
static void stop_pipeline(void) {
    pthread_mutex_lock(&g_pipe_lock);
    if (g_scheduler) {
        zst_scheduler_stop(g_scheduler);
        zst_scheduler_destroy(g_scheduler);
        g_scheduler = NULL;
    }
    if (g_pipeline) {
        ZST_LOG_INFO("server", "Stopping active WebRTC pipeline...");
        zst_pipeline_set_state(g_pipeline, ZST_STATE_NULL);
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
    }
    pthread_mutex_unlock(&g_pipe_lock);
}

/* Parse Chrome's offer SDP to find PT for a given codec name.
 * Returns the PT number, or -1 if not found. */
static int find_offer_pt(const char* sdp, const char* codec_pattern) {
    if (!sdp || !codec_pattern) return -1;
    const char* p = sdp;
    while (*p) {
        const char* nl = strpbrk(p, "\r\n");
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        if (line_len > 10 && strncmp(p, "a=rtpmap:", 9) == 0) {
            char* line = strndup(p, line_len);
            if (line) {
                if (strstr(line, codec_pattern)) {
                    unsigned int pt = 0;
                    if (sscanf(p + 9, "%u", &pt) == 1) {
                        ZST_LOG_INFO("server", "find_offer_pt: found %s at pt=%u", codec_pattern, pt);
                        free(line);
                        return (int)pt;
                    }
                }
                free(line);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return -1;
}

static bool start_pipeline(const char* codec_preference, const char* offer_sdp) {
    stop_pipeline();
    
    pthread_mutex_lock(&g_pipe_lock);
    g_answer_sent = false;

    const char* venc_name = "x264enc";
    int video_codec = ZST_WEBRTC_CODEC_H264;

    if (codec_preference && strstr(codec_preference, "VP8") == codec_preference) {
        venc_name = "vp8enc";
        video_codec = ZST_WEBRTC_CODEC_VP8;
    } else if (codec_preference && strstr(codec_preference, "VP9") == codec_preference) {
        venc_name = "vp9enc";
        video_codec = ZST_WEBRTC_CODEC_VP9;
    }

    ZST_LOG_INFO("server", "Creating new pipeline with %s -> webrtc_endpoint", venc_name);
    
    g_pipeline = zst_pipeline_create();
    if (!g_pipeline) {
        ZST_LOG_ERROR("server", "Failed to create pipeline");
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
    zst_element_t* text = zst_element_factory_make("textoverlay");
    zst_element_t* venc = zst_element_factory_make(venc_name);
    zst_element_t* asrc = zst_element_factory_make("audiotestsrc");
    zst_element_t* aenc = zst_element_factory_make("opusenc");
    zst_element_t* webrtc = zst_element_factory_make("webrtc_endpoint");
    
    if (!vsrc || !text || !venc || !asrc || !aenc || !webrtc) {
        ZST_LOG_ERROR("server", "Failed to create pipeline elements");
        if (vsrc) zst_element_destroy(vsrc);
        if (text) zst_element_destroy(text);
        if (venc) zst_element_destroy(venc);
        if (asrc) zst_element_destroy(asrc);
        if (aenc) zst_element_destroy(aenc);
        if (webrtc) zst_element_destroy(webrtc);
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    g_webrtc_el = webrtc;
    
    /* Configure videotestsrc for smooth real-time generation */
    zst_element_set_property_int(vsrc, "width", 640);
    zst_element_set_property_int(vsrc, "height", 480);
    zst_element_set_property_int(vsrc, "fps", 30);
    zst_element_set_property_string(vsrc, "pattern", "bars");
    zst_element_set_property_bool(vsrc, "use-clock", false);
    zst_element_set_property_bool(vsrc, "real-time-pacing", true);
    
    /* Configure textoverlay to show timecode/frame count */
    zst_element_set_property_string(text, "text", "WebRTC Test Stream");
    zst_element_set_property_bool(text, "timecode", true);
    zst_element_set_property_int(text, "font-size", 32);
    zst_element_set_property_int(text, "x", 20);
    zst_element_set_property_int(text, "y", 40);
    
    /* Configure encoder for low latency real-time streaming. */
    zst_element_set_property_int(venc, "gop-size", 30);
    if (video_codec == ZST_WEBRTC_CODEC_H264) {
        /* MUST use "baseline" profile to match libdatachannel's SDP
         * profile-level-id=42e01f (Baseline Level 3.1). High profile
         * bitstream would be rejected by Chrome. */
        zst_element_set_property_string(venc, "profile", "baseline");
    }

    /* Configure audiotestsrc for WebRTC Opus audio format */
    zst_element_set_property_int(asrc, "sample-rate", 48000);
    zst_element_set_property_int(asrc, "channels", 2);
    zst_element_set_property_string(asrc, "sample-format", "S16LE");
    zst_element_set_property_string(asrc, "wave", "sine");
    zst_element_set_property_int(asrc, "frequency", 440);
    zst_element_set_property_int(asrc, "samples-per-buffer", 960); /* 20ms of audio */
    zst_element_set_property_bool(asrc, "use-clock", false);
    zst_element_set_property_bool(asrc, "real-time-pacing", true);
    
    /* Configure webrtc_endpoint */
    if (strlen(g_stun_server) > 0) {
        zst_element_set_property_string(webrtc, "stun-servers", g_stun_server);
    }
    if (strlen(g_turn_server) > 0) {
        zst_element_set_property_string(webrtc, "turn-servers", g_turn_server);
    }
    if (strlen(g_turn_username) > 0) {
        zst_element_set_property_string(webrtc, "turn-username", g_turn_username);
    }
    if (strlen(g_turn_password) > 0) {
        zst_element_set_property_string(webrtc, "turn-password", g_turn_password);
    }
    if (codec_preference && strlen(codec_preference) > 0) {
        zst_element_set_property_string(webrtc, "codec-preference", codec_preference);
    }
    
    zst_pipeline_add(g_pipeline, vsrc);
    zst_pipeline_add(g_pipeline, text);
    zst_pipeline_add(g_pipeline, venc);
    zst_pipeline_add(g_pipeline, asrc);
    zst_pipeline_add(g_pipeline, aenc);
    zst_pipeline_add(g_pipeline, webrtc);
    
    /* Open endpoint to ready state to enable adding tracks */
    if (zst_element_set_state(webrtc, ZST_STATE_READY) != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to transition webrtc endpoint to READY");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    /* Add video track — use PT from Chrome's offer */
    int offer_pt = -1;
    if (offer_sdp) {
        if (video_codec == ZST_WEBRTC_CODEC_H264) {
            offer_pt = find_offer_pt(offer_sdp, "H264/90000");
        } else if (video_codec == ZST_WEBRTC_CODEC_VP8) {
            offer_pt = find_offer_pt(offer_sdp, "VP8/90000");
        } else if (video_codec == ZST_WEBRTC_CODEC_VP9) {
            offer_pt = find_offer_pt(offer_sdp, "VP9/90000");
        }
    }
    if (offer_pt >= 0) {
        ZST_LOG_INFO("server", "Using offer pt=%d for video track", offer_pt);
        if (zst_webrtc_add_video_track_with_pt(webrtc, video_codec, 12345, "0", offer_pt) != ZST_OK) {
            ZST_LOG_ERROR("server", "Failed to add video track to webrtc endpoint");
            zst_pipeline_destroy(g_pipeline);
            g_pipeline = NULL;
            g_webrtc_el = NULL;
            pthread_mutex_unlock(&g_pipe_lock);
            return false;
        }
    } else {
        if (zst_webrtc_add_video_track(webrtc, video_codec, 12345, "0") != ZST_OK) {
            ZST_LOG_ERROR("server", "Failed to add video track to webrtc endpoint");
            zst_pipeline_destroy(g_pipeline);
            g_pipeline = NULL;
            g_webrtc_el = NULL;
            pthread_mutex_unlock(&g_pipe_lock);
            return false;
        }
    }

    /* Add Opus audio track */
    if (zst_webrtc_add_audio_track(webrtc, ZST_WEBRTC_CODEC_OPUS, 22222, "1") != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to add audio track to webrtc endpoint");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    /* Link videotestsrc → textoverlay → x264enc → webrtc_endpoint(sink_video_0) */
    zst_pad_t* vsrc_src = zst_element_get_pad(vsrc, "src");
    zst_pad_t* text_sink = zst_element_get_pad(text, "sink");
    zst_pad_t* text_src = zst_element_get_pad(text, "src");
    zst_pad_t* venc_sink = zst_element_get_pad(venc, "sink");
    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* webrtc_video_sink = zst_element_get_pad(webrtc, "sink_video_0");

    /* Link audiotestsrc → opusenc → webrtc_endpoint(sink_audio_1) */
    zst_pad_t* asrc_src = zst_element_get_pad(asrc, "src");
    zst_pad_t* aenc_sink = zst_element_get_pad(aenc, "sink");
    zst_pad_t* aenc_src = zst_element_get_pad(aenc, "src");
    zst_pad_t* webrtc_audio_sink = zst_element_get_pad(webrtc, "sink_audio_1");
    
    if (!vsrc_src || !text_sink || !text_src || !venc_sink || !venc_src || !webrtc_video_sink ||
        !asrc_src || !aenc_sink || !aenc_src || !webrtc_audio_sink) {
        ZST_LOG_ERROR("server", "Failed to retrieve pads for linking");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    if (zst_pad_link(vsrc_src, text_sink) != ZST_OK ||
        zst_pad_link(text_src, venc_sink) != ZST_OK ||
        zst_pad_link(venc_src, webrtc_video_sink) != ZST_OK ||
        zst_pad_link(asrc_src, aenc_sink) != ZST_OK ||
        zst_pad_link(aenc_src, webrtc_audio_sink) != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to link elements");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    /* Create clock for pacing and assign it */
    zst_clock_t* clock = zst_clock_system_create();
    zst_pipeline_set_clock(g_pipeline, clock);
    zst_clock_unref(clock);

    /* Create scheduler */
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2,
    };
    g_scheduler = zst_scheduler_create(&cfg);
    if (!g_scheduler) {
        ZST_LOG_ERROR("server", "Failed to create scheduler");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    zst_scheduler_attach(g_scheduler, g_pipeline);
    zst_scheduler_run(g_scheduler);
    
    pthread_mutex_unlock(&g_pipe_lock);
    return true;
}

/* ── WebSocket Callbacks ────────────────────────────────────────────────── */
static void on_ws_connect(int client_id, void* user_data) {
    (void)user_data;
    ZST_LOG_INFO("server", "Browser signaling client connected, ID=%d", client_id);
    
    pthread_mutex_lock(&g_pipe_lock);
    g_active_client = client_id;
    pthread_mutex_unlock(&g_pipe_lock);
}

static void on_ws_disconnect(int client_id, void* user_data) {
    (void)user_data;
    ZST_LOG_INFO("server", "Browser signaling client disconnected, ID=%d", client_id);
    
    pthread_mutex_lock(&g_pipe_lock);
    if (g_active_client == client_id) {
        g_active_client = -1;
    }
    pthread_mutex_unlock(&g_pipe_lock);
    
    stop_pipeline();
}

static void on_ws_message(int client_id, const char* msg, size_t len, void* user_data) {
    (void)user_data;
    (void)len;
    
    char type[64] = {0};
    if (!get_json_string(msg, "type", type, sizeof(type))) {
        ZST_LOG_WARN("server", "Received malformed JSON message");
        return;
    }
    
    if (strcmp(type, "offer") == 0) {
        char sdp[8192] = {0};
        char codec_pref[128] = {0};
        
        if (!get_json_string(msg, "sdp", sdp, sizeof(sdp))) {
            ZST_LOG_ERROR("server", "Offer received without SDP content");
            return;
        }
        
        get_json_string(msg, "codec_preference", codec_pref, sizeof(codec_pref));
        ZST_LOG_INFO("server", "Signaling offer received. Preference: %s",
                     codec_pref[0] ? codec_pref : "(default)");
        printf("--- CHROME OFFER SDP ---\n%s\n------------------------\n", sdp);
        fflush(stdout);
        
        if (!start_pipeline(codec_pref, sdp)) {
            ZST_LOG_ERROR("server", "Could not start loopback pipeline");
            return;
        }
        
        pthread_mutex_lock(&g_pipe_lock);
        if (g_webrtc_el && g_pipeline) {
            zst_result_t res = zst_webrtc_set_remote_description(g_webrtc_el, "offer", sdp);
            if (res == ZST_OK) {
                ZST_LOG_INFO("server", "Remote offer description successfully set");
                zst_pipeline_set_state(g_pipeline, ZST_STATE_PLAYING);
            } else {
                ZST_LOG_ERROR("server", "Failed to set remote offer description");
            }
        }
        pthread_mutex_unlock(&g_pipe_lock);
        
    } else if (strcmp(type, "candidate") == 0) {
        char candidate[2048] = {0};
        char mid[64] = {0};
        char mline_str[32] = {0};
        
        /* Find candidate nested field */
        const char* cand_obj = strstr(msg, "\"candidate\"");
        if (cand_obj) {
            const char* cand_str = strstr(cand_obj + 11, "\"candidate\"");
            if (cand_str) {
                get_json_string(cand_str, "candidate", candidate, sizeof(candidate));
            } else {
                /* Try fallback straight search */
                get_json_string(msg, "candidate", candidate, sizeof(candidate));
            }
        }
        
        get_json_string(msg, "sdpMid", mid, sizeof(mid));
        get_json_string(msg, "sdpMLineIndex", mline_str, sizeof(mline_str));
        int mlineindex = atoi(mline_str);
        
        pthread_mutex_lock(&g_pipe_lock);
        if (g_webrtc_el) {
            ZST_LOG_INFO("server", "Setting remote candidate mid=%s, index=%d", mid, mlineindex);
            zst_webrtc_add_ice_candidate(g_webrtc_el, mid, mlineindex, candidate);
        }
        pthread_mutex_unlock(&g_pipe_lock);
        
    } else if (strcmp(type, "request_keyframe") == 0) {
        pthread_mutex_lock(&g_pipe_lock);
        if (g_webrtc_el) {
            ZST_LOG_INFO("server", "Keyframe request received from browser");
            zst_webrtc_request_keyframe(g_webrtc_el, 0);
        }
        pthread_mutex_unlock(&g_pipe_lock);
    }
}

/* ── Main Entry ─────────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    zst_log_set_level(ZST_LOG_LEVEL_INFO);
    ZST_LOG_INFO("server", "Starting zstreamer WebRTC Control Panel Server...");
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_ws_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            g_http_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc) {
            snprintf(g_stun_server, sizeof(g_stun_server), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--turn") == 0 && i + 1 < argc) {
            snprintf(g_turn_server, sizeof(g_turn_server), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--turn-user") == 0 && i + 1 < argc) {
            snprintf(g_turn_username, sizeof(g_turn_username), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--turn-pass") == 0 && i + 1 < argc) {
            snprintf(g_turn_password, sizeof(g_turn_password), "%s", argv[++i]);
        } else {
            printf("Usage: %s [--port <ws_port>] [--http-port <http_port>] [--stun <stun_url>] [--turn <turn_url>] [--turn-user <user>] [--turn-pass <pass>]\n", argv[0]);
            return 1;
        }
    }
    
    if (zst_register_builtin_elements() != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to register built-in elements");
        return 1;
    }
    
    /* Start HTTP Server */
    if (pthread_create(&g_http_thread, NULL, http_server_thread_fn, &g_http_port) != 0) {
        ZST_LOG_ERROR("server", "Failed to start HTTP server thread");
        return 1;
    }
    
    /* Start Pipeline Bus Event Thread */
    if (pthread_create(&g_bus_thread, NULL, bus_thread_fn, NULL) != 0) {
        ZST_LOG_ERROR("server", "Failed to start Pipeline Bus thread");
        return 1;
    }
    
    /* Start WebSocket Signaling Server */
    g_ws_server = zst_ws_server_create(g_ws_port);
    if (!g_ws_server) {
        ZST_LOG_ERROR("server", "Failed to create WebSocket signaling server");
        return 1;
    }
    
    zst_ws_server_set_callbacks(g_ws_server, on_ws_connect, on_ws_message, on_ws_disconnect, NULL);
    if (zst_ws_server_start(g_ws_server) != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to start WebSocket signaling server");
        return 1;
    }
    
    ZST_LOG_INFO("server", "WebSocket Signaling Server listening on port %d", g_ws_port);
    ZST_LOG_INFO("server", "Point your browser to http://localhost:%d", g_http_port);
    
    /* Keep running until interrupted */
    while (g_running) {
        usleep(500000);
    }
    
    /* Teardown */
    stop_pipeline();
    zst_ws_server_stop(g_ws_server);
    zst_ws_server_free(g_ws_server);
    pthread_join(g_http_thread, NULL);
    pthread_join(g_bus_thread, NULL);
    
    return 0;
}
