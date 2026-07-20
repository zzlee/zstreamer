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
static zst_element_t*   g_webrtc_el = NULL;
static int              g_active_client = -1;

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
        zst_bus_t* bus = NULL;
        pthread_mutex_lock(&g_pipe_lock);
        if (g_pipeline) {
            bus = g_pipeline->bus;
        }
        pthread_mutex_unlock(&g_pipe_lock);
        
        if (!bus) {
            usleep(100000);
            continue;
        }
        
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(bus, &ev, 100);
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION) {
                ZST_LOG_INFO("bus_thread", "Received local SDP answer from WebRTC endpoint");
                
                char selected_video[32] = {0};
                char selected_audio[32] = {0};
                
                pthread_mutex_lock(&g_pipe_lock);
                if (g_webrtc_el) {
                    zst_element_get_property(g_webrtc_el, "selected-video-codec", selected_video, sizeof(selected_video));
                    zst_element_get_property(g_webrtc_el, "selected-audio-codec", selected_audio, sizeof(selected_audio));
                }
                pthread_mutex_unlock(&g_pipe_lock);
                
                char* escaped_sdp = escape_json_string(ev->as.webrtc_local_description.sdp);
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
    if (g_pipeline) {
        ZST_LOG_INFO("server", "Stopping active WebRTC pipeline...");
        zst_pipeline_set_state(g_pipeline, ZST_STATE_NULL);
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
    }
    pthread_mutex_unlock(&g_pipe_lock);
}

static bool start_pipeline(const char* codec_preference) {
    stop_pipeline();
    
    pthread_mutex_lock(&g_pipe_lock);
    ZST_LOG_INFO("server", "Creating new videotestsrc -> x264enc -> webrtc_endpoint pipeline...");
    
    g_pipeline = zst_pipeline_create();
    if (!g_pipeline) {
        ZST_LOG_ERROR("server", "Failed to create pipeline");
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
    zst_element_t* venc = zst_element_factory_make("x264enc");
    zst_element_t* webrtc = zst_element_factory_make("webrtc_endpoint");
    
    if (!vsrc || !venc || !webrtc) {
        ZST_LOG_ERROR("server", "Failed to create pipeline elements");
        if (vsrc) zst_element_destroy(vsrc);
        if (venc) zst_element_destroy(venc);
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
    
    /* Configure x264enc for low latency real-time streaming */
    zst_element_set_property_int(venc, "gop-size", 30);
    
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
    zst_pipeline_add(g_pipeline, venc);
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
    
    /* Add H.264 video track */
    if (zst_webrtc_add_video_track(webrtc, ZST_WEBRTC_CODEC_H264, 12345, "video0") != ZST_OK) {
        ZST_LOG_ERROR("server", "Failed to add video track to webrtc endpoint");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    /* Link videotestsrc → x264enc → webrtc_endpoint(sink_video_0) */
    zst_pad_t* vsrc_src = zst_element_get_pad(vsrc, "src");
    zst_pad_t* venc_sink = zst_element_get_pad(venc, "sink");
    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* webrtc_sink = zst_element_get_pad(webrtc, "sink_video_0");
    
    if (!vsrc_src || !venc_sink || !venc_src || !webrtc_sink) {
        ZST_LOG_ERROR("server", "Failed to retrieve pads for linking");
        zst_pipeline_destroy(g_pipeline);
        g_pipeline = NULL;
        g_webrtc_el = NULL;
        pthread_mutex_unlock(&g_pipe_lock);
        return false;
    }
    
    if (zst_pad_link(vsrc_src, venc_sink) != ZST_OK ||
        zst_pad_link(venc_src, webrtc_sink) != ZST_OK) {
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
        
        if (!start_pipeline(codec_pref)) {
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
