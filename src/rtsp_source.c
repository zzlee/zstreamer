#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*=============================================================================
    rtsp_source.c — RTSP source element (ireader-style client)

    Manual RTSP client using pure socket I/O instead of FFmpeg's built-in
    RTSP client. Architecture adapted from ireader/media-server's librtsp:

      - TCP connection to RTSP server
      - State machine: INIT → DESCRIBE → SETUP → PLAY → STREAMING
      - SDP parsing to extract media tracks and codec info
      - RTP transport: TCP interleaved ($ + channel + len + data) or
        UDP unicast/multicast (RFC 3550 even/odd port pairs)
      - RTP depacketization: H.264 (RFC 3984), AAC (RFC 3640 MPEG4-Generic)
      - Basic/Digest authentication
      - Worker thread for continuous streaming

    Transport negotiation (RFC 2326 §12.39):
      - SETUP with Transport: RTP/AVP/TCP;interleaved=...   (TCP mode)
      - SETUP with Transport: RTP/AVP;unicast;client_port=.. (UDP mode)
      - SETUP with Transport: RTP/AVP;multicast              (multicast mode)
      - Server echoes back server_port/port for RTCP reports

    References:
      - RFC 2326 (RTSP)
      - RFC 3550 (RTP/RTCP)
      - RFC 3984 (H.264 over RTP)
      - RFC 3640 (AAC over RTP)
      - ireader/media-server (github.com/yuan88yuan/ireader-media-server)
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"

/*===========================================================================
    Constants
===========================================================================*/
#define RTSP_BUF_SIZE      16384
#define RTSP_REQ_SIZE      2048
#define RTSP_SDP_SIZE      4096
#define RTP_MTU            1500
#define MAX_TRACKS         8
#define MAX_FU_ACCUM       150000   /* max FU-A accumulation ~150KB */

/* RTP header (RFC 3550) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  cc:4, x:1, p:1, version:2;
    uint8_t  pt:7, m:1;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr_t;
#pragma pack(pop)

/* H.264 NAL types */
#define H264_NAL_SINGLE_MAX   23
#define H264_NAL_STAP_A       24
#define H264_NAL_FU_A         28
#define H264_NAL_IDR          5
#define H264_NAL_SPS          7
#define H264_NAL_PPS          8

/* Transport type */
#define RTSP_SOURCE_TRANSPORT_TCP        0
#define RTSP_SOURCE_TRANSPORT_UDP        1
#define RTSP_SOURCE_TRANSPORT_MULTICAST  2

/*===========================================================================
    Track info from SDP
===========================================================================*/
typedef struct {
    int      type;          /* 0=none, 1=video, 2=audio */
    int      payload_type;  /* RTP payload type number */
    char     encoding[32];  /* "H264", "MPEG4-GENERIC", etc. */
    int      clock_rate;    /* 90000 for video, sample_rate for audio */
    int      channels;      /* for audio */
    char     fmtp[256];     /* format parameters */
    int      interleaved_rtp;   /* TCP interleaved channel for this track */
    int      interleaved_rtcp;
    /* UDP transport state */
    int      udp_rtp_fd;        /* UDP socket for receiving RTP */
    int      udp_rtcp_fd;       /* UDP socket for receiving RTCP */
    uint16_t client_rtp_port;   /* our (client) RTP port */
    uint16_t client_rtcp_port;  /* our (client) RTCP port */
    uint16_t server_rtp_port;   /* server RTP port (from SETUP response) */
    uint16_t server_rtcp_port;  /* server RTCP port (from SETUP response) */
    int      is_multicast;
    char     multicast_addr[64];
    int      multicast_ttl;
    /* RTCP SR state for NTP/RTP timestamp correlation */
    int      has_sr;
    uint64_t last_ntp_time;
    uint32_t last_rtp_time;

    /* SPS/PPS for H.264 extracted from fmtp or stream */
    uint8_t* extra_data;
    int      extra_size;
} track_info_t;

/*===========================================================================
    RTSP client context
===========================================================================*/
typedef struct {
    /* Connection */
    int      fd;
    char     host[256];
    int      port;
    char     path[512];
    char     username[64];
    char     password[64];

    /* Receive buffer */
    uint8_t  buf[RTSP_BUF_SIZE];
    int      buf_len;
    char     last_response[RTSP_BUF_SIZE];
    int      last_response_len;
    int      interleaved_mode;  /* 1=reading interleaved, 0=reading RTSP response */

    /* RTSP state machine */
    enum {
        STATE_INIT,
        STATE_DESCRIBE_SENT,
        STATE_DESCRIBE_DONE,
        STATE_SETUP_SENT,
        STATE_SETUP_DONE,
        STATE_PLAY_SENT,
        STATE_STREAMING,
        STATE_ERROR
    } state;

    unsigned int cseq;
    char         session_id[64];
    int          session_timeout;  /* seconds */

    /* Authentication */
    int          auth_attempts;
    char         auth_header[1024];
    char         auth_realm[64];
    char         auth_nonce[64];
    char         auth_opaque[64];
    int          auth_scheme;     /* 0=none, 1=Basic, 2=Digest */
    int          auth_nc;

    /* Tracks */
    int          track_count;
    track_info_t tracks[MAX_TRACKS];

    /* Current SETUP/PLAY progress */
    int          setup_progress;

    /* FU-A reassembly buffer (H.264) */
    uint8_t      fu_accum[MAX_FU_ACCUM];
    int          fu_accum_len;
    uint32_t     fu_accum_ts;
    int          fu_accum_ssrc;

    /* Transport type */
    int          transport_type;

    /* RTP timestamp tracking */
    uint64_t     base_pts;
    uint32_t     base_rtp_ts_video;
    uint32_t     base_rtp_ts_audio;

    /* Stats */
    uint64_t     bytes_read;
    int          packets_decoded;

    /* Last NTP time for RTCP SR sync */
    uint64_t     last_ntp_time;
    uint32_t     last_rtp_time;
} rtsp_client_t;

/*===========================================================================
    Element private data
===========================================================================*/
typedef struct {
    char             url[512];
    char             transport[16];  /* "tcp" or "udp" */
    rtsp_client_t    client;
    zst_pad_t*       video_pad;
    zst_pad_t*       audio_pad;
    zst_caps_t*      video_caps;
    zst_caps_t*      audio_caps;
    char             username[64];
    char             password[64];
    int              buffer_size;
    int              reconnect;
    int              reconnect_delay_ms;
    int              max_reconnect_attempts;
    int              keepalive_interval_sec;
    int              running;
    int              lock_initialized;
    int              thread_started;
    pthread_t        thread;
    pthread_mutex_t  lock;  /* protects push to downstream */
} rtsp_source_priv_t;

