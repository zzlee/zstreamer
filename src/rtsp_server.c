/*=============================================================================
    rtsp_server.c — RTSP server element supporting multiple sessions

    Serves multiple RTSP streams on a single port, each with its own
    mount point (URI path). Each mount point maps to a named pair of
    sink pads (video/audio). Connected clients DESCRIBE/SETUP/PLAY
    and receive RTP over TCP interleaved or UDP unicast transport.

    Architecture inspired by ireader/media-server:
      - rtsp_server_listen() → per-client rtsp_server_t
      - URI-based routing in ondescribe/onsetup handlers
      - RTP over RTSP interleaved binary framing ($ + channel + len + data)
      - UDP transport (RFC 3550): RTP on even port, RTCP on next odd port

    Transport negotiation (RFC 2326 §12.39):
      - Client requests transport in SETUP via Transport header
      - Server parses client_port, interleaved parameters
      - Server allocates resources and echoes back the chosen transport

    References:
      - RFC 2326 (RTSP)
      - RFC 3550 (RTP/RTCP)
      - RFC 3984 (H.264 over RTP)
      - RFC 3640 (AAC over RTP)
      - https://github.com/yuan88yuan/ireader-media-server
=============================================================================*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_rtsp_server.h"
#include "zstreamer/elements/zst_rtsp_server.h"

/*===========================================================================
    Constants
===========================================================================*/
#define RTSP_DEFAULT_PORT      8554
#define RTSP_SESSION_ID_LEN    32
#define RTSP_MAX_SESSIONS      16
#define RTSP_BUF_SIZE          8192
#define RTSP_REPLY_SIZE        8192
#define RTSP_SDP_SIZE          4096
#define RTP_MTU                1400
#define RTCP_INTERVAL_MS       5000

/* RTP payload types (dynamic) */
#define RTP_PT_H264            96
#define RTP_PT_H265            97
#define RTP_PT_AAC             98

#define RTP_CLOCK_VIDEO        90000

/* H.264 NAL types */
#define H264_NAL_SPS           7
#define H264_NAL_PPS           8

/* Transport type constants */
#define RTSP_TRANSPORT_TCP       0   /* RTP/AVP/TCP interleaved */
#define RTSP_TRANSPORT_UDP       1   /* RTP/AVP unicast UDP */
#define RTSP_TRANSPORT_MULTICAST 2   /* RTP/AVP multicast UDP */

/* Default UDP port range for dynamic allocation */
#define RTSP_UDP_PORT_MIN     55000
#define RTSP_UDP_PORT_MAX     56000

/*===========================================================================
    RTP / RTCP packet headers (RFC 3550)
===========================================================================*/
#pragma pack(push, 1)
typedef struct {
    uint8_t  cc:4;
    uint8_t  x:1;
    uint8_t  p:1;
    uint8_t  version:2;
    uint8_t  pt:7;
    uint8_t  m:1;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_header_t;

typedef struct {
    uint32_t ssrc;
    uint64_t ntp_timestamp;
    uint32_t rtp_timestamp;
    uint32_t sender_packets;
    uint32_t sender_octets;
} rtcp_sr_body_t;

typedef struct {
    uint8_t  version:2;
    uint8_t  p:1;
    uint8_t  rc:5;
    uint8_t  pt;
    uint16_t length;
} rtcp_header_t;
#pragma pack(pop)

/*===========================================================================
    Forward declarations
===========================================================================*/
struct rtsp_server_session_s;
struct rtsp_server_priv_s;

/*===========================================================================
    Per-stream RTP state (one per client per media type)
===========================================================================*/
typedef struct {
    uint32_t ssrc;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t base_rtp_ts;
    uint64_t base_pts;        /* reference PTS in ns */
    uint32_t clock_rate;      /* 90000 for video, sample_rate for audio */
    uint8_t  payload_type;
    int      codec;           /* 1=H264, 2=H265, 3=AAC */
    int      packet_count;
    int      octet_count;
} rtp_stream_state_t;

/*===========================================================================
    Per-client RTSP connection
===========================================================================*/
typedef struct rtsp_client_s {
    int                     fd;
    char                    peer_ip[64];
    uint16_t                peer_port;

    /* Receive buffer */
    char                    buf[RTSP_BUF_SIZE];
    int                     buf_len;

    /* Parsed request */
    char                    method[16];
    char                    uri[512];
    unsigned int            cseq;
    char                    session_id[RTSP_SESSION_ID_LEN];
    char                    transport_hdr[256]; /* raw Transport header from SETUP */

    /* Transport type: RTSP_TRANSPORT_TCP, RTSP_TRANSPORT_UDP, RTSP_TRANSPORT_MULTICAST */
    int                     transport_type;

    /* RTP interleaved channels (TCP mode) */
    int                     interleaved_rtp;
    int                     interleaved_rtcp;

    /* UDP transport state */
    int                     udp_rtp_fd;     /* server UDP socket for RTP */
    int                     udp_rtcp_fd;    /* server UDP socket for RTCP */
    uint16_t                server_rtp_port;
    uint16_t                server_rtcp_port;
    struct sockaddr_in      client_rtp_addr;
    struct sockaddr_in      client_rtcp_addr;
    uint16_t                client_rtp_port;
    uint16_t                client_rtcp_port;

    /* Track setup mask: bit 0=video set up, bit 1=audio set up */
    int                     track_setup_mask;

    /* Linked server session (mount point) */
    struct rtsp_server_session_s* session;
    int                     play_state;  /* 0=init, 1=playing, 2=paused */

    /* Per-stream RTP state */
    rtp_stream_state_t      vstream;     /* video */
    rtp_stream_state_t      astream;     /* audio */

    /* Threading */
    pthread_t               thread;
    int                     running;
    struct rtsp_client_s*   next;

    /* Back-pointer to server */
    struct rtsp_server_priv_s* server;
} rtsp_client_t;

/*===========================================================================
    Server session: one mount point with video/audio sink pads
===========================================================================*/
typedef struct rtsp_server_session_s {
    char        name[64];
    zst_pad_t*  video_pad;
    zst_pad_t*  audio_pad;
    int         has_video;
    int         has_audio;
    int         video_codec;   /* 1=H264, 2=H265 */
    int         audio_codec;   /* 3=AAC */
    int         width;
    int         height;
    double      framerate;
    int         sample_rate;
    int         channels;
    /* SPS/PPS for H.264 SDP fmtp */
    uint8_t*    extra_data;
    int         extra_size;
} rtsp_server_session_t;

/*===========================================================================
    Server element private data
===========================================================================*/
typedef struct rtsp_server_priv_s {
    int                     listen_port;
    int                     listen_fd;
    int                     running;
    pthread_t               listen_thread;
    pthread_mutex_t         lock;

    rtsp_server_session_t   sessions[RTSP_MAX_SESSIONS];
    int                     session_count;

    rtsp_client_t*          clients;
    int                     client_count;
} rtsp_server_priv_t;

/*===========================================================================
    Helpers
===========================================================================*/
static uint32_t rand32(void) {
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static uint64_t ntp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ntp = (uint64_t)ts.tv_sec + 2208988800ULL;
    ntp = (ntp << 32) | (uint32_t)((double)ts.tv_nsec * 4.294967296);
    return ntp;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static int make_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0) { close(fd); return -1; }
    make_nonblock(fd);
    return fd;
}

