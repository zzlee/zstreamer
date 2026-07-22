/*=============================================================================
    rtsp_server.c — RTSP server element supporting multiple sessions

    Serves multiple RTSP streams on a single port, each with its own
    mount point (URI path). Each mount point maps to a named pair of
    sink pads (video/audio). Connected clients DESCRIBE/SETUP/PLAY
    and receive RTP over TCP interleaved, UDP unicast, or UDP multicast transport.

    Architecture inspired by ireader/media-server:
      - rtsp_server_listen() → per-client rtsp_server_t
      - URI-based routing in ondescribe/onsetup handlers
      - RTP over RTSP interleaved binary framing ($ + channel + len + data)
      - UDP transport (RFC 3550): RTP on even port, RTCP on next odd port
      - Multicast transport: RTP/AVP;multicast with destination/port/ttl

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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_rtsp_server.h"
#include "zst_timestamp_pacer.h"

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

static void apply_pacing_properties(struct rtsp_server_priv_s* srv, struct rtsp_server_session_s* sess);

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
    int      sps_pps_sent;       /* SPS/PPS have been sent to this client */
    int      interleaved_ch;     /* TCP interleaved channel for this stream */

    /* UDP transport state per stream */
    int                     udp_rtp_fd;     /* server UDP socket for RTP */
    int                     udp_rtcp_fd;    /* server UDP socket for RTCP */
    uint16_t                server_rtp_port;
    uint16_t                server_rtcp_port;
    struct sockaddr_in      client_rtp_addr;
    struct sockaddr_in      client_rtcp_addr;
    uint16_t                client_rtp_port;
    uint16_t                client_rtcp_port;

    /* Multicast transport state (destination group/TTL announced in SETUP) */
    char                    multicast_destination[64];
    int                     multicast_ttl;
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
    /* Cached H.264 SPS/PPS NAL units in Annex-B format (with 0x00000001 start codes) */
    uint8_t*    sps_pps_cache;
    int         sps_pps_cache_size;
    zst_timestamp_pacer_t* video_udp_pacer;
    zst_timestamp_pacer_t* audio_udp_pacer;
} rtsp_server_session_t;

/*===========================================================================
    Server element private data
===========================================================================*/
typedef struct rtsp_server_priv_s {
    zst_element_t*          self;
    int                     listen_port;
    int                     listen_fd;
    int                     running;
    pthread_t               listen_thread;
    pthread_mutex_t         lock;

    rtsp_server_session_t   sessions[RTSP_MAX_SESSIONS];
    int                     session_count;

    rtsp_client_t*          clients;
    int                     client_count;

    zst_rtsp_server_mount_cb_t mount_callback;
    void*                      mount_user_data;

    int                        force_tcp; /* Ignore UDP client_port and use RTP/RTSP interleaved */

    char                       multicast_address[64];
    uint16_t                   multicast_port_base;
    int                        multicast_ttl;

    int                        udp_timestamp_pacing;
    uint64_t                   udp_pacing_tolerance_ms;
    uint64_t                   udp_pacing_reset_threshold_ms;
    uint64_t                   udp_max_lateness_ms;
} rtsp_server_priv_t;


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

static int annexb_cache_contains_nal(const uint8_t* cache, int cache_size,
                                     const uint8_t* nal, int nal_len) {
    if (!cache || cache_size <= 0 || !nal || nal_len <= 0) return 0;
    int i = 0;
    while (i + 4 + nal_len <= cache_size) {
        if (cache[i] == 0 && cache[i + 1] == 0 &&
            cache[i + 2] == 0 && cache[i + 3] == 1) {
            int nal_start = i + 4;
            int nal_end = cache_size;
            for (int j = nal_start; j + 3 < cache_size; j++) {
                if (cache[j] == 0 && cache[j + 1] == 0 &&
                    (cache[j + 2] == 1 ||
                     (j + 4 < cache_size && cache[j + 2] == 0 && cache[j + 3] == 1))) {
                    nal_end = j;
                    break;
                }
            }
            if (nal_end - nal_start == nal_len &&
                memcmp(cache + nal_start, nal, nal_len) == 0) {
                return 1;
            }
            i = nal_end;
        } else {
            i++;
        }
    }
    return 0;
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
                           int is_last,
                           packet_sink_t sink, void* ctx)
{
    if (nal_len < 1) return 0;

    uint8_t nal_type = nal[0] & 0x1f;
    uint8_t nri      = (nal[0] >> 5) & 0x03;
    int     count    = 0;

    if (nal_len <= RTP_MTU - 12) {
        uint8_t pkt[RTP_MTU];
        /* M=1 only for the last NAL of the access unit (RFC 3984) */
        build_rtp_hdr(pkt, st, is_last ? 1 : 0);
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
        /* M=1 only on last fragment AND last NAL of access unit */
        build_rtp_hdr(pkt, st, (rem == chunk && is_last) ? 1 : 0);
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

    if ((cl->transport_type == RTSP_TRANSPORT_UDP ||
         cl->transport_type == RTSP_TRANSPORT_MULTICAST) && st->udp_rtcp_fd >= 0) {
        /* Send RTCP SR via UDP unicast/multicast */
        int n = sendto(st->udp_rtcp_fd, buf, slen, 0,
                       (struct sockaddr*)&st->client_rtcp_addr,
                       sizeof(st->client_rtcp_addr));
        return (n == slen) ? 0 : -1;
    }

    /* TCP interleaved framing */
    uint8_t frame[4 + 28];
    frame[0] = '$';
    frame[1] = (uint8_t)(st->interleaved_ch + 1);
    frame[2] = (uint8_t)((slen >> 8) & 0xff);
    frame[3] = (uint8_t)(slen & 0xff);
    memcpy(frame + 4, buf, slen);

    return (send(cl->fd, frame, 4 + slen, MSG_NOSIGNAL) == 4 + slen) ? 0 : -1;
}

/*===========================================================================
    Base64 encoder (RFC 4648, no line wrapping)
===========================================================================*/
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Returns number of characters written (not including NUL terminator).
   out must be at least ((in_len + 2) / 3) * 4 + 1 bytes. */
static int base64_encode(const uint8_t* in, int in_len, char* out) {
    int i, o = 0;
    for (i = 0; i < in_len; i += 3) {
        uint32_t v  = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i+2];
        out[o++] = B64_CHARS[(v >> 18) & 0x3f];
        out[o++] = B64_CHARS[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < in_len) ? B64_CHARS[(v >> 6) & 0x3f] : '=';
        out[o++] = (i + 2 < in_len) ? B64_CHARS[(v     ) & 0x3f] : '=';
    }
    out[o] = '\0';
    return o;
}