/*===========================================================================
    Helpers
===========================================================================*/
static uint32_t secure_rand32(void) {
    uint32_t val;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t res = read(fd, &val, sizeof(val));
        close(fd);
        if (res == sizeof(val)) {
            return val;
        }
    }
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static uint32_t rand32(void) { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static void sleep_ms(int delay_ms) {
    if (delay_ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = delay_ms / 1000;
    ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void md5_hex(const char* input, char out[33]) {
    uint8_t digest[16];
    struct AVMD5* md5 = av_md5_alloc();
    if (!md5) {
        out[0] = '\0';
        return;
    }
    av_md5_init(md5);
    av_md5_update(md5, (const uint8_t*)input, (int)strlen(input));
    av_md5_final(md5, digest);
    av_free(md5);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[32] = '\0';
}

static int base64_encode(const uint8_t* in, size_t len, char* out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4 + 1;
    if (!out || out_size < need) return -1;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int remain = (int)(len - i);
        if (remain > 1) v |= (uint32_t)in[i + 1] << 8;
        if (remain > 2) v |= (uint32_t)in[i + 2];
        out[j++] = tbl[(v >> 18) & 0x3f];
        out[j++] = tbl[(v >> 12) & 0x3f];
        out[j++] = remain > 1 ? tbl[(v >> 6) & 0x3f] : '=';
        out[j++] = remain > 2 ? tbl[v & 0x3f] : '=';
    }
    out[j] = '\0';
    return 0;
}

static uint64_t ntp_to_unix_ns(uint64_t ntp) {
    uint32_t sec = (uint32_t)(ntp >> 32);
    uint32_t frac = (uint32_t)(ntp & 0xffffffffu);
    uint64_t unix_sec = sec >= 2208988800U ? (uint64_t)(sec - 2208988800U) : 0;
    uint64_t ns = ((uint64_t)frac * 1000000000ULL) >> 32;
    return unix_sec * 1000000000ULL + ns;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/*===========================================================================
    URL parsing: rtsp://[user:pass@]host[:port]/path
===========================================================================*/
static int parse_rtsp_url(const char* url, rtsp_client_t* cl) {
    const char* p = url;
    if (strncmp(p, "rtsp://", 7) != 0) return -1;
    p += 7;

    /* Optional user:pass@ */
    const char* at = strchr(p, '@');
    const char* slash = strchr(p, '/');
    const char* colon2 = strchr(p, ':');

    if (at && (!slash || at < slash)) {
        const char* colon = strchr(p, ':');
        if (colon && colon < at) {
            size_t ulen = colon - p;
            if (ulen >= sizeof(cl->username)) return -1;
            memcpy(cl->username, p, ulen); cl->username[ulen] = '\0';
            size_t plen = at - colon - 1;
            if (plen >= sizeof(cl->password)) return -1;
            memcpy(cl->password, colon + 1, plen); cl->password[plen] = '\0';
        } else {
            size_t ulen = at - p;
            if (ulen >= sizeof(cl->username)) return -1;
            memcpy(cl->username, p, ulen); cl->username[ulen] = '\0';
        }
        p = at + 1;
    }

    /* host[:port] */
    if (*p == '[') { /* IPv6 */
        const char* end = strchr(p, ']');
        if (!end) return -1;
        size_t hlen = end - p - 1;
        if (hlen >= sizeof(cl->host)) hlen = sizeof(cl->host) - 1;
        memcpy(cl->host, p + 1, hlen); cl->host[hlen] = '\0';
        p = end + 1;
        if (*p == ':') {
            cl->port = atoi(p + 1);
            p = strchr(p, '/');
        }
    } else {
        const char* colon = strchr(p, ':');
        const char* sl = strchr(p, '/');
        if (colon && (!sl || colon < sl)) {
            size_t hlen = colon - p;
            if (hlen >= sizeof(cl->host)) hlen = sizeof(cl->host) - 1;
            memcpy(cl->host, p, hlen); cl->host[hlen] = '\0';
            cl->port = atoi(colon + 1);
            p = sl ? sl : "";
        } else {
            const char* sl2 = strchr(p, '/');
            size_t hlen = sl2 ? (size_t)(sl2 - p) : strlen(p);
            if (hlen >= sizeof(cl->host)) hlen = sizeof(cl->host) - 1;
            memcpy(cl->host, p, hlen); cl->host[hlen] = '\0';
            cl->port = 554;
            p = sl2 ? sl2 : "";
        }
    }

    if (cl->port <= 0) cl->port = 554;
    if (*p == '/') {
        strncpy(cl->path, p, sizeof(cl->path) - 1);
    } else {
        strncpy(cl->path, "/", sizeof(cl->path) - 1);
    }

    return 0;
}

/*===========================================================================
    TCP connect
===========================================================================*/
static int tcp_connect(const char* host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd >= 0) {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }
    return fd;
}

/*===========================================================================
    Send RTSP request and receive response
===========================================================================*/
static int send_rtsp(rtsp_client_t* cl, const char* req, int req_len) {
    int r = send(cl->fd, req, req_len, MSG_NOSIGNAL);
    if (r < 0) return -1;
    return 0;
}

/* Read exactly one RTSP response (until \r\n\r\n + optional Content-Length body).
   Returns: 0=got full response, -1=error, 1=need more data */
static int read_rtsp_response(rtsp_client_t* cl, char** out_body, int* out_body_len) {
    char* buf = (char*)cl->buf;
    int*  len = &cl->buf_len;
    *out_body = NULL;
    *out_body_len = 0;

    /* Look for \r\n\r\n */
    char* eoh = strstr(buf, "\r\n\r\n");
    if (!eoh) return 1;

    int hdr_len = (int)(eoh - buf) + 4;

    /* Check if we have Content-Length */
    int body_len = 0;
    const char* cl_hdr = strstr(buf, "Content-Length: ");
    if (cl_hdr && cl_hdr < eoh) {
        body_len = atoi(cl_hdr + 16);
    }

    int total = hdr_len + body_len;
    if (*len < total) return 1;

    int copy_len = total < (int)sizeof(cl->last_response) - 1 ? total : (int)sizeof(cl->last_response) - 1;
    memcpy(cl->last_response, buf, (size_t)copy_len);
    cl->last_response[copy_len] = '\0';
    cl->last_response_len = copy_len;

    if (body_len > 0 && hdr_len < copy_len) {
        *out_body = cl->last_response + hdr_len;
        *out_body_len = body_len < copy_len - hdr_len ? body_len : copy_len - hdr_len;
    }

    /* Consume from buffer */
    if (*len > total)
        memmove(buf, buf + total, *len - total);
    *len -= total;

    return 0;
}

/* Check RTSP response status code */
static int get_status_code(const char* buf) {
    if (strncmp(buf, "RTSP/1.0 ", 9) != 0) return -1;
    return atoi(buf + 9);
}

/* Get a header value from a response */
static const char* get_header(const char* buf, const char* name) {
    const char* eoh = strstr(buf, "\r\n\r\n");
    if (!eoh) return NULL;

    /* Bolt optimization: Hoist strlen out of the loop and use early-exit length checking
     * to skip the expensive strncasecmp for non-matching header sizes. */
    size_t name_len = strlen(name);
    const char* p = buf;
    while (p < eoh) {
        const char* eol = strstr(p, "\r\n");
        if (!eol || eol > eoh) break;
        const char* colon = strchr(p, ':');
        if (colon && colon < eol) {
            size_t nlen = colon - p;
            if (nlen == name_len && strncasecmp(p, name, name_len) == 0) {
                const char* val = colon + 1;
                while (*val == ' ') val++;
                /* Return a static copy - we'll store in the response buffer */
                return val;
            }
        }
        p = eol + 2;
    }
    return NULL;
}

/* Use a temp buffer to extract a header value */
static int get_header_value(const char* buf, const char* name, char* out, int out_size) {
    const char* val = get_header(buf, name);
    if (!val) return -1;
    const char* eol = strchr(val, '\r');
    if (!eol) {
        eol = val + strlen(val);
    }
    int len = (int)(eol - val);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, val, len);
    out[len] = '\0';
    return 0;
}

/*===========================================================================
    Generate Authentication header (Basic/Digest)
===========================================================================*/
static void build_auth(rtsp_client_t* cl, const char* method, const char* uri,
                        char* out, int out_size)
{
    if (cl->auth_scheme == 0) { out[0] = '\0'; return; }

    if (cl->auth_scheme == 1) {
        /* Basic */
        char cred[160];
        char b64[256];
        snprintf(cred, sizeof(cred), "%s:%s", cl->username, cl->password);
        if (base64_encode((const uint8_t*)cred, strlen(cred), b64, sizeof(b64)) != 0) {
            out[0] = '\0';
            return;
        }
        snprintf(out, out_size, "Authorization: Basic %s\r\n", b64);
        return;
    }

    /* Digest (RFC 2617, qop=auth when advertised) */
    cl->auth_nc++;
    char nc_hex[9];
    snprintf(nc_hex, sizeof(nc_hex), "%08x", cl->auth_nc);

    /* Generate cnonce */
    char cnonce[16];
    snprintf(cnonce, sizeof(cnonce), "%08x", secure_rand32());

    char a1[256], a2[256], kd[512];
    char ha1[33], ha2[33], response[33];
    snprintf(a1, sizeof(a1), "%s:%s:%s", cl->username, cl->auth_realm, cl->password);
    snprintf(a2, sizeof(a2), "%s:%s", method, uri);
    md5_hex(a1, ha1);
    md5_hex(a2, ha2);
    snprintf(kd, sizeof(kd), "%s:%s:%s:%s:auth:%s",
             ha1, cl->auth_nonce, nc_hex, cnonce, ha2);
    md5_hex(kd, response);

    snprintf(out, out_size,
        "Authorization: Digest username=\"%s\", realm=\"%s\", "
        "nonce=\"%s\", uri=\"%s\", qop=auth, nc=%s, "
        "cnonce=\"%s\", response=\"%s\"%s%s%s\r\n",
        cl->username, cl->auth_realm,
        cl->auth_nonce, uri, nc_hex,
        cnonce, response,
        cl->auth_opaque[0] ? ", opaque=\"" : "",
        cl->auth_opaque,
        cl->auth_opaque[0] ? "\"" : "");
}

/*===========================================================================
    SDP Parser (simple line-based, handles H.264 and AAC)
===========================================================================*/
static int parse_sdp(rtsp_client_t* cl, const char* sdp, int len) {
    const char* end = sdp + len;
    const char* p = sdp;
    int media_started = 0;
    track_info_t* track = NULL;

    while (p < end) {
        const char* eol = strchr(p, '\n');
        if (!eol) eol = end;
        size_t line_len = (size_t)(eol - p);
        /* Trim \r */
        while (line_len > 0 && (p[line_len-1] == '\r' || p[line_len-1] == '\n'))
            line_len--;

        if (line_len >= 2 && p[0] == 'm' && p[1] == '=') {
            /* m=<media> <port> <proto> <fmt> ... */
            media_started = 1;
            if (track) {
                cl->track_count++;
                track = NULL;
            }
            if (cl->track_count >= MAX_TRACKS) { p = eol + 1; continue; }
            track = &cl->tracks[cl->track_count];
            memset(track, 0, sizeof(*track));
            track->interleaved_rtp = cl->track_count * 2;
            track->interleaved_rtcp = track->interleaved_rtp + 1;
            track->udp_rtp_fd = -1;
            track->udp_rtcp_fd = -1;

            const char* rest = p + 2;
            if (strncmp(rest, "video", 5) == 0) track->type = 1;
            else if (strncmp(rest, "audio", 5) == 0) track->type = 2;

            /* Parse payload type (last token on m= line) */
            size_t line_len = strcspn(rest, "\r\n");
            const char* last_space = NULL;
            for (size_t i = line_len; i > 0; i--) {
                if (rest[i - 1] == ' ') {
                    last_space = rest + i - 1;
                    break;
                }
            }
            if (last_space) {
                track->payload_type = atoi(last_space + 1);
            }
        } else if (p[0] == 'a' && p[1] == '=' && track) {
            const char* val = p + 2;

            if (strncmp(val, "rtpmap:", 7) == 0) {
                /* a=rtpmap:<pt> <encoding>/<rate>[/<channels>] */
                int pt = atoi(val + 7);
                const char* slash = strchr(val + 7, '/');
                if (slash && pt == track->payload_type) {
                    const char* enc_start = val + 7;
                    enc_start += strcspn(enc_start, " ");
                    if (*enc_start == ' ') enc_start++;
                    const char* enc_end = strchr(enc_start, '/');
                    if (enc_end) {
                        size_t e = (size_t)(enc_end - enc_start);
                        if (e >= sizeof(track->encoding)) e = sizeof(track->encoding) - 1;
                        memcpy(track->encoding, enc_start, e);
                        track->encoding[e] = '\0';
                        track->clock_rate = atoi(enc_end + 1);
                        /* Check for /channels */
                        const char* slash2 = strchr(enc_end + 1, '/');
                        if (slash2) track->channels = atoi(slash2 + 1);
                    }
                }
            } else if (strncmp(val, "fmtp:", 5) == 0) {
                int pt_fmtp = atoi(val + 5);
                if (pt_fmtp == track->payload_type) {
                    const char* fmtp_start = val + 5;
                    fmtp_start += strcspn(fmtp_start, " ");
                    if (*fmtp_start == ' ') {
                        fmtp_start++;
                        strncpy(track->fmtp, fmtp_start, sizeof(track->fmtp) - 1);
                        /* Extract sprop-parameter-sets for H.264 */
                        if (strcasecmp(track->encoding, "H264") == 0) {
                            const char* sps_val = strstr(track->fmtp, "sprop-parameter-sets=");
                            if (sps_val) {
                                /* Parse base64 SPS/PPS */
                                /* For now, mark it and we'll extract from stream */
                            }
                        }
                    }
                }
            }
        }

        if (media_started && line_len >= 2 && !(p[0] == 'a' || p[0] == 'm'))
            media_started = 0;

        if (track && line_len >= 2 && p[0] == 'a' && p[1] >= 'a' && p[1] <= 'z') {
            /* Still parsing attributes for current track */
        }

        p = eol + 1;
    }

    /* Count last track */
    if (track) {
        cl->track_count++;
    }

    return cl->track_count;
}

/*===========================================================================
    RTSP message formatting
===========================================================================*/
#define APPEND_SNPRINTF(buf, size, n, ...) do { \
    if ((n) < (size)) { \
        int _ret = snprintf((buf) + (n), (size) - (n), __VA_ARGS__); \
        if (_ret > 0) { \
            (n) += _ret; \
            if ((n) > (size)) { (n) = (size); } \
        } \
    } \
} while(0)