/*===========================================================================
    RTP header construction
===========================================================================*/
static void build_rtp_hdr(uint8_t* buf, rtp_stream_state_t* st, int marker_bit) {
    rtp_header_t* h = (rtp_header_t*)buf;
    h->version   = 2;
    h->p = h->x = h->cc = 0;
    h->m         = marker_bit ? 1 : 0;
    h->pt        = st->payload_type;
    h->seq       = htons(st->seq++);
    h->timestamp = htonl(st->timestamp);
    h->ssrc      = htonl(st->ssrc);
}

/*===========================================================================
    H.264 RTP packetization (RFC 3984)
      - Single NAL unit if fits in MTU
      - FU-A fragmentation for larger NALs
===========================================================================*/
typedef int (*packet_sink_t)(void* ctx, const uint8_t* data, int len);

static int h264_packetize(rtp_stream_state_t* st,
                           const uint8_t* nal, int nal_len,
                           packet_sink_t sink, void* ctx)
{
    if (nal_len < 1) return 0;

    uint8_t nal_type = nal[0] & 0x1f;
    uint8_t nri      = (nal[0] >> 5) & 0x03;
    int     count    = 0;

    if (nal_len <= RTP_MTU - 12) {
        uint8_t pkt[RTP_MTU];
        build_rtp_hdr(pkt, st, 1);
        memcpy(pkt + 12, nal, nal_len);
        if (sink(ctx, pkt, 12 + nal_len) < 0) return -1;
        return 1;
    }

    /* FU-A */
    int off = 1, rem = nal_len - 1, first = 1;
    while (rem > 0) {
        int chunk = rem;
        if (chunk > RTP_MTU - 14) chunk = RTP_MTU - 14;
        uint8_t pkt[RTP_MTU];
        build_rtp_hdr(pkt, st, (rem == chunk) ? 1 : 0);
        pkt[12] = (uint8_t)((nri << 5) | 28);              /* FU-A indicator */
        pkt[13] = (uint8_t)((first ? 0x80 : 0) |           /* FU-A header  */
                           ((rem == chunk) ? 0x40 : 0) |
                           nal_type);
        memcpy(pkt + 14, nal + off, chunk);
        if (sink(ctx, pkt, 14 + chunk) < 0) return -1;
        off += chunk; rem -= chunk; first = 0; count++;
    }
    return count;
}

/*===========================================================================
    AAC RTP packetization (RFC 3640 MPEG4-Generic)
      - 2-byte AU-headers-length + 2-byte AU-header + raw frame
===========================================================================*/
static int aac_packetize(rtp_stream_state_t* st,
                          const uint8_t* aac, int aac_len,
                          packet_sink_t sink, void* ctx)
{
    int total = 4 + aac_len;
    uint8_t pkt[RTP_MTU];
    build_rtp_hdr(pkt, st, 1);
    int off = 12;
    /* AU-headers-length (in bits) = 16 */
    pkt[off++] = 0;
    pkt[off++] = 16;
    /* AU-header: 13-bit size (in bits) + 3-bit index=0 */
    uint16_t au = (uint16_t)(aac_len << 3);
    pkt[off++] = (uint8_t)(au >> 8);
    pkt[off++] = (uint8_t)(au & 0xff);
    memcpy(pkt + off, aac, aac_len);
    if (sink(ctx, pkt, off + aac_len) < 0) return -1;
    return 1;
}

/*===========================================================================
    RTCP Sender Report
===========================================================================*/
static int send_rtcp_sr(rtsp_client_t* cl, int is_video) {
    rtp_stream_state_t* st = is_video ? &cl->vstream : &cl->astream;
    if (!st->packet_count) return 0;

    /* RTCP Sender Report: 4-byte header + 24-byte SR body = 28 bytes (RFC 3550) */
    uint8_t buf[28];
    rtcp_header_t* h  = (rtcp_header_t*)buf;
    rtcp_sr_body_t* s = (rtcp_sr_body_t*)(buf + 4);

    h->version = 2; h->p = 0; h->rc = 0; h->pt = 200;
    /* length field = (total 32-bit words) - 1 = (28/4) - 1 = 6 */
    h->length  = htons(((int)sizeof(rtcp_sr_body_t) + (int)sizeof(rtcp_header_t)) / 4 - 1);

    s->ssrc           = htonl(st->ssrc);
    s->ntp_timestamp  = ntp_now();
    s->rtp_timestamp  = htonl(st->timestamp);
    s->sender_packets = htonl(st->packet_count);
    s->sender_octets  = htonl(st->octet_count);

    int slen = (int)sizeof(buf); /* 28 bytes */

    if (cl->transport_type == RTSP_TRANSPORT_UDP && cl->udp_rtcp_fd >= 0) {
        /* Send RTCP SR via UDP */
        int n = sendto(cl->udp_rtcp_fd, buf, slen, 0,
                       (struct sockaddr*)&cl->client_rtcp_addr,
                       sizeof(cl->client_rtcp_addr));
        return (n == slen) ? 0 : -1;
    }

    /* TCP interleaved framing */
    uint8_t frame[4 + 28];
    frame[0] = '$';
    frame[1] = (uint8_t)cl->interleaved_rtcp;
    frame[2] = (uint8_t)((slen >> 8) & 0xff);
    frame[3] = (uint8_t)(slen & 0xff);
    memcpy(frame + 4, buf, slen);

    return (write(cl->fd, frame, 4 + slen) == 4 + slen) ? 0 : -1;
}