/*===========================================================================
    Extract SDP fmtp parameters from avcC (H.264) extradata
    Fills profile_level_id (6 hex chars + NUL) and sprop (base64 SPS,PPS + NUL).
    Returns 1 on success, 0 if extradata is absent/malformed.
===========================================================================*/
static int avcc_to_sdp_params(const uint8_t* extra, int extra_size,
                               char* profile_level_id,  /* at least 7 bytes */
                               char* sprop,             /* at least 512 bytes */
                               int   sprop_cap)
{
    if (!extra || extra_size < 7 || extra[0] != 1) return 0;

    /* profile_level_id = profile_idc | profile_compatibility | level_idc */
    snprintf(profile_level_id, 7, "%02x%02x%02x",
             extra[1], extra[2], extra[3]);

    /* Walk the avcC NAL unit lists */
    int p = 5;  /* skip configurationVersion, profile/compat/level, lengthSizeMinusOne */
    int num_sps = extra[p++] & 0x1F;

    int sprop_len = 0;
    char tmp[1024];

    for (int i = 0; i < num_sps && p + 2 <= extra_size; i++) {
        int nal_len = (extra[p] << 8) | extra[p+1];
        p += 2;
        if (p + nal_len > extra_size) break;
        if (i > 0 && sprop_len + 1 < sprop_cap) {
            sprop[sprop_len++] = ',';
        }
        int enc_len = base64_encode(extra + p, nal_len, tmp);
        if (sprop_len + enc_len < sprop_cap) {
            memcpy(sprop + sprop_len, tmp, enc_len);
            sprop_len += enc_len;
        }
        p += nal_len;
    }

    if (p >= extra_size) { sprop[sprop_len] = '\0'; return 1; }
    int num_pps = extra[p++];
    for (int i = 0; i < num_pps && p + 2 <= extra_size; i++) {
        int nal_len = (extra[p] << 8) | extra[p+1];
        p += 2;
        if (p + nal_len > extra_size) break;
        if (sprop_len + 1 < sprop_cap) {
            sprop[sprop_len++] = ',';
        }
        int enc_len = base64_encode(extra + p, nal_len, tmp);
        if (sprop_len + enc_len < sprop_cap) {
            memcpy(sprop + sprop_len, tmp, enc_len);
            sprop_len += enc_len;
        }
        p += nal_len;
    }
    sprop[sprop_len] = '\0';
    return 1;
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
        "a=range:npt=0-\r\n"
        "a=sendonly\r\n"
        "a=control:*\r\n",
        (unsigned long long)now, (unsigned long long)now, sess->name);

    if (sess->has_video) {
        int pt = (sess->video_codec == 1) ? RTP_PT_H264 : RTP_PT_H265;
        const char* enc = (sess->video_codec == 1) ? "H264" : "H265";
        n += snprintf(out + n, cap - n,
            "m=video 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d %s/%d\r\n",
            pt, pt, enc, RTP_CLOCK_VIDEO);

        if (sess->video_codec == 1 && sess->extra_data && sess->extra_size > 0) {
            /* H.264: derive profile-level-id and sprop-parameter-sets from avcC */
            char plid[8]  = "42e01f";   /* fallback: Constrained Baseline 3.1 */
            char sprop[1024] = "";
            avcc_to_sdp_params(sess->extra_data, sess->extra_size,
                               plid, sprop, (int)sizeof(sprop));
            if (sprop[0] != '\0') {
                n += snprintf(out + n, cap - n,
                    "a=fmtp:%d packetization-mode=1;"
                    "profile-level-id=%s;"
                    "sprop-parameter-sets=%s\r\n"
                    "a=control:trackID=0\r\n",
                    pt, plid, sprop);
            } else {
                n += snprintf(out + n, cap - n,
                    "a=fmtp:%d packetization-mode=1;profile-level-id=%s\r\n"
                    "a=control:trackID=0\r\n",
                    pt, plid);
            }
        } else {
            /* No extradata — use generic fallback */
            n += snprintf(out + n, cap - n,
                "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f\r\n"
                "a=control:trackID=0\r\n", pt);
        }
    }

    if (sess->has_audio) {
        int sr = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        int ch = sess->channels > 0 ? sess->channels : 2;
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                     24000, 22050, 16000, 12000, 11025, 8000,
                                     7350 };
        int freq_idx = 4; /* 44100 Hz */
        for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++) {
            if (rates[i] == sr) { freq_idx = i; break; }
        }
        if (ch < 1) ch = 1;
        if (ch > 7) ch = 2;
        int object_type = 2; /* AAC LC */
        uint8_t asc0 = (uint8_t)((object_type << 3) | (freq_idx >> 1));
        uint8_t asc1 = (uint8_t)(((freq_idx & 1) << 7) | (ch << 3));
        n += snprintf(out + n, cap - n,
            "m=audio 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d MPEG4-GENERIC/%d/%d\r\n"
            "a=fmtp:%d streamtype=5;profile-level-id=1;"
            "mode=AAC-hbr;config=%02X%02X;"
            "sizelength=13;indexlength=3;indexdeltalength=3\r\n"
            "a=control:trackID=1\r\n",
            RTP_PT_AAC, RTP_PT_AAC, sr, ch, RTP_PT_AAC, asc0, asc1);
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
        "Server: zstreamer/1.0\r\n",
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

    int r = send(cl->fd, reply, n, MSG_NOSIGNAL);
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

static int extract_mount_clean(const char* uri, char* out, int max_len) {
    if (!uri || !out || max_len <= 0) return 0;
    const char* p = strstr(uri, "://");
    if (p) {
        p = strchr(p + 3, '/');
        if (!p) return 0;
    } else {
        p = uri;
    }
    while (*p == '/') p++;

    int len = 0;
    while (p[len] != '\0' && p[len] != '/' && p[len] != '?' && p[len] != ';') {
        len++;
    }
    if (len >= max_len) len = max_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return len > 0;
}

static rtsp_server_session_t* find_session_locked(rtsp_server_priv_t* srv, const char* mount) {
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, mount) == 0) {
            return &srv->sessions[i];
        }
    }
    return NULL;
}

static rtsp_server_session_t* find_or_mount_session(rtsp_client_t* cl, const char* mount) {
    rtsp_server_priv_t* srv = cl->server;
    rtsp_server_session_t* sess = NULL;

    pthread_mutex_lock(&srv->lock);
    sess = find_session_locked(srv, mount);

    if (!sess && srv->mount_callback) {
        zst_rtsp_server_mount_cb_t cb = srv->mount_callback;
        void* ud = srv->mount_user_data;
        zst_element_t* server_el = srv->self;

        pthread_mutex_unlock(&srv->lock);

        ZST_LOG_INFO("rtsp_server", "triggering dynamic mount callback for session /%s", mount);
        zst_result_t res = cb(server_el, mount, ud);

        pthread_mutex_lock(&srv->lock);
        if (res == ZST_OK) {
            sess = find_session_locked(srv, mount);
        }
    }

    pthread_mutex_unlock(&srv->lock);
    return sess;
}


/*===========================================================================
    Transport header parser (RFC 2326 §12.39)

    Parses: Transport: RTP/AVP/TCP;interleaved=0-1
            Transport: RTP/AVP;unicast;client_port=6970-6971
            Transport: RTP/AVP;multicast;destination=224.0.0.1;port=5000-5001;ttl=16

    Returns 0 on success with fields set, -1 on parse error.
===========================================================================*/

static void parse_transport_token(char* tok,
                                  int* transport_type,
                                  uint16_t* client_port1, uint16_t* client_port2,
                                  uint16_t* port1, uint16_t* port2,
                                  int* interleaved1, int* interleaved2,
                                  int* multicast, char* destination, int* ttl)
{
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
    else if (strncasecmp(tok, "client_port=", 12) == 0) {
        if (sscanf(tok + 12, "%hu-%hu", client_port1, client_port2) >= 1) {
            *client_port1 = (*client_port1 / 2) * 2; /* even */
            *client_port2 = *client_port1 + 1;
        }
    }
    else if (strncasecmp(tok, "port=", 5) == 0) {
        if (sscanf(tok + 5, "%hu-%hu", port1, port2) >= 1) {
            *port1 = (*port1 / 2) * 2; /* even */
            *port2 = *port1 + 1;
        }
    }
    else if (strncasecmp(tok, "interleaved=", 12) == 0) {
        if (sscanf(tok + 12, "%d-%d", interleaved1, interleaved2) >= 1) {
            if (*interleaved2 < 0) *interleaved2 = *interleaved1 + 1;
        }
    }
    else if (strncasecmp(tok, "ttl=", 4) == 0) {
        sscanf(tok + 4, "%d", ttl);
    }
    else if (strncasecmp(tok, "destination=", 12) == 0 && destination) {
        strncpy(destination, tok + 12, 64);
        destination[63] = '\0';
    }
}