/* Build request with auth */
static int build_request(rtsp_client_t* cl, const char* method,
                          const char* uri, const char* extra_hdrs,
                          const char* body, int body_len,
                          char* out, int out_size)
{
    char auth[1024];
    build_auth(cl, method, uri, auth, sizeof(auth));

    int n = 0;
    APPEND_SNPRINTF(out, out_size, n,
        "%s %s RTSP/1.0\r\n"
        "CSeq: %u\r\n"
        "%s"     /* Session header (if set) */
        "%s"     /* Authorization */
        "%s"     /* Extra headers */
        "User-Agent: zstreamer/1.0\r\n",
        method, uri, cl->cseq++,
        cl->session_id[0] ? (const char*)(char[64]){0} : "", /* simplified */
        auth,
        extra_hdrs ? extra_hdrs : "");

    /* Actually include session if set */
    if (cl->session_id[0]) {
        char sess_hdr[128];
        snprintf(sess_hdr, sizeof(sess_hdr), "Session: %s\r\n", cl->session_id);
        /* Rebuild with session */
        n = 0;
        APPEND_SNPRINTF(out, out_size, n,
            "%s %s RTSP/1.0\r\n"
            "CSeq: %u\r\n"
            "Session: %s\r\n"
            "%s"
            "%s"
            "User-Agent: zstreamer/1.0\r\n",
            method, uri, cl->cseq - 1,
            cl->session_id,
            auth,
            extra_hdrs ? extra_hdrs : "");
    }

    /* Add body */
    if (body && body_len > 0) {
        APPEND_SNPRINTF(out, out_size, n,
            "Content-Length: %d\r\n\r\n", body_len);
        if (n + body_len < out_size) {
            memcpy(out + n, body, body_len);
            n += body_len;
        }
    } else {
        APPEND_SNPRINTF(out, out_size, n, "Content-Length: 0\r\n\r\n");
    }

    return n;
}

/*===========================================================================
    UDP socket helpers
===========================================================================*/
static int create_udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static int bind_udp_port(int fd, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) return -1;
    return 0;
}

static int bind_udp_pair(int* rtp_fd, int* rtcp_fd, uint16_t* out_rtp_port, uint16_t* out_rtcp_port) {
    static pthread_mutex_t port_mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint16_t next_port = 30000;
    
    pthread_mutex_lock(&port_mutex);
    for (int i = 0; i < 500; i++) {
        uint16_t rtp_port = next_port;
        next_port += 2;
        if (next_port >= 40000 || next_port < 30000) {
            next_port = 30000;
        }
        
        if (bind_udp_port(*rtp_fd, rtp_port) < 0) {
            close(*rtp_fd);
            *rtp_fd = create_udp_socket();
            if (*rtp_fd < 0) { pthread_mutex_unlock(&port_mutex); return -1; }
            continue;
        }
        
        if (bind_udp_port(*rtcp_fd, rtp_port + 1) < 0) {
            close(*rtp_fd);
            *rtp_fd = create_udp_socket();
            if (*rtp_fd < 0) { pthread_mutex_unlock(&port_mutex); return -1; }
            
            close(*rtcp_fd);
            *rtcp_fd = create_udp_socket();
            if (*rtcp_fd < 0) { pthread_mutex_unlock(&port_mutex); return -1; }
            continue;
        }
        
        *out_rtp_port = rtp_port;
        *out_rtcp_port = rtp_port + 1;
        pthread_mutex_unlock(&port_mutex);
        return 0;
    }
    pthread_mutex_unlock(&port_mutex);
    return -1;
}

static int bind_multicast_socket(int fd, const char* group, uint16_t port) {
    if (fd < 0 || !group || !*group || port == 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) return -1;

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (mreq.imr_multiaddr.s_addr == INADDR_NONE) return -1;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) return -1;
    return 0;
}

/*===========================================================================
    RTSP state machine operations
===========================================================================*/
static int parse_www_authenticate(rtsp_client_t* cl, const char* resp)
{
    const char* www_auth = strstr(resp, "WWW-Authenticate: ");
    if (!www_auth) return -1;
    www_auth += 18;
    if (strncmp(www_auth, "Basic ", 6) == 0) {
        cl->auth_scheme = 1;
    } else if (strncmp(www_auth, "Digest ", 7) == 0) {
        cl->auth_scheme = 2;
        const char* r = strstr(www_auth, "realm=\"");
        if (r) { r += 7; const char* e = strchr(r, '"');
            if (e) { size_t l = (size_t)(e - r);
                if (l >= sizeof(cl->auth_realm)) l = sizeof(cl->auth_realm)-1;
                memcpy(cl->auth_realm, r, l); cl->auth_realm[l] = '\0'; } }
        r = strstr(www_auth, "nonce=\"");
        if (r) { r += 7; const char* e = strchr(r, '"');
            if (e) { size_t l = (size_t)(e - r);
                if (l >= sizeof(cl->auth_nonce)) l = sizeof(cl->auth_nonce)-1;
                memcpy(cl->auth_nonce, r, l); cl->auth_nonce[l] = '\0'; } }
        r = strstr(www_auth, "opaque=\"");
        if (r) { r += 8; const char* e = strchr(r, '"');
            if (e) { size_t l = (size_t)(e - r);
                if (l >= sizeof(cl->auth_opaque)) l = sizeof(cl->auth_opaque)-1;
                memcpy(cl->auth_opaque, r, l); cl->auth_opaque[l] = '\0'; } }
    } else {
        return -1;
    }
    cl->auth_attempts++;
    return 0;
}

static int do_describe(rtsp_client_t* cl) {
    char req[RTSP_REQ_SIZE];
    int req_len = build_request(cl, "DESCRIBE", cl->path,
                                 "Accept: application/sdp\r\n", NULL, 0,
                                 req, sizeof(req));
    if (send_rtsp(cl, req, req_len) < 0) return -1;
    cl->state = STATE_DESCRIBE_SENT;
    return 0;
}