/*===========================================================================
    SDP generation for a session
===========================================================================*/
static int make_sdp(rtsp_server_session_t* sess, char* out, int cap) {
    int n = 0;
    uint64_t now = (uint64_t)time(NULL) + 2208988800ULL;

    n += snprintf(out + n, cap - n,
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=%s\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "a=range:npt=now-\r\n"
        "a=recvonly\r\n"
        "a=control:*\r\n",
        (unsigned long long)now, (unsigned long long)now, sess->name);

    if (sess->has_video) {
        int pt = (sess->video_codec == 1) ? RTP_PT_H264 : RTP_PT_H265;
        const char* enc = (sess->video_codec == 1) ? "H264" : "H265";
        n += snprintf(out + n, cap - n,
            "m=video 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d %s/%d\r\n",
            pt, pt, enc, RTP_CLOCK_VIDEO);
        /* fmtp with profile-level-id */
        n += snprintf(out + n, cap - n,
            "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f\r\n"
            "a=control:trackID=0\r\n", pt);
    }

    if (sess->has_audio) {
        int sr = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        int ch = sess->channels > 0 ? sess->channels : 2;
        n += snprintf(out + n, cap - n,
            "m=audio 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d MPEG4-GENERIC/%d/%d\r\n"
            "a=fmtp:%d streamtype=5;profile-level-id=1;"
            "mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3\r\n"
            "a=control:trackID=1\r\n",
            RTP_PT_AAC, RTP_PT_AAC, sr, ch, RTP_PT_AAC);
    }
    return n;
}

/*===========================================================================
    RTSP response helpers
===========================================================================*/
static const char* reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 454: return "Session Not Found";
        case 461: return "Unsupported Transport";
        case 500: return "Internal Server Error";
        case 505: return "RTSP Version Not Supported";
        default:  return "Unknown";
    }
}

static int send_reply(rtsp_client_t* cl, int code,
                       const char* extra_hdrs,
                       const char* body, int body_len)
{
    char reply[RTSP_REPLY_SIZE];
    time_t t = time(NULL);
    struct tm tm; gmtime_r(&t, &tm);
    char date[64]; strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    int n = snprintf(reply, sizeof(reply),
        "RTSP/1.0 %d %s\r\n"
        "CSeq: %u\r\n"
        "Date: %s\r\n"
        "User-Agent: zstreamer/1.0\r\n",
        code, reason_phrase(code), cl->cseq, date);

    if (cl->session_id[0])
        n += snprintf(reply + n, sizeof(reply) - n,
            "Session: %s\r\n", cl->session_id);
    if (extra_hdrs)
        n += snprintf(reply + n, sizeof(reply) - n, "%s", extra_hdrs);

    if (body && body_len > 0) {
        n += snprintf(reply + n, sizeof(reply) - n,
            "Content-Length: %d\r\n\r\n", body_len);
        if (n + body_len <= (int)sizeof(reply)) {
            memcpy(reply + n, body, body_len);
            n += body_len;
        }
    } else {
        n += snprintf(reply + n, sizeof(reply) - n, "Content-Length: 0\r\n\r\n");
    }

    int r = write(cl->fd, reply, n);
    return (r == n) ? 0 : -1;
}

static int reply_simple(rtsp_client_t* cl, int code) {
    return send_reply(cl, code, NULL, NULL, 0);
}

/*===========================================================================
    RTSP request parser
===========================================================================*/
/* Returns: 0=full request parsed, 1=need more data, <0=error */
static int parse_rtsp_request(rtsp_client_t* cl) {
    char* buf = cl->buf;
    int   len = cl->buf_len;

    /* Find \r\n\r\n end-of-headers */
    char* eoh = strstr(buf, "\r\n\r\n");
    if (!eoh) return 1;

    int hdr_len = (int)(eoh - buf) + 4;

    /* Parse request line */
    char* eol = strstr(buf, "\r\n");
    if (!eol) return 400;
    *eol = '\0';

    char* method = strtok(buf, " ");
    char* uri    = strtok(NULL, " ");
    char* ver    = strtok(NULL, " ");
    if (!method || !uri || !ver) return 400;

    strncpy(cl->method, method, sizeof(cl->method) - 1);
    strncpy(cl->uri,    uri,    sizeof(cl->uri) - 1);
    cl->cseq = 0;
    cl->session_id[0] = '\0';
    cl->transport_hdr[0] = '\0';

    /* Parse headers */
    char* h = eol + 2;
    while (h < eoh) {
        char* he = strstr(h, "\r\n");
        if (!he || he > eoh) break;
        *he = '\0';
        char* colon = strchr(h, ':');
        if (colon) {
            *colon = '\0';
            char* hv = colon + 1;
            while (*hv == ' ') hv++;
            if (strcasecmp(h, "CSeq") == 0)
                cl->cseq = (unsigned int)atoi(hv);
            else if (strcasecmp(h, "Session") == 0) {
                char* sp = strchr(hv, ';');
                if (sp) *sp = '\0';
                strncpy(cl->session_id, hv, sizeof(cl->session_id) - 1);
            } else if (strcasecmp(h, "Transport") == 0) {
                strncpy(cl->transport_hdr, hv, sizeof(cl->transport_hdr) - 1);
            }
        }
        h = he + 2;
    }

    /* Consume from buffer */
    if (hdr_len < len)
        memmove(buf, buf + hdr_len, len - hdr_len);
    cl->buf_len = len - hdr_len;
    return 0;
}

/*===========================================================================
    Extract mount point from URI
    Returns pointer into uri or NULL
===========================================================================*/
static const char* extract_mount(const char* uri) {
    const char* p = strstr(uri, "://");
    if (p) {
        p = strchr(p + 3, '/');
        if (!p) return NULL;
    } else {
        p = uri;
    }
    while (*p == '/') p++;
    return p;
}