static int parse_transport_header(const char* field,
                                   int* transport_type,
                                   uint16_t* client_port1, uint16_t* client_port2,
                                   uint16_t* port1, uint16_t* port2,
                                   int* interleaved1, int* interleaved2,
                                   int* multicast, char* destination, int* ttl)
{
    const char* p = field;
    *transport_type = RTSP_TRANSPORT_TCP;
    *client_port1 = *client_port2 = 0;
    *port1 = *port2 = 0;
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
        parse_transport_token(tok, transport_type, client_port1, client_port2,
                              port1, port2, interleaved1, interleaved2,
                              multicast, destination, ttl);
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

static int configure_multicast_sender(int fd, int ttl) {
    if (fd < 0) return -1;
    if (ttl <= 0) ttl = 16;
    if (ttl > 255) ttl = 255;
    unsigned char ttl_uc = (unsigned char)ttl;
    unsigned char loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_uc, sizeof(ttl_uc));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return 0;
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
    static uint16_t next_port = 40000;
    
    for (int i = 0; i < 500; i++) {
        uint16_t rtp_port = next_port;
        next_port += 2;
        if (next_port >= 50000 || next_port < 40000) {
            next_port = 40000;
        }
        
        if (bind_udp_port(*rtp_fd, rtp_port) < 0) {
            close(*rtp_fd);
            *rtp_fd = create_udp_socket();
            if (*rtp_fd < 0) return -1;
            continue;
        }
        
        if (bind_udp_port(*rtcp_fd, rtp_port + 1) < 0) {
            close(*rtp_fd);
            *rtp_fd = create_udp_socket();
            if (*rtp_fd < 0) return -1;
            
            close(*rtcp_fd);
            *rtcp_fd = create_udp_socket();
            if (*rtcp_fd < 0) return -1;
            continue;
        }
        
        *out_rtp_port = rtp_port;
        *out_rtcp_port = rtp_port + 1;
        return 0;
    }
    return -1;
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
    char mount[128];
    if (!extract_mount_clean(cl->uri, mount, sizeof(mount))) {
        return reply_simple(cl, 404);
    }

    rtsp_server_priv_t* srv = cl->server;
    rtsp_server_session_t* sess = find_or_mount_session(cl, mount);

    if (!sess) return reply_simple(cl, 404);

    cl->session = sess;

    char sdp[RTSP_SDP_SIZE];
    int  sdp_len = make_sdp(sess, sdp, sizeof(sdp));

    char base_url[1024];
    if (strstr(cl->uri, "://")) {
        int uri_len = strlen(cl->uri);
        if (uri_len > 0 && cl->uri[uri_len - 1] == '/') {
            snprintf(base_url, sizeof(base_url), "%s", cl->uri);
        } else {
            snprintf(base_url, sizeof(base_url), "%s/", cl->uri);
        }
    } else {
        struct sockaddr_in local_addr;
        socklen_t local_addr_len = sizeof(local_addr);
        char local_ip[64] = "127.0.0.1";
        uint16_t local_port = srv->listen_port;
        if (getsockname(cl->fd, (struct sockaddr*)&local_addr, &local_addr_len) == 0) {
            inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip));
            local_port = ntohs(local_addr.sin_port);
        }
        const char* path = cl->uri;
        while (*path == '/') path++;
        snprintf(base_url, sizeof(base_url), "rtsp://%s:%d/%s/", local_ip, local_port, path);
    }

    char extras[1200];
    snprintf(extras, sizeof(extras),
        "Content-Type: application/sdp\r\n"
        "Content-Base: %s\r\n",
        base_url);

    return send_reply(cl, 200, extras, sdp, sdp_len);
}