static int handle_describe_reply(rtsp_client_t* cl, const char* body, int body_len) {
    /* Check for auth */
    const char* resp = cl->last_response;

    int code = get_status_code(resp);
    if (code == 401) {
        if (cl->auth_attempts < 3 && parse_www_authenticate(cl, resp) == 0) {
            return do_describe(cl); /* retry with auth */
        }
        ZST_LOG_ERROR("rtspsrc", "DESCRIBE auth failed");
        return -1;
    }

    if (code != 200) {
        ZST_LOG_ERROR("rtspsrc", "DESCRIBE failed: %d", code);
        return -1;
    }

    /* Parse SDP */
    parse_sdp(cl, body, body_len);

    cl->state = STATE_DESCRIBE_DONE;
    return 0;
}

static int do_setup(rtsp_client_t* cl, int idx, const char* transport_mode) {
    if (idx >= cl->track_count) return -1;
    track_info_t* tr = &cl->tracks[idx];

    /* Build track URI */
    char track_uri[1024];
    snprintf(track_uri, sizeof(track_uri), "%s/trackID=%d", cl->path, idx);

    char transport_hdr[256];

    if (transport_mode && strcasecmp(transport_mode, "udp") == 0) {
        /* UDP unicast transport */
        /* Create and bind UDP sockets to ephemeral ports */
        tr->udp_rtp_fd = create_udp_socket();
        tr->udp_rtcp_fd = create_udp_socket();

        if (tr->udp_rtp_fd < 0 || tr->udp_rtcp_fd < 0 ||
            bind_udp_pair(&tr->udp_rtp_fd, &tr->udp_rtcp_fd, &tr->client_rtp_port, &tr->client_rtcp_port) < 0)
        {
            if (tr->udp_rtp_fd >= 0) close(tr->udp_rtp_fd);
            if (tr->udp_rtcp_fd >= 0) close(tr->udp_rtcp_fd);
            tr->udp_rtp_fd = tr->udp_rtcp_fd = -1;
            ZST_LOG_ERROR("rtspsrc", "failed to create UDP sockets for track %d", idx);
            return -1;
        }

        snprintf(transport_hdr, sizeof(transport_hdr),
            "Transport: RTP/AVP;unicast;client_port=%hu-%hu\r\n",
            tr->client_rtp_port, tr->client_rtcp_port);

        ZST_LOG_INFO("rtspsrc", "track %d UDP: client_port=%hu-%hu",
                     idx, tr->client_rtp_port, tr->client_rtcp_port);
    } else if (transport_mode && strcasecmp(transport_mode, "multicast") == 0) {
        /* Multicast transport: server chooses/echoes destination and port. */
        snprintf(transport_hdr, sizeof(transport_hdr),
            "Transport: RTP/AVP;multicast\r\n");
        ZST_LOG_INFO("rtspsrc", "track %d multicast SETUP", idx);
    } else {
        /* TCP interleaved transport */
        snprintf(transport_hdr, sizeof(transport_hdr),
            "Transport: RTP/AVP/TCP;interleaved=%d-%d\r\n",
            tr->interleaved_rtp, tr->interleaved_rtcp);
    }

    char req[RTSP_REQ_SIZE];
    int req_len = build_request(cl, "SETUP", track_uri,
                                 transport_hdr, NULL, 0,
                                 req, sizeof(req));
    if (send_rtsp(cl, req, req_len) < 0) return -1;
    cl->state = STATE_SETUP_SENT;
    cl->setup_progress = idx;
    return 0;
}

static int handle_setup_reply(rtsp_client_t* cl, const char* transport_mode) {
    const char* resp = cl->last_response;
    int code = get_status_code(resp);
    if (code == 401) {
        if (cl->auth_attempts < 6 && parse_www_authenticate(cl, resp) == 0) {
            return do_setup(cl, cl->setup_progress, transport_mode);
        }
        ZST_LOG_ERROR("rtspsrc", "SETUP auth failed for track %d", cl->setup_progress);
        return -1;
    }
    if (code != 200) {
        ZST_LOG_ERROR("rtspsrc", "SETUP failed for track %d: %d",
                      cl->setup_progress, code);
        return -1;
    }

    /* Get session ID */
    get_header_value(resp, "Session", cl->session_id, sizeof(cl->session_id));

    /* Parse Transport response for UDP unicast or multicast */
    int idx = cl->setup_progress;
    if (transport_mode && (strcasecmp(transport_mode, "udp") == 0 ||
                           strcasecmp(transport_mode, "multicast") == 0)) {
        track_info_t* tr = &cl->tracks[idx];
        const char* transport_val = strstr(resp, "Transport: ");
        if (transport_val) {
            transport_val += 11; /* skip "Transport: " */

            if (strcasecmp(transport_mode, "udp") == 0) {
                const char* sp = strstr(transport_val, "server_port=");
                if (sp) {
                    sp += 12;
                    unsigned long sp1 = strtoul(sp, NULL, 10);
                    const char* dash = strchr(sp, '-');
                    unsigned long sp2 = dash ? strtoul(dash + 1, NULL, 10) : sp1 + 1;
                    tr->server_rtp_port  = (uint16_t)sp1;
                    tr->server_rtcp_port = (uint16_t)sp2;
                    ZST_LOG_INFO("rtspsrc", "track %d server_port=%lu-%lu",
                                 idx, sp1, sp2);
                }
            } else {
                char group[64] = "";
                const char* dst = strstr(transport_val, "destination=");
                if (dst) {
                    dst += 12;
                    const char* end = strpbrk(dst, ";\r\n");
                    size_t len = end ? (size_t)(end - dst) : strlen(dst);
                    if (len >= sizeof(group)) len = sizeof(group) - 1;
                    memcpy(group, dst, len);
                    group[len] = '\0';
                }

                const char* pp = strstr(transport_val, "port=");
                if (pp) {
                    pp += 5;
                    unsigned long p1 = strtoul(pp, NULL, 10);
                    const char* dash = strchr(pp, '-');
                    unsigned long p2 = dash ? strtoul(dash + 1, NULL, 10) : p1 + 1;
                    tr->server_rtp_port = (uint16_t)p1;
                    tr->server_rtcp_port = (uint16_t)p2;
                }
                const char* ttlp = strstr(transport_val, "ttl=");
                if (ttlp) tr->multicast_ttl = atoi(ttlp + 4);

                if (group[0] == '\0' || tr->server_rtp_port == 0) {
                    ZST_LOG_ERROR("rtspsrc", "multicast SETUP missing destination or port for track %d", idx);
                    return -1;
                }

                strncpy(tr->multicast_addr, group, sizeof(tr->multicast_addr) - 1);
                tr->multicast_addr[sizeof(tr->multicast_addr) - 1] = '\0';
                tr->client_rtp_port = tr->server_rtp_port;
                tr->client_rtcp_port = tr->server_rtcp_port;
                tr->is_multicast = 1;

                tr->udp_rtp_fd = create_udp_socket();
                tr->udp_rtcp_fd = create_udp_socket();
                if (tr->udp_rtp_fd < 0 || tr->udp_rtcp_fd < 0 ||
                    bind_multicast_socket(tr->udp_rtp_fd, tr->multicast_addr, tr->server_rtp_port) < 0 ||
                    bind_multicast_socket(tr->udp_rtcp_fd, tr->multicast_addr, tr->server_rtcp_port) < 0) {
                    if (tr->udp_rtp_fd >= 0) close(tr->udp_rtp_fd);
                    if (tr->udp_rtcp_fd >= 0) close(tr->udp_rtcp_fd);
                    tr->udp_rtp_fd = tr->udp_rtcp_fd = -1;
                    ZST_LOG_ERROR("rtspsrc", "failed to join multicast %s:%hu-%hu for track %d",
                                  tr->multicast_addr, tr->server_rtp_port, tr->server_rtcp_port, idx);
                    return -1;
                }
                ZST_LOG_INFO("rtspsrc", "track %d multicast: %s:%hu-%hu ttl=%d",
                             idx, tr->multicast_addr, tr->server_rtp_port,
                             tr->server_rtcp_port, tr->multicast_ttl);
            }
        }
    }

    if (idx + 1 < cl->track_count) {
        /* Setup next track */
        return do_setup(cl, idx + 1, transport_mode);
    }

    cl->state = STATE_SETUP_DONE;
    return 0;
}

static int do_play(rtsp_client_t* cl) {
    char req[RTSP_REQ_SIZE];
    int req_len = build_request(cl, "PLAY", cl->path,
                                 "Range: npt=0.000-\r\n", NULL, 0,
                                 req, sizeof(req));
    if (send_rtsp(cl, req, req_len) < 0) return -1;
    cl->state = STATE_PLAY_SENT;
    return 0;
}

static int rtsp_message_is_end_of_stream(const char* msg)
{
    if (!msg) return 0;
    return strncmp(msg, "TEARDOWN ", 9) == 0 ||
           strncmp(msg, "BYE ", 4) == 0 ||
           strstr(msg, "Session: 0") != NULL;
}