/*===========================================================================
    Transport header parser (RFC 2326 §12.39)

    Parses: Transport: RTP/AVP/TCP;interleaved=0-1
            Transport: RTP/AVP;unicast;client_port=6970-6971
            Transport: RTP/AVP;multicast;destination=224.0.0.1;port=5000-5001;ttl=16

    Returns 0 on success with fields set, -1 on parse error.
===========================================================================*/
static int parse_transport_header(const char* field,
                                   int* transport_type,
                                   uint16_t* client_port1, uint16_t* client_port2,
                                   int* interleaved1, int* interleaved2,
                                   int* multicast, char* destination, int* ttl)
{
    const char* p = field;
    *transport_type = RTSP_TRANSPORT_TCP;
    *client_port1 = *client_port2 = 0;
    *interleaved1 = *interleaved2 = -1;
    *multicast = 0;
    *ttl = 0;
    if (destination) destination[0] = '\0';

    if (!p) return -1;

    /* Use strtok to split on ';' and ',' */
    char buf[256];
    strncpy(buf, field, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* delims = ";,";
    char* tok = strtok(buf, delims);
    while (tok) {
        /* Skip leading spaces */
        while (*tok == ' ') tok++;

        if (strncasecmp(tok, "RTP/AVP/TCP", 11) == 0)
            *transport_type = RTSP_TRANSPORT_TCP;
        else if (strncasecmp(tok, "RTP/AVP/UDP", 11) == 0)
            *transport_type = RTSP_TRANSPORT_UDP;
        else if (strncasecmp(tok, "RTP/AVP", 7) == 0)
            *transport_type = RTSP_TRANSPORT_UDP;
        else if (strncasecmp(tok, "unicast", 7) == 0)
            *multicast = 0;
        else if (strncasecmp(tok, "multicast", 9) == 0)
            *multicast = 1;
        else if (sscanf(tok, "client_port=%hu-%hu", client_port1, client_port2) >= 1) {
            if (*client_port2 == 0) *client_port2 = *client_port1 + 1;
            *client_port1 = (*client_port1 / 2) * 2; /* even */
            *client_port2 = *client_port1 + 1;
        }
        else if (sscanf(tok, "interleaved=%d-%d", interleaved1, interleaved2) >= 1) {
            if (*interleaved2 < 0) *interleaved2 = *interleaved1 + 1;
        }
        else if (sscanf(tok, "ttl=%d", ttl) == 1) {
        }
        else if (strncasecmp(tok, "destination=", 12) == 0 && destination) {
            strncpy(destination, tok + 12, 64);
            destination[63] = '\0';
        }

        tok = strtok(NULL, delims);
    }

    /* If multicast was explicitly requested, set type */
    if (*multicast) *transport_type = RTSP_TRANSPORT_MULTICAST;

    /* Default interleaved values for TCP if not specified */
    if (*interleaved1 < 0 && *transport_type == RTSP_TRANSPORT_TCP) {
        *interleaved1 = 0;
        *interleaved2 = 1;
    }

    return 0;
}

/*===========================================================================
    UDP socket helpers
===========================================================================*/
static int create_udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    make_nonblock(fd);
    return fd;
}

/* Bind UDP socket to ephemeral port and return the assigned port */
static int bind_udp_ephemeral(int fd, uint16_t* out_port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0; /* ephemeral */
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) return -1;

    socklen_t alen = sizeof(a);
    if (getsockname(fd, (struct sockaddr*)&a, &alen) < 0) return -1;
    *out_port = ntohs(a.sin_port);
    return 0;
}

/*===========================================================================
    RTSP method handlers
===========================================================================*/
static int on_options(rtsp_client_t* cl) {
    (void)cl;
    return send_reply(cl, 200,
        "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, OPTIONS\r\n",
        NULL, 0);
}

static int on_describe(rtsp_client_t* cl) {
    const char* mount = extract_mount(cl->uri);
    if (!mount || !*mount) return reply_simple(cl, 404);

    rtsp_server_priv_t* srv = cl->server;
    pthread_mutex_lock(&srv->lock);
    rtsp_server_session_t* sess = NULL;
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, mount) == 0) {
            sess = &srv->sessions[i];
            break;
        }
    }
    pthread_mutex_unlock(&srv->lock);

    if (!sess) return reply_simple(cl, 404);

    cl->session = sess;

    char sdp[RTSP_SDP_SIZE];
    int  sdp_len = make_sdp(sess, sdp, sizeof(sdp));

    char extras[256];
    snprintf(extras, sizeof(extras),
        "Content-Type: application/sdp\r\n"
        "Content-Base: rtsp://%s:%d/%s/\r\n",
        cl->peer_ip, RTSP_DEFAULT_PORT, sess->name);

    return send_reply(cl, 200, extras, sdp, sdp_len);
}