static int on_setup(rtsp_client_t* cl) {
    if (!cl->session) {
        char mount[128];
        if (!extract_mount_clean(cl->uri, mount, sizeof(mount))) {
            return reply_simple(cl, 454);
        }
        cl->session = find_or_mount_session(cl, mount);
        if (!cl->session) return reply_simple(cl, 454);
    }

    rtsp_server_session_t* sess = cl->session;
    rtsp_server_priv_t* srv = cl->server;

    /* Determine which track is being set up from the URI */
    const char* track = strstr(cl->uri, "trackID=");
    int is_video_track = 0;
    int is_audio_track = 0;
    if (track) {
        int tid = atoi(track + 8);
        if (tid == 0) is_video_track = 1;
        else if (tid == 1) is_audio_track = 1;
    } else {
        /* No track ID: treat this as an aggregate SETUP request and bind the
           first available media stream.  Generic RTSP clients normally SETUP
           each a=control track URL separately; accepting the aggregate URL
           here keeps clients that send SETUP before DESCRIBE from failing
           with 454 while avoiding an invalid one-Transport/two-stream setup. */
        if (sess->has_video) is_video_track = 1;
        else if (sess->has_audio) is_audio_track = 1;
    }

    /* Parse Transport header */
    int transport_type_parsed = RTSP_TRANSPORT_TCP;
    uint16_t cport1 = 0, cport2 = 0;
    uint16_t port1 = 0, port2 = 0;
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
                                &port1, &port2,
                                &il1, &il2,
                                &multicast, destination, &ttl);
    }

    if (srv->force_tcp) {
        transport_type_parsed = RTSP_TRANSPORT_TCP;
        cport1 = cport2 = 0;
        multicast = 0;
    }

    /* If this is the first SETUP, establish the transport */
    if (cl->track_setup_mask == 0) {
        /* Generate session ID on first SETUP */
        snprintf(cl->session_id, sizeof(cl->session_id), "%08x", secure_rand32());

        if (multicast) {
            /* Client requested RTP/AVP multicast.  The server sends to the
               requested destination/port when supplied, otherwise to its
               configured multicast-address/port-base. */
            cl->transport_type = RTSP_TRANSPORT_MULTICAST;
        } else if (transport_type_parsed == RTSP_TRANSPORT_TCP ||
                   (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 == 0 && has_transport)) {
            /* Client requested TCP interleaved, or UDP without client_port — use TCP */
            cl->transport_type = RTSP_TRANSPORT_TCP;
            cl->interleaved_rtp  = (il1 >= 0) ? il1 : 0;
            cl->interleaved_rtcp = (il2 >= 0) ? il2 : cl->interleaved_rtp + 1;
        } else if (transport_type_parsed == RTSP_TRANSPORT_UDP && cport1 > 0) {
            /* Client requested UDP unicast with client_port */
            cl->transport_type = RTSP_TRANSPORT_UDP;
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
        cl->vstream.udp_rtp_fd   = -1;
        cl->vstream.udp_rtcp_fd  = -1;
        cl->vstream.ssrc         = rand32();
        cl->vstream.seq          = (uint16_t)rand32();
        cl->vstream.timestamp    = rand32();
        cl->vstream.payload_type = (sess->video_codec == 1) ? RTP_PT_H264 : RTP_PT_H265;
        cl->vstream.clock_rate   = RTP_CLOCK_VIDEO;
        cl->vstream.interleaved_ch = cl->interleaved_rtp;  /* video on channel from SETUP */
        cl->vstream.codec        = sess->video_codec;       /* must be LAST — signals delivery ready */
        cl->track_setup_mask |= 1;
    }

    if (is_audio_track && sess->has_audio && !(cl->track_setup_mask & 2)) {
        memset(&cl->astream, 0, sizeof(cl->astream));
        cl->astream.udp_rtp_fd   = -1;
        cl->astream.udp_rtcp_fd  = -1;
        cl->astream.ssrc         = rand32();
        cl->astream.seq          = (uint16_t)rand32();
        cl->astream.timestamp    = rand32();
        cl->astream.payload_type = RTP_PT_AAC;
        cl->astream.clock_rate   = sess->sample_rate > 0 ? sess->sample_rate : 44100;
        /* Audio uses the interleaved channel from the audio SETUP, or video+2 as default */
        cl->astream.interleaved_ch = (il1 >= 0) ? il1 : cl->interleaved_rtp + 2;
        cl->astream.codec        = 3;  /* must be LAST — signals delivery ready */
        cl->track_setup_mask |= 2;
    }

    /* Set up UDP sockets specifically for this track if using UDP transport */
    if (cl->transport_type == RTSP_TRANSPORT_UDP && cport1 > 0) {
        rtp_stream_state_t* st = is_audio_track ? &cl->astream : &cl->vstream;

        /* Create server UDP sockets */
        st->udp_rtp_fd = create_udp_socket();
        st->udp_rtcp_fd = create_udp_socket();
        if (st->udp_rtp_fd < 0 || st->udp_rtcp_fd < 0) {
            if (st->udp_rtp_fd >= 0) close(st->udp_rtp_fd);
            if (st->udp_rtcp_fd >= 0) close(st->udp_rtcp_fd);
            st->udp_rtp_fd = st->udp_rtcp_fd = -1;
            return reply_simple(cl, 500);
        }

        /* Bind to ephemeral ports */
        pthread_mutex_lock(&cl->server->lock);
        int bind_res = bind_udp_pair(&st->udp_rtp_fd, &st->udp_rtcp_fd, &st->server_rtp_port, &st->server_rtcp_port);
        pthread_mutex_unlock(&cl->server->lock);

        if (bind_res < 0) {
            if (st->udp_rtp_fd >= 0) close(st->udp_rtp_fd);
            if (st->udp_rtcp_fd >= 0) close(st->udp_rtcp_fd);
            st->udp_rtp_fd = st->udp_rtcp_fd = -1;
            return reply_simple(cl, 500);
        }

        /* Store client address and ports */
        memset(&st->client_rtp_addr, 0, sizeof(st->client_rtp_addr));
        st->client_rtp_addr.sin_family = AF_INET;
        st->client_rtp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
        st->client_rtp_addr.sin_port = htons(cport1);

        memset(&st->client_rtcp_addr, 0, sizeof(st->client_rtcp_addr));
        st->client_rtcp_addr.sin_family = AF_INET;
        st->client_rtcp_addr.sin_addr.s_addr = inet_addr(cl->peer_ip);
        st->client_rtcp_addr.sin_port = htons(cport2);

        st->client_rtp_port   = cport1;
        st->client_rtcp_port  = cport2;

        ZST_LOG_INFO("rtsp_server", "UDP transport: client %s:%hu-%hu, server :%hu-%hu (track=%s)",
                     cl->peer_ip, cport1, cport2,
                     st->server_rtp_port, st->server_rtcp_port,
                     is_audio_track ? "audio" : "video");
    }

    /* Set up multicast sockets for this track if using multicast transport */
    if (cl->transport_type == RTSP_TRANSPORT_MULTICAST) {
        rtp_stream_state_t* st = is_audio_track ? &cl->astream : &cl->vstream;
        const char* group = destination[0] ? destination : srv->multicast_address;
        int effective_ttl = ttl > 0 ? ttl : srv->multicast_ttl;
        uint16_t base_port = port1 ? port1 : (uint16_t)(srv->multicast_port_base + (is_audio_track ? 2 : 0));
        uint16_t rtcp_port = port2 ? port2 : (uint16_t)(base_port + 1);
        base_port = (uint16_t)((base_port / 2) * 2);
        rtcp_port = (uint16_t)(base_port + 1);

        st->udp_rtp_fd = create_udp_socket();
        st->udp_rtcp_fd = create_udp_socket();
        if (st->udp_rtp_fd < 0 || st->udp_rtcp_fd < 0) {
            if (st->udp_rtp_fd >= 0) close(st->udp_rtp_fd);
            if (st->udp_rtcp_fd >= 0) close(st->udp_rtcp_fd);
            st->udp_rtp_fd = st->udp_rtcp_fd = -1;
            return reply_simple(cl, 500);
        }
        configure_multicast_sender(st->udp_rtp_fd, effective_ttl);
        configure_multicast_sender(st->udp_rtcp_fd, effective_ttl);

        memset(&st->client_rtp_addr, 0, sizeof(st->client_rtp_addr));
        st->client_rtp_addr.sin_family = AF_INET;
        st->client_rtp_addr.sin_addr.s_addr = inet_addr(group);
        st->client_rtp_addr.sin_port = htons(base_port);

        memset(&st->client_rtcp_addr, 0, sizeof(st->client_rtcp_addr));
        st->client_rtcp_addr.sin_family = AF_INET;
        st->client_rtcp_addr.sin_addr.s_addr = inet_addr(group);
        st->client_rtcp_addr.sin_port = htons(rtcp_port);

        st->server_rtp_port = base_port;
        st->server_rtcp_port = rtcp_port;
        st->client_rtp_port = base_port;
        st->client_rtcp_port = rtcp_port;
        strncpy(st->multicast_destination, group, sizeof(st->multicast_destination) - 1);
        st->multicast_destination[sizeof(st->multicast_destination) - 1] = '\0';
        st->multicast_ttl = effective_ttl;

        ZST_LOG_INFO("rtsp_server", "multicast transport: destination %s:%hu-%hu ttl=%d (track=%s)",
                     st->multicast_destination, st->server_rtp_port, st->server_rtcp_port,
                     st->multicast_ttl, is_audio_track ? "audio" : "video");
    }

    /* Build Transport response header */
    char extra[256];
    if (cl->transport_type == RTSP_TRANSPORT_UDP) {
        rtp_stream_state_t* st = is_audio_track ? &cl->astream : &cl->vstream;
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/UDP;unicast;"
            "client_port=%hu-%hu;"
            "server_port=%hu-%hu\r\n",
            st->client_rtp_port, st->client_rtcp_port,
            st->server_rtp_port, st->server_rtcp_port);
    } else if (cl->transport_type == RTSP_TRANSPORT_MULTICAST) {
        rtp_stream_state_t* st = is_audio_track ? &cl->astream : &cl->vstream;
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP;multicast;"
            "destination=%s;port=%hu-%hu;ttl=%d\r\n",
            st->multicast_destination[0] ? st->multicast_destination : srv->multicast_address,
            st->server_rtp_port, st->server_rtcp_port,
            st->multicast_ttl > 0 ? st->multicast_ttl : srv->multicast_ttl);
    } else {
        /* Use per-track interleaved channels */
        int rtp_ch  = is_audio_track ? ((il1 >= 0) ? il1 : cl->interleaved_rtp + 2)
                                      : cl->interleaved_rtp;
        int rtcp_ch = is_audio_track ? ((il2 >= 0) ? il2 : rtp_ch + 1)
                                      : cl->interleaved_rtcp;
        snprintf(extra, sizeof(extra),
            "Transport: RTP/AVP/TCP;interleaved=%d-%d\r\n",
            rtp_ch, rtcp_ch);
    }

    return send_reply(cl, 200, extra, NULL, 0);
}