static int do_options(rtsp_client_t* cl) {
    char req[RTSP_REQ_SIZE];
    int req_len = build_request(cl, "OPTIONS", cl->path, NULL, NULL, 0,
                                 req, sizeof(req));
    return send_rtsp(cl, req, req_len);
}

static int handle_play_reply(rtsp_client_t* cl) {
    const char* resp = cl->last_response;
    int code = get_status_code(resp);
    if (code == 401) {
        if (cl->auth_attempts < 8 && parse_www_authenticate(cl, resp) == 0) {
            return do_play(cl);
        }
        ZST_LOG_ERROR("rtspsrc", "PLAY auth failed");
        return -1;
    }
    if (code != 200) {
        ZST_LOG_ERROR("rtspsrc", "PLAY failed: %d", code);
        return -1;
    }
    cl->state = STATE_STREAMING;
    ZST_LOG_INFO("rtspsrc", "PLAY successful, now streaming");
    return 0;
}

/*===========================================================================
    RTP depacketization: H.264 (RFC 3984)
===========================================================================*/
/* Convert RTP timestamp delta to PTS in ns */
static uint64_t rtp_ts_to_pts_track(rtsp_client_t* cl, track_info_t* tr, uint32_t rtp_ts) {
    int clock_rate = tr && tr->clock_rate > 0 ? tr->clock_rate : 90000;
    if (tr && tr->has_sr) {
        uint32_t delta = rtp_ts - tr->last_rtp_time;
        return ntp_to_unix_ns(tr->last_ntp_time) +
               (uint64_t)delta * 1000000000ULL / (uint64_t)clock_rate;
    }

    if (cl->base_pts == 0) {
        cl->base_pts = now_us() * 1000;
        if (clock_rate == 90000) {
            cl->base_rtp_ts_video = rtp_ts;
        } else {
            cl->base_rtp_ts_audio = rtp_ts;
        }
        return cl->base_pts;
    }

    uint32_t base_rtp = (clock_rate == 90000) ? cl->base_rtp_ts_video : cl->base_rtp_ts_audio;
    uint32_t delta = rtp_ts - base_rtp;
    return cl->base_pts + (uint64_t)delta * 1000000000ULL / (uint64_t)clock_rate;
}

/* Push a reconstructed NAL unit as a zst_buffer */
static void push_h264_nal(rtsp_source_priv_t* srv, const uint8_t* data, int len,
                           uint64_t pts, int marker)
{
    (void)marker;
    /* Write with Annex B start code */
    int total = len + 4;
    uint8_t* out = malloc(total);
    if (!out) return;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    memcpy(out + 4, data, len);

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!buf) { free(out); return; }
    buf->memory.data = out;
    buf->memory.size = total;
    buf->memory.priv = out;
    buf->memory.release = free;
    buf->pts = pts;
    buf->dts = pts;

    zst_pad_push(srv->video_pad, buf);
    zst_buffer_unref(buf);
}

/* Process H.264 RTP payload */
static void process_h264_rtp(rtsp_source_priv_t* srv, rtsp_client_t* cl,
                              track_info_t* tr,
                              const uint8_t* payload, int payload_len,
                              uint32_t rtp_ts, uint32_t ssrc, int marker)
{
    if (payload_len < 1) return;
    uint8_t nal_type = payload[0] & 0x1f;
    uint64_t pts = rtp_ts_to_pts_track(cl, tr, rtp_ts);

    if (nal_type <= H264_NAL_SINGLE_MAX) {
        /* Single NAL unit */
        push_h264_nal(srv, payload, payload_len, pts, marker);
    } else if (nal_type == H264_NAL_FU_A && payload_len >= 2) {
        /* FU-A fragmentation */
        uint8_t fu_indicator = payload[0];
        uint8_t fu_header = payload[1];
        uint8_t fu_nal_type = fu_header & 0x1f;
        uint8_t start_bit = (fu_header >> 7) & 1;
        uint8_t end_bit = (fu_header >> 6) & 1;
        uint8_t nri = (fu_indicator >> 5) & 0x03;

        if (start_bit) {
            /* Start of FU-A: begin accumulation */
            cl->fu_accum_len = 0;
            cl->fu_accum_ts = rtp_ts;
            cl->fu_accum_ssrc = ssrc;
            /* Write NAL header */
            if (cl->fu_accum_len + 1 <= (int)sizeof(cl->fu_accum)) {
                cl->fu_accum[cl->fu_accum_len++] = (nri << 5) | fu_nal_type;
            }
        }

        /* Skip FU indicator and header, copy payload */
        if (cl->fu_accum_ssrc == (int)ssrc && cl->fu_accum_ts == rtp_ts) {
            int copy_len = payload_len - 2;
            if (cl->fu_accum_len + copy_len <= (int)sizeof(cl->fu_accum)) {
                memcpy(cl->fu_accum + cl->fu_accum_len, payload + 2, copy_len);
                cl->fu_accum_len += copy_len;
            }
        }

        if (end_bit && cl->fu_accum_len > 1) {
            /* Complete NAL */
            push_h264_nal(srv, cl->fu_accum, cl->fu_accum_len,
                          rtp_ts_to_pts_track(cl, tr, cl->fu_accum_ts), 1);
            cl->fu_accum_len = 0;
        }
    } else if (nal_type == H264_NAL_STAP_A) {
        /* STAP-A: multiple NALs in one packet */
        int offset = 1; /* skip STAP-A NAL header */
        while (offset + 2 <= payload_len) {
            uint16_t nalu_size = (payload[offset] << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nalu_size > payload_len) break;
            push_h264_nal(srv, payload + offset, nalu_size, pts,
                          (offset + nalu_size >= payload_len) ? marker : 0);
            offset += nalu_size;
        }
    }
}

/* Process AAC RTP payload (RFC 3640 MPEG4-Generic) */
static void push_aac_frame(rtsp_source_priv_t* srv, const uint8_t* data, int len,
                            uint64_t pts)
{
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
    if (!buf) return;
    void* copy = malloc(len);
    if (!copy) { zst_buffer_unref(buf); return; }
    memcpy(copy, data, len);
    buf->memory.data = copy;
    buf->memory.size = len;
    buf->memory.priv = copy;
    buf->memory.release = free;
    buf->pts = pts;
    buf->dts = pts;

    zst_pad_push(srv->audio_pad, buf);
    zst_buffer_unref(buf);
}

static void process_aac_rtp(rtsp_source_priv_t* srv, rtsp_client_t* cl,
                             track_info_t* tr,
                             const uint8_t* payload, int payload_len,
                             uint32_t rtp_ts, int clock_rate, int marker)
{
    (void)marker;
    if (payload_len < 4) return;

    /* MPEG4-Generic: AU-headers-length (2 bytes, in bits) */
    int au_headers_len_bits = (payload[0] << 8) | payload[1];
    int au_headers_len_bytes = (au_headers_len_bits + 7) / 8;

    if (au_headers_len_bytes + 2 > payload_len) return;

    int offset = 2; /* skip AU-headers-length */
    if (tr && tr->clock_rate <= 0) tr->clock_rate = clock_rate;
    uint64_t pts = rtp_ts_to_pts_track(cl, tr, rtp_ts);

    /* Parse each AU-header (13-bit size + 3-bit index) */
    while (offset + 2 <= payload_len) {
        uint16_t au_header = (payload[offset] << 8) | payload[offset + 1];
        offset += 2;
        int au_size = (au_header >> 3) & 0x1FFF; /* in bytes now */
        /* Actually RTP spec: AU-size in bytes = au_header >> 3 */

        if (au_size > 0 && offset + au_size <= payload_len) {
            push_aac_frame(srv, payload + offset, au_size, pts);
            offset += au_size;
        } else {
            break;
        }
    }
}

/*===========================================================================
    RTCP processing (Sender Reports for NTP sync, BYE for EOS)
===========================================================================*/
static int process_rtcp_packet(rtsp_client_t* cl, track_info_t* tr,
                               const uint8_t* data, int len)
{
    (void)cl;
    if (!tr || !data || len < 4) return 0;
    int off = 0;
    while (off + 4 <= len) {
        uint8_t pt = data[off + 1];
        uint16_t words = ((uint16_t)data[off + 2] << 8) | data[off + 3];
        int plen = ((int)words + 1) * 4;
        if (plen <= 0 || off + plen > len) break;

        if (pt == 200 && plen >= 28) { /* Sender Report */
            uint64_t ntp = ((uint64_t)data[off + 8] << 56) |
                           ((uint64_t)data[off + 9] << 48) |
                           ((uint64_t)data[off + 10] << 40) |
                           ((uint64_t)data[off + 11] << 32) |
                           ((uint64_t)data[off + 12] << 24) |
                           ((uint64_t)data[off + 13] << 16) |
                           ((uint64_t)data[off + 14] << 8) |
                           (uint64_t)data[off + 15];
            uint32_t rtp = ((uint32_t)data[off + 16] << 24) |
                           ((uint32_t)data[off + 17] << 16) |
                           ((uint32_t)data[off + 18] << 8) |
                           (uint32_t)data[off + 19];
            tr->last_ntp_time = ntp;
            tr->last_rtp_time = rtp;
            tr->has_sr = 1;
        } else if (pt == 203) { /* BYE */
            return 1;
        }
        off += plen;
    }
    return 0;
}