static int on_setup(rtsp_client_t* cl) {
    if (!cl->session) return reply_simple(cl, 454);

    rtsp_server_session_t* sess = cl->session;

    /* Determine which track is being set up from the URI */
    const char* track = strstr(cl->uri, "trackID=");
    int is_video_track = 0;
    int is_audio_track = 0;
    if (track) {
        int tid = atoi(track + 8);
        if (tid == 0) is_video_track = 1;
        else if (tid == 1) is_audio_track = 1;
    } else {
        /* No track ID — set up both on first SETUP */
        is_video_track = sess->has_video;
        is_audio_track = sess->has_audio;
    }

    /* Parse Transport header */
    int transport_type_parsed = RTSP_TRANSPORT_TCP;
    uint16_t cport1 = 0, cport2 = 0;
    int il1 = -1, il2 = -1;
    int multicast = 0;
    char destination[64];
    int ttl = 0;

    int has_transport = 0;
    if (cl->transport_hdr[0]) {
        has_transport = 1;
        parse_transport_header(cl->transport_hdr,
                                &transport_type_parsed,
                                &cport1, &cport2,
                                &il1, &il2,
                                &multicast, destination, &ttl);
    }

    /* If this is the first SETUP, establish the transport */
    if (cl->track_setup_mask == 0) {
        /* Generate session ID on first SETUP */
        snprintf(cl->session_id, sizeof(cl->session_id), "%08x", rand32());

        if (multicast) {
            /* Multicast not fully implemented; fall back to TCP */
            ZST_LOG_WARN("rtsp_server", "multicast not supported, falling back to TCP");
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = 0;
            cl->interleaved_rtcp = 1;
        } else if (transport_type_parsed == RTSP_TRANSPORT_TCP ||
                   (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 == 0 && has_transport)) {
            /* Client requested TCP interleaved, or UDP without client_port — use TCP */
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = (il1 >= 0) ? il1 : 0;
            cl->interleaved_rtcp = (il2 >= 0) ? il2 : cl->interleaved_rtp + 1;
        } else if (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 > 0) {
            /* Client requested UDP unicast with client_port */
            cl->transport_type = RTSP_TRANSPORT_UDP;

            /* Create server UDP sockets */
            cl->udp_rtp_fd = create_udp_socket();
            cl->udp_rtcp_fd = create_udp_socket();
            if (cl->udp_rtp_fd < 0 || cl->udp_rtcp_fd < 0) {
                if (cl->udp_rtp_fd >= 0) close(cl->udp_rtp_fd);
                if (cl->udp_rtcp_fd >= 0) close(cl->udp_rtcp_fd);
                cl->udp_rtp_fd = cl->udp_rtcp_fd = -1;
                return reply_simple(cl, 500);
            }

            /* Bind to ephemeral ports */
            if (bind_udp_ephemeral(cl->udp_rtp_fd, &cl->server_rtp_port) < 0 ||
                bind_udp_ephemeral(cl->udp_rtcp_fd, &cl->server_rtcp_port) < 0) {
                close(cl->udp_rtp_fd); close(cl->udp_rtcp_fd);
                cl->udp_rtp_fd = cl->udp_rtcp_fd = -1;
                return reply_simple(cl, 500);
            }

            /* Store client address and ports */
            memset(&cl->client_rtp_addr, 0, sizeof(cl->client_rtp_addr));
            cl->client_rtp_addr.sin_family = AF_INET;
            cl->client_rtp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
            cl->client_rtp_addr.sin_port = htons(cport1);

            memset(&cl->client_rtcp_addr, 0, sizeof(cl->client_rtcp_addr));
            cl->client_rtcp_addr.sin_family = AF_INET;
            cl->client_rtcp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
            cl->client_rtcp_addr.sin_port = htons(cport2);

            cl->client_rtp_port   = cport1;
            cl->client_rtcp_port  = cport2;

            ZST_LOG_INFO("rtsp_server", "UDP transport: client %s:%hu-%hu, server :%hu-%hu",
                         cl->peer_ip, cport1, cport2,
                         cl->server_rtp_port, cl->server_rtcp_port);
        } else {
            /* No transport header — default to TCP */
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = 0;
            cl->interleaved_rtcp = 1;
        }
    }

    /* Initialize RTP stream state(s) for this track */
    if (is_video_track && sess->has_video && !(cl->track_setup_mask & 1)) {
        memset(&cl->vstream, 0, sizeof(cl->vstream));
        cl->vstream.ssrc         = rand32();
        cl->vstream.seq          = (uint16_t)rand32();
        cl->vstream.timestamp    = rand32();
        cl->vstream.payload_type = (sess->video_codec == 1) ? RTP_PT_H264 : RTP_PT_H265;
        cl->vstream.clock_rate   = RTP_CLOCK_VIDEO;
        cl->vstream.codec        = sess->video_codec;
        cl->track_setup_mask |= 1;
    }

    if (is_audio_track && sess->has_audio && !(cl->track_setup_mask & 2)) {
        memset(&cl->astream, 0, sizeof(cl->astream));
        cl->astream.ssrc         = rand32();
        cl->astream.seq          = (uint16_t)rand32();
        cl->astream.timestamp    = rand32();
        cl->astream.payload_type = RTP_PT_AAC;
        cl->astream.clock_rate   = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        cl->astream.codec        = 3;
        cl->track_setup_mask |= 2;
    }

    /* Build Transport response header */
    char extra[256];
    if (cl->transport_type == RTSP_TRANSPORT_UDP) {
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/UDP;unicast;"
            "client_port=%hu-%hu;"
            "server_port=%hu-%hu\r\n",
            cl->client_rtp_port, cl->client_rtcp_port,
            cl->server_rtp_port, cl->server_rtcp_port);
    } else {
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/TCP;interleaved=%d-%d\r\n",
            cl->interleaved_rtp, cl->interleaved_rtcp);
    }

    return send_reply(cl, 200, extra, NULL, 0);
}

static int on_play(rtsp_client_t* cl) {
    if (!cl->session_id[0]) return reply_simple(cl, 454);
    cl->play_state = 1;

    char extra[512];
    int n = 0;
    n += snprintf(extra + n, sizeof(extra) - n,
        "Range: npt=0.000-\r\n"
        "RTP-Info: ");

    int first = 1;
    if (cl->track_setup_mask & 1) {
        if (!first) n += snprintf(extra + n, sizeof(extra) - n, ",");
        n += snprintf(extra + n, sizeof(extra) - n,
            "url=%s/trackID=0;seq=%u;rtptime=%u",
            cl->uri, (unsigned)cl->vstream.seq,
            (unsigned)cl->vstream.timestamp);
        first = 0;
    }
    if (cl->track_setup_mask & 2) {
        if (!first) n += snprintf(extra + n, sizeof(extra) - n, ",");
        n += snprintf(extra + n, sizeof(extra) - n,
            "url=%s/trackID=1;seq=%u;rtptime=%u",
            cl->uri, (unsigned)cl->astream.seq,
            (unsigned)cl->astream.timestamp);
    }

    return send_reply(cl, 200, extra, NULL, 0);
}

static int on_pause(rtsp_client_t* cl) {
    if (!cl->session_id[0]) return reply_simple(cl, 454);
    cl->play_state = 2;
    return reply_simple(cl, 200);
}

static int on_teardown(rtsp_client_t* cl) {
    cl->play_state = 0;
    return reply_simple(cl, 200);
}