static int on_play(rtsp_client_t* cl) {
    if (!cl->session_id[0]) return reply_simple(cl, 454);

    /* Build RTP-Info BEFORE allowing data delivery (play_state=1), so that
       seq/rtptime values are captured before any pipeline thread can deliver
       RTP data and increment seq counters. The response MUST be fully sent
       to the socket before play_state=1 to guarantee that ffmpeg receives
       RTP-Info before the first RTP data packet. */
    char extra[512];
    int n = 0;
    n += snprintf(extra + n, sizeof(extra) - n,
        "Range: npt=0.000-\r\n"
        "RTP-Info: ");

    int first = 1;
    int uri_len = strlen(cl->uri);
    const char* slash = (uri_len > 0 && cl->uri[uri_len - 1] == '/') ? "" : "/";

    if (cl->track_setup_mask & 1) {
        if (!first) n += snprintf(extra + n, sizeof(extra) - n, ",");
        n += snprintf(extra + n, sizeof(extra) - n,
            "url=%s%strackID=0;seq=%u;rtptime=%u",
            cl->uri, slash, (unsigned)cl->vstream.seq,
            (unsigned)cl->vstream.timestamp);
        first = 0;
    }
    if (cl->track_setup_mask & 2) {
        if (!first) n += snprintf(extra + n, sizeof(extra) - n, ",");
        n += snprintf(extra + n, sizeof(extra) - n,
            "url=%s%strackID=1;seq=%u;rtptime=%u",
            cl->uri, slash, (unsigned)cl->astream.seq,
            (unsigned)cl->astream.timestamp);
    }
    /* Terminate RTP-Info header line so that Content-Length doesn't get concatenated */
    n += snprintf(extra + n, sizeof(extra) - n, "\r\n");

    /* Send PLAY response before enabling data delivery */
    int ret = send_reply(cl, 200, extra, NULL, 0);

    /* Now safe to allow pipeline threads to deliver RTP data */
    if (cl->session) {
        if (cl->session->video_udp_pacer) zst_timestamp_pacer_reset(cl->session->video_udp_pacer);
        if (cl->session->audio_udp_pacer) zst_timestamp_pacer_reset(cl->session->audio_udp_pacer);
    }
    cl->play_state = 1;

    return ret;
}

static int on_pause(rtsp_client_t* cl) {
    if (!cl->session_id[0]) return reply_simple(cl, 454);
    if (cl->session) {
        if (cl->session->video_udp_pacer) zst_timestamp_pacer_reset(cl->session->video_udp_pacer);
        if (cl->session->audio_udp_pacer) zst_timestamp_pacer_reset(cl->session->audio_udp_pacer);
    }
    cl->play_state = 2;
    return reply_simple(cl, 200);
}