/*===========================================================================
    Read and process RTP data (interleaved mode)
===========================================================================*/
static int process_interleaved_data(rtsp_source_priv_t* srv, rtsp_client_t* cl) {
    uint8_t* buf = cl->buf;
    int* len = &cl->buf_len;

    while (*len >= 4 && buf[0] == '$') {
        int channel = buf[1];
        int pkt_len = (buf[2] << 8) | buf[3];

        if (*len < 4 + pkt_len) return 1; /* need more data */

        uint8_t* rtp_data = buf + 4;

        /* Find which track */
        int track_idx = -1;
        int is_rtcp = 0;
        for (int i = 0; i < cl->track_count; i++) {
            if (cl->tracks[i].interleaved_rtp == channel) {
                track_idx = i;
                break;
            }
            if (cl->tracks[i].interleaved_rtcp == channel) {
                track_idx = i;
                is_rtcp = 1;
                break;
            }
        }

        if (track_idx >= 0 && is_rtcp) {
            if (process_rtcp_packet(cl, &cl->tracks[track_idx], rtp_data, pkt_len)) {
                return -1;
            }
        } else if (track_idx >= 0 && pkt_len >= 12) {
            /* Parse RTP header */
            rtp_hdr_t* rh = (rtp_hdr_t*)rtp_data;
            int version = rh->version;
            (void)version;
            int pt = rh->pt;
            int marker = rh->m;
            uint16_t seq = ntohs(rh->seq);
            uint32_t rtp_ts = ntohl(rh->timestamp);
            uint32_t ssrc = ntohl(rh->ssrc);
            (void)seq;
            (void)ssrc;

            int payload_offset = 12;
            /* Skip CSRC if any */
            if (rh->cc > 0) payload_offset += rh->cc * 4;
            /* Skip extension if present */
            if (rh->x && payload_offset + 4 <= pkt_len) {
                uint16_t ext_len = ntohs(*(uint16_t*)(rtp_data + payload_offset + 2));
                payload_offset += 4 + ext_len * 4;
            }

            if (payload_offset < pkt_len) {
                int payload_len = pkt_len - payload_offset;
                track_info_t* tr = &cl->tracks[track_idx];

                if (tr->type == 1 &&
                    (strcasecmp(tr->encoding, "H264") == 0)) {
                    process_h264_rtp(srv, cl, tr, rtp_data + payload_offset,
                                     payload_len, rtp_ts, ssrc, marker);
                } else if (tr->type == 2 &&
                           (strcasecmp(tr->encoding, "MPEG4-GENERIC") == 0)) {
                    process_aac_rtp(srv, cl, tr, rtp_data + payload_offset,
                                    payload_len, rtp_ts, tr->clock_rate, marker);
                }
            }
        }

        /* Consume from buffer */
        int total = 4 + pkt_len;
        if (*len > total)
            memmove(buf, buf + total, *len - total);
        *len -= total;
    }

    return 0;
}

/*===========================================================================
    Read and process RTP data from UDP
===========================================================================*/
#define RTSP_BATCH_SIZE 16

static int process_udp_data(rtsp_source_priv_t* srv, rtsp_client_t* cl) {
    for (int i = 0; i < cl->track_count; i++) {
        track_info_t* tr = &cl->tracks[i];
        if (tr->udp_rtp_fd < 0) continue;

        /* Non-blocking read — grab all available RTCP sender reports/BYE */
        uint8_t rtcp_buf[2048];
        int rn;
        while (tr->udp_rtcp_fd >= 0 &&
               (rn = (int)read(tr->udp_rtcp_fd, rtcp_buf, sizeof(rtcp_buf))) > 0) {
            if (process_rtcp_packet(cl, tr, rtcp_buf, rn)) {
                cl->state = STATE_ERROR;
                return -1;
            }
        }

        /* Non-blocking read — grab all available packets using recvmmsg */
        struct mmsghdr msgs[RTSP_BATCH_SIZE];
        struct iovec iovecs[RTSP_BATCH_SIZE];
        uint8_t rtp_bufs[RTSP_BATCH_SIZE][2048];

        memset(msgs, 0, sizeof(msgs));
        for (int k = 0; k < RTSP_BATCH_SIZE; k++) {
            iovecs[k].iov_base = rtp_bufs[k];
            iovecs[k].iov_len = sizeof(rtp_bufs[k]);
            msgs[k].msg_hdr.msg_iov = &iovecs[k];
            msgs[k].msg_hdr.msg_iovlen = 1;
        }

        int n_pkts;
        while ((n_pkts = recvmmsg(tr->udp_rtp_fd, msgs, RTSP_BATCH_SIZE, MSG_DONTWAIT, NULL)) > 0) {
            for (int k = 0; k < n_pkts; k++) {
                uint8_t* rtp_buf = rtp_bufs[k];
                int n = msgs[k].msg_len;
                cl->bytes_read += n;

                if (n < 12) continue; /* too small for RTP header */

                /* Parse RTP header */
                rtp_hdr_t* rh = (rtp_hdr_t*)rtp_buf;
                int pt = rh->pt;
                int marker = rh->m;
                uint16_t seq = ntohs(rh->seq);
                uint32_t rtp_ts = ntohl(rh->timestamp);
                uint32_t ssrc = ntohl(rh->ssrc);
                (void)seq;
                (void)ssrc;

                int payload_offset = 12;
                if (rh->cc > 0) payload_offset += rh->cc * 4;
                if (rh->x && payload_offset + 4 <= n) {
                    uint16_t ext_len = ntohs(*(uint16_t*)(rtp_buf + payload_offset + 2));
                    payload_offset += 4 + ext_len * 4;
                }

                if (payload_offset >= n) continue;
                int payload_len = n - payload_offset;

                /* Route by payload type to the right track */
                int track_idx = -1;
                for (int j = 0; j < cl->track_count; j++) {
                    if (cl->tracks[j].payload_type == pt) {
                        track_idx = j;
                        break;
                    }
                }

                if (track_idx < 0) continue;
                track_info_t* trk = &cl->tracks[track_idx];

                if (trk->type == 1 &&
                    (strcasecmp(trk->encoding, "H264") == 0)) {
                    process_h264_rtp(srv, cl, trk, rtp_buf + payload_offset,
                                     payload_len, rtp_ts, ssrc, marker);
                } else if (trk->type == 2 &&
                           (strcasecmp(trk->encoding, "MPEG4-GENERIC") == 0)) {
                    process_aac_rtp(srv, cl, trk, rtp_buf + payload_offset,
                                    payload_len, rtp_ts, trk->clock_rate, marker);
                }
            }
        }
    }
    return 0;
}