/*===========================================================================
    Main RTSP dispatch
===========================================================================*/
static int dispatch_rtsp(rtsp_client_t* cl) {
    if      (strcasecmp(cl->method, "OPTIONS")   == 0) return on_options(cl);
    else if (strcasecmp(cl->method, "DESCRIBE")  == 0) return on_describe(cl);
    else if (strcasecmp(cl->method, "SETUP")     == 0) return on_setup(cl);
    else if (strcasecmp(cl->method, "PLAY")      == 0) return on_play(cl);
    else if (strcasecmp(cl->method, "PAUSE")     == 0) return on_pause(cl);
    else if (strcasecmp(cl->method, "TEARDOWN")  == 0) return on_teardown(cl);
    return reply_simple(cl, 501);
}

/*===========================================================================
    RTP packet send — TCP interleaved or UDP unicast
===========================================================================*/
static void write_rtp_packet(rtsp_client_t* cl, const uint8_t* data, int len) {
    if (cl->transport_type == RTSP_TRANSPORT_UDP && cl->udp_rtp_fd >= 0) {
        /* Send raw RTP packet via UDP */
        sendto(cl->udp_rtp_fd, data, len, 0,
               (struct sockaddr*)&cl->client_rtp_addr,
               sizeof(cl->client_rtp_addr));
    } else {
        /* TCP interleaved framing ($ + channel + 2-byte length + data) */
        uint8_t frame[4];
        frame[0] = '$';
        frame[1] = (uint8_t)cl->interleaved_rtp;
        frame[2] = (uint8_t)((len >> 8) & 0xff);
        frame[3] = (uint8_t)(len & 0xff);

        struct iovec iov[2] = {
            { .iov_base = frame, .iov_len = 4 },
            { .iov_base = (void*)data, .iov_len = (size_t)len }
        };
        struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
        sendmsg(cl->fd, &msg, MSG_NOSIGNAL);
    }
}

/* Context for packetization callbacks */
typedef struct {
    rtsp_client_t*    cl;
    rtp_stream_state_t* stream;
} send_ctx_t;

static int packet_send_cb(void* ctx, const uint8_t* data, int len) {
    send_ctx_t* sc = (send_ctx_t*)ctx;
    write_rtp_packet(sc->cl, data, len);
    sc->stream->packet_count++;
    sc->stream->octet_count += len - 12;
    return 0;
}

/*===========================================================================
    Send codec data to all clients subscribed to a session
===========================================================================*/
static void session_deliver(rtsp_server_priv_t* srv,
                             rtsp_server_session_t* sess,
                             zst_buffer_t* buf)
{
    int is_video = (buf->type == ZST_BUFFER_VIDEO_PACKET);
    int is_audio = (buf->type == ZST_BUFFER_AUDIO_PACKET);
    if (!is_video && !is_audio) return;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) return;

    pthread_mutex_lock(&srv->lock);

    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next) {
        if (cl->session != sess || cl->play_state != 1) continue;

        rtp_stream_state_t* st = is_video ? &cl->vstream : &cl->astream;
        if (st->codec == 0) continue;

        /* Initialize base PTS on first data */
        if (st->base_pts == 0) {
            st->base_pts    = buf->pts;
            st->base_rtp_ts = st->timestamp;
        }

        /* Convert PTS delta to RTP timestamp */
        int64_t delta = (int64_t)(buf->pts - st->base_pts);
        if (delta < 0) delta = 0;
        st->timestamp = st->base_rtp_ts +
            (uint32_t)((delta * st->clock_rate) / 1000000000ULL);

        send_ctx_t sc = { .cl = cl, .stream = st };

        if (is_video && st->codec == 1) {
            /* H.264: walk NAL units separated by 00 00 00 01 or 00 00 01 */
            const uint8_t* d = buf->memory.data;
            int sz = (int)buf->memory.size;
            int i = 0;
            while (i < sz) {
                /* Locate start code */
                if (i + 2 < sz && d[i] == 0 && d[i+1] == 0) {
                    int nal_start;
                    int code_len;
                    if (i + 3 < sz && d[i+2] == 1) {
                        nal_start = i + 3; code_len = 3;
                    } else if (i + 4 < sz && d[i+2] == 0 && d[i+3] == 1) {
                        nal_start = i + 4; code_len = 4;
                    } else { i++; continue; }
                    i += code_len;

                    /* Find next start code */
                    int nal_end = sz;
                    for (int j = i; j + 3 < sz; j++) {
                        if (d[j] == 0 && d[j+1] == 0 &&
                            (d[j+2] == 1 ||
                             (j + 4 < sz && d[j+2] == 0 && d[j+3] == 1))) {
                            nal_end = j;
                            break;
                        }
                    }
                    int nal_len = nal_end - nal_start;
                    if (nal_len > 0)
                        h264_packetize(st, d + nal_start, nal_len,
                                       packet_send_cb, &sc);
                    i = nal_end;
                } else {
                    i++;
                }
            }
        } else if (is_audio && st->codec == 3) {
            aac_packetize(st, buf->memory.data, (int)buf->memory.size,
                          packet_send_cb, &sc);
        }
    }

    pthread_mutex_unlock(&srv->lock);
}

/*===========================================================================
    Pad push callbacks (called by upstream elements)
===========================================================================*/
static zst_result_t video_push_cb(zst_pad_t* pad, zst_buffer_t* buf) {
    rtsp_server_priv_t* srv = pad->parent->priv;
    for (int i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i].video_pad == pad) {
            session_deliver(srv, &srv->sessions[i], buf);
            return ZST_OK;
        }
    }
    return ZST_ERROR;
}

static zst_result_t audio_push_cb(zst_pad_t* pad, zst_buffer_t* buf) {
    rtsp_server_priv_t* srv = pad->parent->priv;
    for (int i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i].audio_pad == pad) {
            session_deliver(srv, &srv->sessions[i], buf);
            return ZST_OK;
        }
    }
    return ZST_ERROR;
}