static int on_teardown(rtsp_client_t* cl) {
    if (cl->session) {
        if (cl->session->video_udp_pacer) zst_timestamp_pacer_reset(cl->session->video_udp_pacer);
        if (cl->session->audio_udp_pacer) zst_timestamp_pacer_reset(cl->session->audio_udp_pacer);
    }
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
    RTP packet send — TCP interleaved, UDP unicast, or UDP multicast
===========================================================================*/
static void write_rtp_packet(rtsp_client_t* cl, rtp_stream_state_t* st, const uint8_t* data, int len) {
    if ((cl->transport_type == RTSP_TRANSPORT_UDP ||
         cl->transport_type == RTSP_TRANSPORT_MULTICAST) && st->udp_rtp_fd >= 0) {
        /* Send raw RTP packet via UDP unicast/multicast */
        sendto(st->udp_rtp_fd, data, len, 0,
               (struct sockaddr*)&st->client_rtp_addr,
               sizeof(st->client_rtp_addr));
    } else {
        /* TCP interleaved framing ($ + channel + 2-byte length + data) */
        uint8_t frame[4];
        frame[0] = '$';
        frame[1] = (uint8_t)st->interleaved_ch;
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
    write_rtp_packet(sc->cl, sc->stream, data, len);
    sc->stream->packet_count++;
    sc->stream->octet_count += len - 12;
    return 0;
}

/*===========================================================================
    Send codec data to all clients subscribed to a session
===========================================================================*/
static void session_deliver(rtsp_server_priv_t* srv,
                             int is_video,
                             int is_audio,
                             rtsp_server_session_t* sess,
                             zst_buffer_t* buf)
{
    if (is_video) {
        static int v_cnt = 0;
        if (v_cnt++ % 30 == 0) {
            ZST_LOG_INFO("rtsp_server", "session_deliver: received video packet, size=%d, pts=%lld", (int)buf->memory.size, (long long)buf->pts);
        }
    }
    if (is_audio) {
        static int a_cnt = 0;
        if (a_cnt++ % 100 == 0) {
            ZST_LOG_INFO("rtsp_server", "session_deliver: received audio packet, size=%d, pts=%lld", (int)buf->memory.size, (long long)buf->pts);
        }
    }

    if (buf->flags & ZST_BUFFER_FLAG_EOS) return;

    int need_pacing = 0;
    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next) {
        if (cl->session == sess && cl->play_state == 1) {
            if (cl->transport_type == RTSP_TRANSPORT_UDP ||
                cl->transport_type == RTSP_TRANSPORT_MULTICAST) {
                need_pacing = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&srv->lock);

    if (need_pacing) {
        zst_timestamp_pacer_t* pacer = is_video ? sess->video_udp_pacer : sess->audio_udp_pacer;
        if (pacer) {
            int dropped = 0;
            zst_result_t pacing_res = zst_timestamp_pacer_wait(pacer, srv->self ? srv->self->clock : NULL, buf->pts, &dropped);
            if (pacing_res != ZST_OK && dropped) {
                return;
            }
        }
    }

    pthread_mutex_lock(&srv->lock);

    /*=== Phase 1: Cache SPS/PPS from H.264 video data into the session ===*/
    if (is_video && sess->video_codec == 1) {
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
                if (nal_len > 0) {
                    uint8_t nal_type = d[nal_start] & 0x1f;
                    if ((nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS) &&
                        !annexb_cache_contains_nal(sess->sps_pps_cache,
                                                   sess->sps_pps_cache_size,
                                                   d + nal_start, nal_len)) {
                        /* Cache with 4-byte start code prefix.  Avoid duplicate
                           SPS/PPS entries because encoders may repeat headers on
                           every keyframe; sending a huge parameter-set burst can
                           confuse strict RTSP clients such as VLC/live555. */
                        int full_len = 4 + nal_len;
                        uint8_t* new_cache = (uint8_t*)realloc(
                            sess->sps_pps_cache,
                            sess->sps_pps_cache_size + full_len);
                        if (new_cache) {
                            sess->sps_pps_cache = new_cache;
                            new_cache[sess->sps_pps_cache_size + 0] = 0;
                            new_cache[sess->sps_pps_cache_size + 1] = 0;
                            new_cache[sess->sps_pps_cache_size + 2] = 0;
                            new_cache[sess->sps_pps_cache_size + 3] = 1;
                            memcpy(new_cache + sess->sps_pps_cache_size + 4,
                                   d + nal_start, nal_len);
                            sess->sps_pps_cache_size += full_len;
                        }
                    }
                }
                i = nal_end;
            } else {
                i++;
            }
        }
    }

    /*=== Phase 2: Deliver data to clients ===*/
    struct { uint32_t addr; uint16_t port; } multicast_sent[32];
    int multicast_sent_count = 0;
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next) {
        if (cl->session != sess || cl->play_state != 1) continue;

        rtp_stream_state_t* st = is_video ? &cl->vstream : &cl->astream;
        if (st->codec == 0) continue;
        if (cl->transport_type == RTSP_TRANSPORT_MULTICAST) {
            int already_sent = 0;
            for (int i = 0; i < multicast_sent_count; i++) {
                if (multicast_sent[i].addr == st->client_rtp_addr.sin_addr.s_addr &&
                    multicast_sent[i].port == st->client_rtp_addr.sin_port) {
                    already_sent = 1;
                    break;
                }
            }
            if (already_sent) continue;
        }

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

            /* Prepend cached SPS/PPS if this client hasn't received them yet
               and the current frame doesn't already contain them */
            int has_sps_pps = 0;
            {
                int scan_i = 0;
                while (scan_i < sz) {
                    if (scan_i + 2 < sz && d[scan_i] == 0 && d[scan_i+1] == 0) {
                        int ns, clen;
                        if (scan_i + 3 < sz && d[scan_i+2] == 1) {
                            ns = scan_i + 3; clen = 3;
                        } else if (scan_i + 4 < sz && d[scan_i+2] == 0 && d[scan_i+3] == 1) {
                            ns = scan_i + 4; clen = 4;
                        } else { scan_i++; continue; }
                        scan_i += clen;
                        if (scan_i <= sz) {
                            uint8_t nt = d[ns] & 0x1f;
                            if (nt == H264_NAL_SPS || nt == H264_NAL_PPS) {
                                has_sps_pps = 1;
                            }
                        }
                        /* Skip rest of this NAL */
                        int ne = sz;
                        for (int j = scan_i; j + 3 < sz; j++) {
                            if (d[j] == 0 && d[j+1] == 0 &&
                                (d[j+2] == 1 ||
                                 (j + 4 < sz && d[j+2] == 0 && d[j+3] == 1))) {
                                ne = j; break;
                            }
                        }
                        scan_i = ne;
                    } else {
                        scan_i++;
                    }
                }
            }

            if (!st->sps_pps_sent && !has_sps_pps &&
                sess->sps_pps_cache && sess->sps_pps_cache_size > 0) {
                /* Send cached SPS/PPS as RTP packets */
                const uint8_t* cd = sess->sps_pps_cache;
                int csz = sess->sps_pps_cache_size;
                int ci = 0;
                while (ci < csz) {
                    if (ci + 4 <= csz &&
                        cd[ci] == 0 && cd[ci+1] == 0 &&
                        cd[ci+2] == 0 && cd[ci+3] == 1) {
                        int nal_start = ci + 4;
                        int nal_end = csz;
                        for (int j = nal_start; j + 3 < csz; j++) {
                            if (cd[j] == 0 && cd[j+1] == 0 &&
                                (cd[j+2] == 1 ||
                                 (j + 4 < csz && cd[j+2] == 0 && cd[j+3] == 1))) {
                                nal_end = j; break;
                            }
                        }
                        int nal_len = nal_end - nal_start;
                        if (nal_len > 0)
                            h264_packetize(st, cd + nal_start, nal_len, 0,
                                           packet_send_cb, &sc);
                        ci = nal_end;
                    } else {
                        ci++;
                    }
                }
                st->sps_pps_sent = 1;
            }

            /* Send current frame's NAL units */
            /* Pre-scan to find the last NAL offset for marker bit */
            int last_nal_off = -1;
            {
                int scan_i = 0;
                while (scan_i < sz) {
                    if (scan_i + 2 < sz && d[scan_i] == 0 && d[scan_i+1] == 0) {
                        int ns, clen;
                        if (scan_i + 3 < sz && d[scan_i+2] == 1) {
                            ns = scan_i + 3; clen = 3;
                        } else if (scan_i + 4 < sz && d[scan_i+2] == 0 && d[scan_i+3] == 1) {
                            ns = scan_i + 4; clen = 4;
                        } else { scan_i++; continue; }
                        scan_i += clen;
                        int ne = sz;
                        for (int j = scan_i; j + 3 < sz; j++) {
                            if (d[j] == 0 && d[j+1] == 0 &&
                                (d[j+2] == 1 ||
                                 (j + 4 < sz && d[j+2] == 0 && d[j+3] == 1))) {
                                ne = j; break;
                            }
                        }
                        last_nal_off = ns;
                        scan_i = ne;
                    } else {
                        scan_i++;
                    }
                }
            }

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
                    if (nal_len > 0) {
                        int is_last = (nal_start == last_nal_off);
                        h264_packetize(st, d + nal_start, nal_len, is_last,
                                       packet_send_cb, &sc);
                        /* Mark client as having received SPS/PPS */
                        uint8_t nt = d[nal_start] & 0x1f;
                        if (nt == H264_NAL_SPS || nt == H264_NAL_PPS)
                            st->sps_pps_sent = 1;
                    }
                    i = nal_end;
                } else {
                    i++;
                }
            }
        } else if (is_audio && st->codec == 3) {
            aac_packetize(st, buf->memory.data, (int)buf->memory.size,
                          packet_send_cb, &sc);
        }

        if (cl->transport_type == RTSP_TRANSPORT_MULTICAST && multicast_sent_count < 32) {
            multicast_sent[multicast_sent_count].addr = st->client_rtp_addr.sin_addr.s_addr;
            multicast_sent[multicast_sent_count].port = st->client_rtp_addr.sin_port;
            multicast_sent_count++;
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
            session_deliver(srv, 1, 0, &srv->sessions[i], buf);
            return ZST_OK;
        }
    }
    return ZST_ERROR;
}

static zst_result_t audio_push_cb(zst_pad_t* pad, zst_buffer_t* buf) {
    rtsp_server_priv_t* srv = pad->parent->priv;
    for (int i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i].audio_pad == pad) {
            session_deliver(srv, 0, 1, &srv->sessions[i], buf);
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
    if (cl->vstream.udp_rtp_fd >= 0) {
        close(cl->vstream.udp_rtp_fd);
        cl->vstream.udp_rtp_fd = -1;
    }
    if (cl->vstream.udp_rtcp_fd >= 0) {
        close(cl->vstream.udp_rtcp_fd);
        cl->vstream.udp_rtcp_fd = -1;
    }
    if (cl->astream.udp_rtp_fd >= 0) {
        close(cl->astream.udp_rtp_fd);
        cl->astream.udp_rtp_fd = -1;
    }
    if (cl->astream.udp_rtcp_fd >= 0) {
        close(cl->astream.udp_rtcp_fd);
        cl->astream.udp_rtcp_fd = -1;
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
        cl->vstream.udp_rtp_fd   = -1;
        cl->vstream.udp_rtcp_fd  = -1;
        cl->astream.udp_rtp_fd   = -1;
        cl->astream.udp_rtcp_fd  = -1;
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

    /* Stop the listener and disconnect all clients (idempotent) */
    srv->running = 0;
    if (srv->listen_thread) {
        pthread_join(srv->listen_thread, NULL);
        srv->listen_thread = 0;
    }

    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next)
        shutdown(cl->fd, SHUT_RDWR);
    pthread_mutex_unlock(&srv->lock);

    /* Small delay for threads to notice */
    usleep(100000);

    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }

    /* Free per-session extradata — null pointer after free to prevent double-free
       if el_close is ever called more than once (e.g., via el_stop + el_close). */
    for (int i = 0; i < srv->session_count; i++) {
        free(srv->sessions[i].extra_data);
        srv->sessions[i].extra_data = NULL;
        srv->sessions[i].extra_size = 0;
        free(srv->sessions[i].sps_pps_cache);
        srv->sessions[i].sps_pps_cache = NULL;
        srv->sessions[i].sps_pps_cache_size = 0;
        if (srv->sessions[i].video_udp_pacer) {
            zst_timestamp_pacer_deinit(srv->sessions[i].video_udp_pacer);
            free(srv->sessions[i].video_udp_pacer);
            srv->sessions[i].video_udp_pacer = NULL;
        }
        if (srv->sessions[i].audio_udp_pacer) {
            zst_timestamp_pacer_deinit(srv->sessions[i].audio_udp_pacer);
            free(srv->sessions[i].audio_udp_pacer);
            srv->sessions[i].audio_udp_pacer = NULL;
        }
    }

    return ZST_OK;
}