/*===========================================================================
    Main streaming thread
===========================================================================*/
static void* streaming_thread(void* arg) {
    rtsp_source_priv_t* srv = (rtsp_source_priv_t*)arg;
    rtsp_client_t* cl = &srv->client;
    int reconnect_attempts = 0;

reconnect_start:
    if (!__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE)) {
        srv->thread_started = 0;
        return NULL;
    }

    memset(cl, 0, sizeof(*cl));
    cl->fd = -1;
    cl->state = STATE_INIT;
    parse_rtsp_url(srv->url, cl);
    if (srv->username[0]) strncpy(cl->username, srv->username, sizeof(cl->username) - 1);
    if (srv->password[0]) strncpy(cl->password, srv->password, sizeof(cl->password) - 1);

    int is_udp = (strcasecmp(srv->transport, "udp") == 0);
    int is_multicast = (strcasecmp(srv->transport, "multicast") == 0);
    int is_datagram = is_udp || is_multicast;
    cl->transport_type = is_multicast ? RTSP_SOURCE_TRANSPORT_MULTICAST :
                         (is_udp ? RTSP_SOURCE_TRANSPORT_UDP : RTSP_SOURCE_TRANSPORT_TCP);

    ZST_LOG_INFO("rtspsrc", "connecting to rtsp://%s:%d%s transport=%s",
                 cl->host, cl->port, cl->path,
                 is_multicast ? "MULTICAST" : (is_udp ? "UDP" : "TCP"));

    /* Connect */
    cl->fd = tcp_connect(cl->host, cl->port);
    if (cl->fd < 0) {
        ZST_LOG_ERROR("rtspsrc", "failed to connect to %s:%d", cl->host, cl->port);
        if (srv->reconnect && __atomic_load_n(&srv->running, __ATOMIC_ACQUIRE) &&
            (srv->max_reconnect_attempts < 0 || reconnect_attempts < srv->max_reconnect_attempts)) {
            reconnect_attempts++;
            sleep_ms(srv->reconnect_delay_ms > 0 ? srv->reconnect_delay_ms : 500);
            goto reconnect_start;
        }
        __atomic_store_n(&srv->running, 0, __ATOMIC_RELEASE);
        srv->thread_started = 0;
        return NULL;
    }

    cl->cseq = 1;
    uint64_t next_keepalive_us = now_us() + (uint64_t)(srv->keepalive_interval_sec > 0 ? srv->keepalive_interval_sec : 30) * 1000000ULL;

    /* Phase 1: DESCRIBE */
    ZST_LOG_INFO("rtspsrc", "sending DESCRIBE");
    if (do_describe(cl) < 0) { close(cl->fd); return NULL; }

    /* Read DESCRIBE response */
    while (cl->state == STATE_DESCRIBE_SENT) {
        struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 10000);
        if (ret <= 0) { ZST_LOG_ERROR("rtspsrc", "DESCRIBE timeout"); close(cl->fd); return NULL; }

        int n = (int)read(cl->fd, cl->buf + cl->buf_len, sizeof(cl->buf) - cl->buf_len);
        if (n <= 0) { close(cl->fd); return NULL; }
        cl->buf_len += n;

        char* body; int body_len;
        int r = read_rtsp_response(cl, &body, &body_len);
        if (r == 0) {
            if (handle_describe_reply(cl, body, body_len) < 0) {
                close(cl->fd); return NULL;
            }
        }
    }

    ZST_LOG_INFO("rtspsrc", "got %d track(s)", cl->track_count);

    /* Phase 2: SETUP each track */
    for (int i = 0; i < cl->track_count; i++) {
        /* handle_setup_reply chains do_setup for subsequent tracks,
           so skip if already set up (i.e. state advanced past SETUP). */
        if (cl->state == STATE_SETUP_DONE) break;

        ZST_LOG_INFO("rtspsrc", "setting up track %d (%s)", i,
                     cl->tracks[i].encoding);
        if (do_setup(cl, i, srv->transport) < 0) { close(cl->fd); return NULL; }

        while (cl->state == STATE_SETUP_SENT) {
            struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
            int ret = poll(&pfd, 1, 10000);
            if (ret <= 0) { ZST_LOG_ERROR("rtspsrc", "SETUP timeout"); close(cl->fd); return NULL; }

            int n = (int)read(cl->fd, cl->buf + cl->buf_len, sizeof(cl->buf) - cl->buf_len);
            if (n <= 0) { close(cl->fd); return NULL; }
            cl->buf_len += n;

            char* body; int body_len;
            int r = read_rtsp_response(cl, &body, &body_len);
            if (r == 0) {
                if (handle_setup_reply(cl, srv->transport) < 0) {
                    close(cl->fd); return NULL;
                }
            }
        }
    }

    /* Phase 3: PLAY */
    ZST_LOG_INFO("rtspsrc", "sending PLAY");
    if (do_play(cl) < 0) { close(cl->fd); return NULL; }

    while (cl->state == STATE_PLAY_SENT) {
        struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 10000);
        if (ret <= 0) { ZST_LOG_ERROR("rtspsrc", "PLAY timeout"); close(cl->fd); return NULL; }

        int n = (int)read(cl->fd, cl->buf + cl->buf_len, sizeof(cl->buf) - cl->buf_len);
        if (n <= 0) { close(cl->fd); return NULL; }
        cl->buf_len += n;

        char* body; int body_len;
        int r = read_rtsp_response(cl, &body, &body_len);
        if (r == 0) {
            if (handle_play_reply(cl) < 0) {
                close(cl->fd); return NULL;
            }
        }
    }

    /* Phase 4: Streaming */
    ZST_LOG_INFO("rtspsrc", "streaming started (%s)",
                 is_multicast ? "UDP multicast" : (is_udp ? "UDP unicast" : "TCP interleaved"));
    srv->running = 1;

    if (is_datagram) {
        /* UDP unicast/multicast mode: poll TCP for RTSP + all UDP RTP sockets */
        int max_fds = 1 + cl->track_count;
        struct pollfd* pfds = malloc(sizeof(struct pollfd) * (size_t)max_fds);
        if (!pfds) { close(cl->fd); srv->running = 0; return NULL; }

        while (srv->running && cl->state == STATE_STREAMING) {
            uint64_t now = now_us();
            if (srv->keepalive_interval_sec > 0 && now >= next_keepalive_us) {
                do_options(cl);
                next_keepalive_us = now + (uint64_t)srv->keepalive_interval_sec * 1000000ULL;
            }
            int nfds = 0;

            /* TCP socket for RTSP keepalive/commands */
            pfds[nfds].fd = cl->fd;
            pfds[nfds].events = POLLIN;
            nfds++;

            /* UDP RTP sockets for each track */
            for (int i = 0; i < cl->track_count; i++) {
                if (cl->tracks[i].udp_rtp_fd >= 0) {
                    pfds[nfds].fd = cl->tracks[i].udp_rtp_fd;
                    pfds[nfds].events = POLLIN;
                    nfds++;
                }
            }

            int ret = poll(pfds, (nfds_t)nfds, 1000);
            if (ret < 0) { if (errno == EINTR) continue; break; }
            if (ret == 0) continue;

            /* Check TCP for RTSP data */
            if (pfds[0].revents & POLLIN) {
                int n = (int)read(cl->fd, cl->buf + cl->buf_len,
                                  sizeof(cl->buf) - cl->buf_len);
                if (n <= 0) break;
                cl->buf_len += n;
                /* Process any RTSP responses (e.g. keepalive) */
                char* body; int body_len;
                while (cl->buf_len > 0) {
                    int r = read_rtsp_response(cl, &body, &body_len);
                    if (r == 1) break; /* need more data */
                    if (r < 0) { break; }
                    if (rtsp_message_is_end_of_stream(cl->last_response)) {
                        cl->state = STATE_ERROR;
                        break;
                    }
                }
            }
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;

            /* Check UDP RTP sockets */
            if (process_udp_data(srv, cl) < 0) break;
        }
        free(pfds);
    } else {
        /* TCP interleaved mode: poll TCP only */
        while (srv->running && cl->state == STATE_STREAMING) {
            uint64_t now = now_us();
            if (srv->keepalive_interval_sec > 0 && now >= next_keepalive_us) {
                do_options(cl);
                next_keepalive_us = now + (uint64_t)srv->keepalive_interval_sec * 1000000ULL;
            }
            struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
            int ret = poll(&pfd, 1, 1000);

            if (ret < 0) { if (errno == EINTR) continue; break; }
            if (ret == 0) continue;

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

            int n = (int)read(cl->fd, cl->buf + cl->buf_len,
                              sizeof(cl->buf) - cl->buf_len);
            if (n <= 0) break;
            cl->buf_len += n;
            cl->bytes_read += n;

            /* Process RTSP messages or interleaved RTP/RTCP data */
            if (cl->buf_len > 0 && cl->buf[0] != '$') {
                char* body; int body_len;
                while (cl->buf_len > 0 && cl->buf[0] != '$') {
                    int r = read_rtsp_response(cl, &body, &body_len);
                    if (r == 1) break;
                    if (r < 0) break;
                    if (rtsp_message_is_end_of_stream(cl->last_response)) {
                        cl->state = STATE_ERROR;
                        break;
                    }
                }
                if (cl->state != STATE_STREAMING) break;
            }
            if (process_interleaved_data(srv, cl) < 0) break;
        }
    }

    /* Teardown */
    if (cl->session_id[0] && cl->fd >= 0) {
        char tear[256];
        int tear_len = snprintf(tear, sizeof(tear),
            "TEARDOWN %s RTSP/1.0\r\n"
            "CSeq: %u\r\n"
            "Session: %s\r\n"
            "Content-Length: 0\r\n\r\n",
            cl->path, cl->cseq++, cl->session_id);
        (void)send(cl->fd, tear, tear_len, MSG_NOSIGNAL);
    }

    close(cl->fd);
    cl->fd = -1;

    /* Close UDP sockets */
    for (int i = 0; i < cl->track_count; i++) {
        if (cl->tracks[i].udp_rtp_fd >= 0) {
            close(cl->tracks[i].udp_rtp_fd);
            cl->tracks[i].udp_rtp_fd = -1;
        }
        if (cl->tracks[i].udp_rtcp_fd >= 0) {
            close(cl->tracks[i].udp_rtcp_fd);
            cl->tracks[i].udp_rtcp_fd = -1;
        }
    }

    if (__atomic_load_n(&srv->running, __ATOMIC_ACQUIRE) && srv->reconnect &&
        (srv->max_reconnect_attempts < 0 || reconnect_attempts < srv->max_reconnect_attempts)) {
        reconnect_attempts++;
        ZST_LOG_INFO("rtspsrc", "stream ended, reconnecting attempt %d", reconnect_attempts);
        sleep_ms(srv->reconnect_delay_ms > 0 ? srv->reconnect_delay_ms : 500);
        goto reconnect_start;
    }

    pthread_mutex_lock(&srv->lock);
    if (srv->video_pad && srv->video_pad->peer) {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        if (eos) {
            eos->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(srv->video_pad, eos);
            zst_buffer_unref(eos);
        }
    }
    if (srv->audio_pad && srv->audio_pad->peer) {
        zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
        if (eos) {
            eos->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(srv->audio_pad, eos);
            zst_buffer_unref(eos);
        }
    }
    pthread_mutex_unlock(&srv->lock);

    __atomic_store_n(&srv->running, 0, __ATOMIC_RELEASE);
    srv->thread_started = 0;

    ZST_LOG_INFO("rtspsrc", "streaming ended, %llu bytes read",
                 (unsigned long long)cl->bytes_read);
    return NULL;
}