/*===========================================================================
    Client connection thread
===========================================================================*/
static void* client_thread(void* arg) {
    rtsp_client_t* cl = (rtsp_client_t*)arg;
    cl->running = 1;

    ZST_LOG_INFO("rtsp_server", "client connected: %s:%u",
                 cl->peer_ip, cl->peer_port);

    uint64_t last_rtcp = 0;

    while (cl->running) {
        struct pollfd pfd = { .fd = cl->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        uint64_t now = now_us();
        if (cl->play_state == 1 && (now - last_rtcp) > RTCP_INTERVAL_MS * 1000) {
            last_rtcp = now;
            if (cl->vstream.codec) send_rtcp_sr(cl, 1);
            if (cl->astream.codec) send_rtcp_sr(cl, 0);
        }

        if (ret == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        int n = (int)read(cl->fd, cl->buf + cl->buf_len,
                          sizeof(cl->buf) - cl->buf_len);
        if (n <= 0) break;
        cl->buf_len += n;

        /* Process all complete messages */
        while (cl->buf_len > 0) {
            /* Skip interleaved binary data from client */
            if ((uint8_t)cl->buf[0] == '$') {
                if (cl->buf_len < 4) break;
                int dlen = ((uint8_t)cl->buf[2] << 8) | (uint8_t)cl->buf[3];
                int total = 4 + dlen;
                if (cl->buf_len < total) break;
                memmove(cl->buf, cl->buf + total, cl->buf_len - total);
                cl->buf_len -= total;
                continue;
            }
            int r = parse_rtsp_request(cl);
            if (r == 1) break;
            if (r != 0) { reply_simple(cl, r); break; }
            dispatch_rtsp(cl);
        }
    }

    ZST_LOG_INFO("rtsp_server", "client disconnected: %s:%u",
                 cl->peer_ip, cl->peer_port);

    /* Remove from server list */
    if (cl->server) {
        pthread_mutex_lock(&cl->server->lock);
        rtsp_client_t** pp = &cl->server->clients;
        while (*pp) {
            if (*pp == cl) { *pp = cl->next; cl->server->client_count--; break; }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&cl->server->lock);
    }

    /* Close TCP socket */
    close(cl->fd);

    /* Close UDP sockets if allocated */
    if (cl->udp_rtp_fd >= 0) {
        close(cl->udp_rtp_fd);
        cl->udp_rtp_fd = -1;
    }
    if (cl->udp_rtcp_fd >= 0) {
        close(cl->udp_rtcp_fd);
        cl->udp_rtcp_fd = -1;
    }

    free(cl);
    return NULL;
}

/*===========================================================================
    Listen thread
===========================================================================*/
static void* listen_thread(void* arg) {
    rtsp_server_priv_t* srv = (rtsp_server_priv_t*)arg;
    ZST_LOG_INFO("rtsp_server", "listening on port %d", srv->listen_port);

    while (srv->running) {
        struct pollfd pfd = { .fd = srv->listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;

        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(srv->listen_fd, (struct sockaddr*)&addr, &alen);
        if (fd < 0) continue;

        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        rtsp_client_t* cl = calloc(1, sizeof(*cl));
        if (!cl) { close(fd); continue; }

        cl->fd = fd;
        inet_ntop(AF_INET, &addr.sin_addr, cl->peer_ip, sizeof(cl->peer_ip));
        cl->peer_port = ntohs(addr.sin_port);
        cl->server = srv;
        cl->transport_type   = RTSP_TRANSPORT_TCP;
        cl->interleaved_rtp  = 0;
        cl->interleaved_rtcp = 1;
        cl->udp_rtp_fd   = -1;
        cl->udp_rtcp_fd  = -1;
        cl->track_setup_mask = 0;

        pthread_mutex_lock(&srv->lock);
        cl->next = srv->clients;
        srv->clients = cl;
        srv->client_count++;
        pthread_mutex_unlock(&srv->lock);

        pthread_create(&cl->thread, NULL, client_thread, cl);
        pthread_detach(cl->thread);
    }

    ZST_LOG_INFO("rtsp_server", "listen thread stopped");
    return NULL;
}

/*===========================================================================
    Element ops
===========================================================================*/
static zst_caps_t* get_caps(zst_element_t* el, zst_pad_t* pad,
                             const zst_caps_t* filter)
{
    (void)filter;
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return NULL;

    for (int i = 0; i < srv->session_count; i++) {
        if (pad == srv->sessions[i].video_pad) {
            zst_caps_t* c = zst_caps_create();
            zst_caps_append(c, zst_caps_struct_create_video("video/x-h264", 0,0,0,""));
            zst_caps_append(c, zst_caps_struct_create_video("video/x-h265", 0,0,0,""));
            return c;
        }
        if (pad == srv->sessions[i].audio_pad) {
            zst_caps_t* c = zst_caps_create();
            zst_caps_append(c, zst_caps_struct_create_audio("audio/aac", 0,0,""));
            return c;
        }
    }
    return NULL;
}

static zst_result_t el_open(zst_element_t* el) {
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;
    srv->listen_fd  = -1;
    srv->running    = 0;
    srv->clients    = NULL;
    srv->client_count = 0;
    return ZST_OK;
}

static zst_result_t el_close(zst_element_t* el) {
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    srv->running = 0;
    if (srv->listen_thread) {
        pthread_join(srv->listen_thread, NULL);
        srv->listen_thread = 0;
    }

    /* Close all clients to wake up their threads */
    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next)
        shutdown(cl->fd, SHUT_RDWR);
    pthread_mutex_unlock(&srv->lock);

    /* Small delay for threads to notice */
    usleep(100000);

    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }

    for (int i = 0; i < srv->session_count; i++)
        free(srv->sessions[i].extra_data);

    return ZST_OK;
}

static zst_result_t el_start(zst_element_t* el) {
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    srv->listen_fd = create_tcp_listener(srv->listen_port);
    if (srv->listen_fd < 0) {
        ZST_LOG_ERROR("rtsp_server", "cannot listen on port %d", srv->listen_port);
        return ZST_ERROR;
    }

    srv->running = 1;
    pthread_create(&srv->listen_thread, NULL, listen_thread, srv);

    ZST_LOG_INFO("rtsp_server", "started port=%d sessions=%d",
                 srv->listen_port, srv->session_count);
    return ZST_OK;
}

static zst_result_t el_stop(zst_element_t* el) { return el_close(el); }

static zst_result_t el_set_prop(zst_element_t* el, const char* name,
                                 const char* value)
{
    rtsp_server_priv_t* srv = el->priv;
    if (!srv || !name || !value) return ZST_ERROR;

    if (strcmp(name, "listen_port") == 0 || strcmp(name, "listen-port") == 0) {
        srv->listen_port = atoi(value);
        if (srv->listen_port <= 0) srv->listen_port = RTSP_DEFAULT_PORT;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t el_get_prop(zst_element_t* el, const char* name,
                                 char* out, size_t max)
{
    rtsp_server_priv_t* srv = el->priv;
    if (!srv || !name || !out) return ZST_ERROR;

    if (strcmp(name, "listen_port") == 0) {
        snprintf(out, max, "%d", srv->listen_port);
        return ZST_OK;
    }
    if (strcmp(name, "session_count") == 0) {
        snprintf(out, max, "%d", srv->session_count);
        return ZST_OK;
    }
    if (strcmp(name, "client_count") == 0) {
        snprintf(out, max, "%d", srv->client_count);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static const zst_element_ops_t g_ops = {
    .name          = "rtsp_server",
    .open          = el_open,
    .close         = el_close,
    .start         = el_start,
    .stop          = el_stop,
    .get_caps      = get_caps,
    .set_property  = el_set_prop,
    .get_property  = el_get_prop,
};

/*===========================================================================
    Public API
===========================================================================*/
zst_element_t* zst_rtsp_server_create(void) {
    rtsp_server_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    priv->listen_port = RTSP_DEFAULT_PORT;
    priv->listen_fd   = -1;
    pthread_mutex_init(&priv->lock, NULL);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) { pthread_mutex_destroy(&priv->lock); free(priv); return NULL; }
    return el;
}

zst_result_t zst_rtsp_server_add_session(zst_element_t* el, const char* name) {
    if (!el || !name || !*name) return ZST_ERROR;
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    pthread_mutex_lock(&srv->lock);

    if (srv->session_count >= RTSP_MAX_SESSIONS) {
        pthread_mutex_unlock(&srv->lock);
        return ZST_ERROR;
    }
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, name) == 0) {
            pthread_mutex_unlock(&srv->lock);
            return ZST_ERROR;
        }
    }

    rtsp_server_session_t* sess = &srv->sessions[srv->session_count];
    memset(sess, 0, sizeof(*sess));
    strncpy(sess->name, name, sizeof(sess->name) - 1);

    char pn[128];
    snprintf(pn, sizeof(pn), "%s_video", name);
    sess->video_pad = zst_pad_create(pn, ZST_PAD_SINK);
    sess->video_pad->push = video_push_cb;

    snprintf(pn, sizeof(pn), "%s_audio", name);
    sess->audio_pad = zst_pad_create(pn, ZST_PAD_SINK);
    sess->audio_pad->push = audio_push_cb;

    if (zst_element_add_pad(el, sess->video_pad) != ZST_OK ||
        zst_element_add_pad(el, sess->audio_pad) != ZST_OK)
    {
        zst_pad_destroy(sess->video_pad);
        zst_pad_destroy(sess->audio_pad);
        pthread_mutex_unlock(&srv->lock);
        return ZST_ERROR;
    }

    sess->has_video   = 1;
    sess->has_audio   = 1;
    sess->video_codec = 1; /* H264 */
    sess->audio_codec = 3; /* AAC */
    srv->session_count++;

    ZST_LOG_INFO("rtsp_server", "session /%s added (pads: %s_video, %s_audio)",
                 name, name, name);

    pthread_mutex_unlock(&srv->lock);
    return ZST_OK;
}

zst_result_t zst_rtsp_server_remove_session(zst_element_t* el, const char* name) {
    if (!el || !name) return ZST_ERROR;
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    pthread_mutex_lock(&srv->lock);
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, name) != 0) continue;
        zst_pad_destroy(srv->sessions[i].video_pad);
        zst_pad_destroy(srv->sessions[i].audio_pad);
        free(srv->sessions[i].extra_data);
        for (int j = i; j < srv->session_count - 1; j++)
            srv->sessions[j] = srv->sessions[j + 1];
        srv->session_count--;
        pthread_mutex_unlock(&srv->lock);
        return ZST_OK;
    }
    pthread_mutex_unlock(&srv->lock);
    return ZST_ERROR;
}