/* el_stop: called on PLAYING→PAUSED/NULL — stop the network listener and
   disconnect clients, but do NOT free session memory (el_close handles that). */
static zst_result_t el_stop(zst_element_t* el) {
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    srv->running = 0;
    if (srv->listen_thread) {
        pthread_join(srv->listen_thread, NULL);
        srv->listen_thread = 0;
    }

    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next)
        shutdown(cl->fd, SHUT_RDWR);
    pthread_mutex_unlock(&srv->lock);

    usleep(100000);

    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }

    for (int i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i].video_udp_pacer) zst_timestamp_pacer_reset(srv->sessions[i].video_udp_pacer);
        if (srv->sessions[i].audio_udp_pacer) zst_timestamp_pacer_reset(srv->sessions[i].audio_udp_pacer);
    }

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

    for (int i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i].video_udp_pacer) zst_timestamp_pacer_reset(srv->sessions[i].video_udp_pacer);
        if (srv->sessions[i].audio_udp_pacer) zst_timestamp_pacer_reset(srv->sessions[i].audio_udp_pacer);
    }

    srv->running = 1;
    pthread_create(&srv->listen_thread, NULL, listen_thread, srv);

    ZST_LOG_INFO("rtsp_server", "started port=%d sessions=%d",
                 srv->listen_port, srv->session_count);
    return ZST_OK;
}


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
    if (strcmp(name, "force-tcp") == 0 || strcmp(name, "force_tcp") == 0) {
        srv->force_tcp = (strcmp(value, "1") == 0 ||
                          strcasecmp(value, "true") == 0 ||
                          strcasecmp(value, "yes") == 0 ||
                          strcasecmp(value, "on") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "multicast-address") == 0 || strcmp(name, "multicast_address") == 0) {
        strncpy(srv->multicast_address, value, sizeof(srv->multicast_address) - 1);
        srv->multicast_address[sizeof(srv->multicast_address) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "multicast-port-base") == 0 || strcmp(name, "multicast_port_base") == 0) {
        int port = atoi(value);
        if (port <= 0) port = 56000;
        port = (port / 2) * 2;
        srv->multicast_port_base = (uint16_t)port;
        return ZST_OK;
    }
    if (strcmp(name, "multicast-ttl") == 0 || strcmp(name, "multicast_ttl") == 0) {
        srv->multicast_ttl = atoi(value);
        if (srv->multicast_ttl <= 0) srv->multicast_ttl = 16;
        if (srv->multicast_ttl > 255) srv->multicast_ttl = 255;
        return ZST_OK;
    }
    if (strcmp(name, "udp-timestamp-pacing") == 0 || strcmp(name, "udp_timestamp_pacing") == 0) {
        srv->udp_timestamp_pacing = (strcmp(value, "1") == 0 ||
                                     strcasecmp(value, "true") == 0 ||
                                     strcasecmp(value, "yes") == 0 ||
                                     strcasecmp(value, "on") == 0);
        for (int i = 0; i < srv->session_count; i++) {
            apply_pacing_properties(srv, &srv->sessions[i]);
        }
        return ZST_OK;
    }
    if (strcmp(name, "udp-pacing-tolerance-ms") == 0 || strcmp(name, "udp_pacing_tolerance_ms") == 0) {
        srv->udp_pacing_tolerance_ms = strtoull(value, NULL, 10);
        for (int i = 0; i < srv->session_count; i++) {
            apply_pacing_properties(srv, &srv->sessions[i]);
        }
        return ZST_OK;
    }
    if (strcmp(name, "udp-pacing-reset-threshold-ms") == 0 || strcmp(name, "udp_pacing_reset_threshold_ms") == 0) {
        srv->udp_pacing_reset_threshold_ms = strtoull(value, NULL, 10);
        for (int i = 0; i < srv->session_count; i++) {
            apply_pacing_properties(srv, &srv->sessions[i]);
        }
        return ZST_OK;
    }
    if (strcmp(name, "udp-max-lateness-ms") == 0 || strcmp(name, "udp_max_lateness_ms") == 0) {
        srv->udp_max_lateness_ms = strtoull(value, NULL, 10);
        for (int i = 0; i < srv->session_count; i++) {
            apply_pacing_properties(srv, &srv->sessions[i]);
        }
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
    if (strcmp(name, "force-tcp") == 0 || strcmp(name, "force_tcp") == 0) {
        snprintf(out, max, "%d", srv->force_tcp ? 1 : 0);
        return ZST_OK;
    }
    if (strcmp(name, "multicast-address") == 0 || strcmp(name, "multicast_address") == 0) {
        snprintf(out, max, "%s", srv->multicast_address);
        return ZST_OK;
    }
    if (strcmp(name, "multicast-port-base") == 0 || strcmp(name, "multicast_port_base") == 0) {
        snprintf(out, max, "%hu", srv->multicast_port_base);
        return ZST_OK;
    }
    if (strcmp(name, "multicast-ttl") == 0 || strcmp(name, "multicast_ttl") == 0) {
        snprintf(out, max, "%d", srv->multicast_ttl);
        return ZST_OK;
    }
    if (strcmp(name, "udp-timestamp-pacing") == 0 || strcmp(name, "udp_timestamp_pacing") == 0) {
        snprintf(out, max, "%s", srv->udp_timestamp_pacing ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "udp-pacing-tolerance-ms") == 0 || strcmp(name, "udp_pacing_tolerance_ms") == 0) {
        snprintf(out, max, "%llu", (unsigned long long)srv->udp_pacing_tolerance_ms);
        return ZST_OK;
    }
    if (strcmp(name, "udp-pacing-reset-threshold-ms") == 0 || strcmp(name, "udp_pacing_reset_threshold_ms") == 0) {
        snprintf(out, max, "%llu", (unsigned long long)srv->udp_pacing_reset_threshold_ms);
        return ZST_OK;
    }
    if (strcmp(name, "udp-max-lateness-ms") == 0 || strcmp(name, "udp_max_lateness_ms") == 0) {
        snprintf(out, max, "%llu", (unsigned long long)srv->udp_max_lateness_ms);
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

static void apply_pacing_properties(rtsp_server_priv_t* srv, rtsp_server_session_t* sess) {
    if (sess->video_udp_pacer) {
        zst_timestamp_pacer_set_enabled(sess->video_udp_pacer, srv->udp_timestamp_pacing);
        zst_timestamp_pacer_configure(
            sess->video_udp_pacer,
            srv->udp_pacing_tolerance_ms * 1000000ULL,
            srv->udp_pacing_reset_threshold_ms * 1000000ULL,
            srv->udp_max_lateness_ms * 1000000ULL
        );
    }
    if (sess->audio_udp_pacer) {
        zst_timestamp_pacer_set_enabled(sess->audio_udp_pacer, srv->udp_timestamp_pacing);
        zst_timestamp_pacer_configure(
            sess->audio_udp_pacer,
            srv->udp_pacing_tolerance_ms * 1000000ULL,
            srv->udp_pacing_reset_threshold_ms * 1000000ULL,
            srv->udp_max_lateness_ms * 1000000ULL
        );
    }
}

/*===========================================================================
    Public API
===========================================================================*/
zst_element_t* zst_rtsp_server_create(void) {
    rtsp_server_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    priv->listen_port = RTSP_DEFAULT_PORT;
    priv->listen_fd   = -1;
    priv->force_tcp   = 0;
    strncpy(priv->multicast_address, "239.255.42.42", sizeof(priv->multicast_address) - 1);
    priv->multicast_port_base = 56000;
    priv->multicast_ttl = 16;
    priv->udp_timestamp_pacing = 1;
    priv->udp_pacing_tolerance_ms = 5;
    priv->udp_pacing_reset_threshold_ms = 2000;
    priv->udp_max_lateness_ms = 0;
    pthread_mutex_init(&priv->lock, NULL);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) { pthread_mutex_destroy(&priv->lock); free(priv); return NULL; }
    priv->self = el;
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

    sess->video_udp_pacer = calloc(1, sizeof(zst_timestamp_pacer_t));
    sess->audio_udp_pacer = calloc(1, sizeof(zst_timestamp_pacer_t));
    if (sess->video_udp_pacer) zst_timestamp_pacer_init(sess->video_udp_pacer);
    if (sess->audio_udp_pacer) zst_timestamp_pacer_init(sess->audio_udp_pacer);
    apply_pacing_properties(srv, sess);

    if (zst_element_add_pad(el, sess->video_pad) != ZST_OK ||
        zst_element_add_pad(el, sess->audio_pad) != ZST_OK)
    {
        zst_pad_destroy(sess->video_pad);
        zst_pad_destroy(sess->audio_pad);
        if (sess->video_udp_pacer) {
            zst_timestamp_pacer_deinit(sess->video_udp_pacer);
            free(sess->video_udp_pacer);
        }
        if (sess->audio_udp_pacer) {
            zst_timestamp_pacer_deinit(sess->audio_udp_pacer);
            free(sess->audio_udp_pacer);
        }
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

    /* Find session and disconnect all clients attached to it */
    rtsp_server_session_t* target_sess = NULL;
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, name) == 0) {
            target_sess = &srv->sessions[i];
            break;
        }
    }

    if (target_sess) {
        for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next) {
            if (cl->session == target_sess) {
                cl->session = NULL;
                cl->play_state = 0;
                cl->running = 0;
                shutdown(cl->fd, SHUT_RDWR);
            }
        }
    }

    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, name) != 0) continue;
        zst_pad_destroy(srv->sessions[i].video_pad);
        zst_pad_destroy(srv->sessions[i].audio_pad);
        free(srv->sessions[i].extra_data);
        srv->sessions[i].extra_data = NULL;
        srv->sessions[i].extra_size = 0;
        free(srv->sessions[i].sps_pps_cache);
        srv->sessions[i].sps_pps_cache = NULL;
        srv->sessions[i].sps_pps_cache_size = 0;
        if (srv->sessions[i].video_udp_pacer) {
            zst_timestamp_pacer_deinit(srv->sessions[i].video_udp_pacer);
            free(srv->sessions[i].video_udp_pacer);
            srv->sessions[i].video_udp_pacer = NULL;
        }
        if (srv->sessions[i].audio_udp_pacer) {
            zst_timestamp_pacer_deinit(srv->sessions[i].audio_udp_pacer);
            free(srv->sessions[i].audio_udp_pacer);
            srv->sessions[i].audio_udp_pacer = NULL;
        }
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