/*===========================================================================
    Element ops
===========================================================================*/
static zst_caps_t* get_caps(zst_element_t* el, zst_pad_t* pad,
                             const zst_caps_t* filter)
{
    (void)filter;
    rtsp_source_priv_t* s = el->priv;
    if (!s) return NULL;

    if (pad == s->video_pad) {
        if (s->video_caps) return zst_caps_copy(s->video_caps);
        zst_caps_t* c = zst_caps_create();
        zst_caps_append(c, zst_caps_struct_create_video("video/x-h264", 0,0,0,""));
        zst_caps_append(c, zst_caps_struct_create_video("video/x-h265", 0,0,0,""));
        return c;
    }
    if (pad == s->audio_pad) {
        if (s->audio_caps) return zst_caps_copy(s->audio_caps);
        zst_caps_t* c = zst_caps_create();
        zst_caps_append(c, zst_caps_struct_create_audio("audio/aac", 0,0,""));
        return c;
    }
    return NULL;
}

static zst_result_t el_open(zst_element_t* el) {
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (s->url[0] == '\0') {
        ZST_LOG_ERROR("rtspsrc", "no RTSP URL configured");
        return ZST_ERROR;
    }

    rtsp_client_t* cl = &s->client;
    memset(cl, 0, sizeof(*cl));
    cl->fd = -1;
    cl->state = STATE_INIT;

    parse_rtsp_url(s->url, cl);
    if (s->username[0]) strncpy(cl->username, s->username, sizeof(cl->username) - 1);
    if (s->password[0]) strncpy(cl->password, s->password, sizeof(cl->password) - 1);

    if (!s->lock_initialized) {
        pthread_mutex_init(&s->lock, NULL);
        s->lock_initialized = 1;
    }
    ZST_LOG_INFO("rtspsrc", "connecting to %s:%d%s", cl->host, cl->port, cl->path);
    return ZST_OK;
}

static zst_result_t el_close(zst_element_t* el) {
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    s->running = 0;
    if (s->client.fd >= 0) {
        shutdown(s->client.fd, SHUT_RDWR);
    }
    if (s->thread_started) {
        pthread_join(s->thread, NULL);
        s->thread_started = 0;
    }
    zst_caps_destroy(s->video_caps);
    zst_caps_destroy(s->audio_caps);
    s->video_caps = NULL;
    s->audio_caps = NULL;
    if (s->lock_initialized) {
        pthread_mutex_destroy(&s->lock);
        s->lock_initialized = 0;
    }
    return ZST_OK;
}

static zst_result_t el_start(zst_element_t* el) {
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&s->thread, NULL, streaming_thread, el->priv) != 0) {
        __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
        return ZST_ERROR;
    }
    s->thread_started = 1;
    return ZST_OK;
}

static zst_result_t el_stop(zst_element_t* el) {
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
    if (s->client.fd >= 0) {
        shutdown(s->client.fd, SHUT_RDWR);
    }
    if (s->thread_started) {
        pthread_join(s->thread, NULL);
        s->thread_started = 0;
    }
    return ZST_OK;
}

static zst_result_t el_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el; (void)in; (void)out;
    return ZST_OK; /* handled by thread */
}

static zst_result_t el_set_prop(zst_element_t* el, const char* name, const char* value) {
    if (!el || !name || !value) return ZST_ERROR;
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0 || strcmp(name, "rtsp_url") == 0 || strcmp(name, "rtsp-url") == 0) {
        strncpy(s->url, value, sizeof(s->url) - 1);
        s->url[sizeof(s->url) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "username") == 0) {
        strncpy(s->username, value, sizeof(s->username) - 1);
        s->username[sizeof(s->username) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "password") == 0) {
        strncpy(s->password, value, sizeof(s->password) - 1);
        s->password[sizeof(s->password) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "transport") == 0) {
        strncpy(s->transport, value, sizeof(s->transport) - 1);
        s->transport[sizeof(s->transport) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "buffer_size") == 0 || strcmp(name, "buffer-size") == 0) {
        s->buffer_size = atoi(value);
        if (s->buffer_size < 0) s->buffer_size = 0;
        return ZST_OK;
    }
    if (strcmp(name, "reconnect") == 0) {
        s->reconnect = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        s->reconnect_delay_ms = atoi(value);
        if (s->reconnect_delay_ms < 0) s->reconnect_delay_ms = 0;
        return ZST_OK;
    }
    if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        s->max_reconnect_attempts = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "keepalive-interval") == 0 || strcmp(name, "keepalive-interval-sec") == 0) {
        s->keepalive_interval_sec = atoi(value);
        if (s->keepalive_interval_sec < 0) s->keepalive_interval_sec = 0;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t el_get_prop(zst_element_t* el, const char* name, char* out, size_t max) {
    if (!el || !name || !out) return ZST_ERROR;
    rtsp_source_priv_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0 || strcmp(name, "rtsp_url") == 0 || strcmp(name, "rtsp-url") == 0) {
        strncpy(out, s->url, max - 1); out[max - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "username") == 0) {
        strncpy(out, s->username, max - 1); out[max - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "password") == 0) {
        strncpy(out, s->password, max - 1); out[max - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "transport") == 0) {
        strncpy(out, s->transport, max - 1); out[max - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "buffer_size") == 0 || strcmp(name, "buffer-size") == 0) {
        snprintf(out, max, "%d", s->buffer_size);
        return ZST_OK;
    }
    if (strcmp(name, "reconnect") == 0) {
        snprintf(out, max, "%s", s->reconnect ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        snprintf(out, max, "%d", s->reconnect_delay_ms);
        return ZST_OK;
    }
    if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        snprintf(out, max, "%d", s->max_reconnect_attempts);
        return ZST_OK;
    }
    if (strcmp(name, "keepalive-interval") == 0 || strcmp(name, "keepalive-interval-sec") == 0) {
        snprintf(out, max, "%d", s->keepalive_interval_sec);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name          = "rtspsrc",
    .open          = el_open,
    .close         = el_close,
    .start         = el_start,
    .stop          = el_stop,
    .process       = el_process,
    .get_caps      = get_caps,
    .set_property  = el_set_prop,
    .get_property  = el_get_prop,
};

zst_element_t* zst_rtsp_source_create(const char* url) {
    rtsp_source_priv_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (url) {
        strncpy(s->url, url, sizeof(s->url) - 1);
        s->url[sizeof(s->url) - 1] = '\0';
    }
    strncpy(s->transport, "tcp", sizeof(s->transport) - 1);
    s->buffer_size = RTSP_BUF_SIZE;
    s->reconnect = 0;
    s->reconnect_delay_ms = 500;
    s->max_reconnect_attempts = -1;
    s->keepalive_interval_sec = 30;

    zst_element_t* el = zst_element_create(&g_ops, s);
    if (!el) { free(s); return NULL; }

    s->video_pad = zst_pad_create("video", ZST_PAD_SRC);
    s->audio_pad = zst_pad_create("audio", ZST_PAD_SRC);
    if (!s->video_pad || !s->audio_pad) { zst_element_destroy(el); return NULL; }

    if (zst_element_add_pad(el, s->video_pad) != ZST_OK ||
        zst_element_add_pad(el, s->audio_pad) != ZST_OK) {
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create(const char* name) {
    if (strcmp(name, "rtspsrc") == 0) return zst_rtsp_source_create(NULL);
    return NULL;
}

static const zst_pad_template_t g_rtspsrc_pads[] = {
    { "video", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" },
    { "audio", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" }
};

static const zst_property_spec_t g_rtspsrc_properties[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP URL" },
    { "rtsp_url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "username", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP username" },
    { "password", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTSP password" },
    { "transport", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "RTSP transport: tcp, udp, or multicast" },
    { "buffer-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16384", "Receive buffer size" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on transport loss" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" },
    { "keepalive-interval-sec", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "RTSP OPTIONS keepalive interval in seconds" }
};

static const zst_element_desc_t g_rtspsrc_elements[] = {
    {
        .name = "rtspsrc",
        .long_name = "RTSP Source",
        .category = "Source/Network",
        .description = "Receives audio/video from an RTSP endpoint",
        .author = "zstreamer",
        .properties = g_rtspsrc_properties,
        .nb_properties = sizeof(g_rtspsrc_properties) / sizeof(g_rtspsrc_properties[0]),
        .pads = g_rtspsrc_pads,
        .nb_pads = sizeof(g_rtspsrc_pads) / sizeof(g_rtspsrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name    = "rtspsrc_plugin",
        .author  = "zstreamer",
        .version = "1.0.0",
        .init    = NULL,
        .deinit  = NULL
    },
    .create_element = plugin_create
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_rtspsrc_elements) / sizeof(g_rtspsrc_elements[0]);
    }
    return g_rtspsrc_elements;
}

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void) {
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