int zst_rtsp_server_session_count(zst_element_t* el) {
    if (!el) return 0;
    rtsp_server_priv_t* srv = el->priv;
    return srv ? srv->session_count : 0;
}



zst_element_t*
zst_rtsp_server_create_with_config(const zst_rtsp_server_config_t* config)
{
    (void)config;
    return zst_element_factory_make("rtsp_server");
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create(const char* name) {
    if (strcmp(name, "rtsp_server") == 0) return zst_rtsp_server_create();
    return NULL;
}

static const zst_property_spec_t g_rtspserver_properties[] = {
    { "listen-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "RTSP server listen port" },
    { "listen_port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "Alias for listen-port" },
    { "session_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of active RTSP streaming sessions" },
    { "client_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of connected RTSP clients" }
};

static const zst_pad_template_t g_rtspserver_pads[] = {
    { "video_%u", ZST_PAD_SINK, "video/x-h264" },
    { "audio_%u", ZST_PAD_SINK, "audio/x-aac" }
};

static const zst_element_desc_t g_rtspserver_elements[] = {
    {
        .name = "rtsp_server",
        .long_name = "RTSP Server",
        .category = "Sink/Network",
        .description = "Serves RTP streams over RTSP",
        .author = "zstreamer",
        .properties = g_rtspserver_properties,
        .nb_properties = sizeof(g_rtspserver_properties) / sizeof(g_rtspserver_properties[0]),
        .pads = g_rtspserver_pads,
        .nb_pads = sizeof(g_rtspserver_pads) / sizeof(g_rtspserver_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name    = "rtsp_server_plugin",
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
        *nb_elements_out = sizeof(g_rtspserver_elements) / sizeof(g_rtspserver_elements[0]);
    }
    return g_rtspserver_elements;
}

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void) {
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