zst_result_t zst_rtsp_server_set_mount_callback(
    zst_element_t* server,
    zst_rtsp_server_mount_cb_t callback,
    void* user_data)
{
    if (!server) return ZST_ERROR;
    rtsp_server_priv_t* srv = server->priv;
    if (!srv) return ZST_ERROR;

    pthread_mutex_lock(&srv->lock);
    srv->mount_callback = callback;
    srv->mount_user_data = user_data;
    pthread_mutex_unlock(&srv->lock);

    return ZST_OK;
}


zst_result_t zst_rtsp_server_session_set_extradata(
    zst_element_t* el,
    const char* name,
    const uint8_t* data,
    int size)
{
    if (!el || !name || !data || size <= 0) return ZST_ERROR;
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return ZST_ERROR;

    pthread_mutex_lock(&srv->lock);
    for (int i = 0; i < srv->session_count; i++) {
        if (strcmp(srv->sessions[i].name, name) != 0) continue;
        /* Replace existing extradata */
        free(srv->sessions[i].extra_data);
        srv->sessions[i].extra_data = malloc(size);
        if (!srv->sessions[i].extra_data) {
            srv->sessions[i].extra_size = 0;
            pthread_mutex_unlock(&srv->lock);
            return ZST_ERROR;
        }
        memcpy(srv->sessions[i].extra_data, data, size);
        srv->sessions[i].extra_size = size;
        ZST_LOG_INFO("rtsp_server", "session /%s extradata set (%d bytes)", name, size);
        pthread_mutex_unlock(&srv->lock);
        return ZST_OK;
    }
    pthread_mutex_unlock(&srv->lock);
    ZST_LOG_ERROR("rtsp_server", "session /%s not found for set_extradata", name);
    return ZST_ERROR;
}

int zst_rtsp_server_session_client_count(zst_element_t* el, const char* name) {
    if (!el || !name) return 0;
    rtsp_server_priv_t* srv = el->priv;
    if (!srv) return 0;

    int count = 0;
    pthread_mutex_lock(&srv->lock);
    for (rtsp_client_t* cl = srv->clients; cl; cl = cl->next) {
        if (cl->session && strcmp(cl->session->name, name) == 0) {
            count++;
        }
    }
    pthread_mutex_unlock(&srv->lock);
    return count;
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
    { "force-tcp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Force RTP over RTSP/TCP interleaved transport" },
    { "force_tcp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Alias for force-tcp" },
    { "multicast-address", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "239.255.42.42", "Default multicast destination group" },
    { "multicast-port-base", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "56000", "Default multicast RTP port for video; audio uses +2" },
    { "multicast-ttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "Default multicast IP TTL" },
    { "session_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of active RTSP streaming sessions" },
    { "client_count", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE, "0", "Number of connected RTSP clients" },
    { "udp-timestamp-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Pace UDP RTSP output according to buffer timestamps" },
    { "udp_timestamp_pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Alias for udp-timestamp-pacing" },
    { "udp-pacing-tolerance-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Pacing tolerance in milliseconds" },
    { "udp_pacing_tolerance_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5", "Alias for udp-pacing-tolerance-ms" },
    { "udp-pacing-reset-threshold-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Discontinuity reset threshold in milliseconds" },
    { "udp_pacing_reset_threshold_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2000", "Alias for udp-pacing-reset-threshold-ms" },
    { "udp-max-lateness-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Max lateness in milliseconds before packet drop (0=disabled)" },
    { "udp_max_lateness_ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "0", "Alias for udp-max-lateness-ms" }
};

static const zst_pad_template_t g_rtspserver_pads[] = {
    { "video_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "audio_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" }
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
