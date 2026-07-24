/*=============================================================================
    demo_sdp_multicast.c — SDP mux/demux RTP multicast demo

    Terminal 1:
      ./demo_sdp_multicast send /tmp/zstreamer-demo.sdp 239.255.42.42 10

    Terminal 2:
      ./demo_sdp_multicast recv /tmp/zstreamer-demo.sdp 239.255.42.42 10

    Sender pipeline (manual demo driver):
      videotestsrc + audiotestsrc -> x264enc + aacenc -> rtppay -> netsink(udp) -> RTP multicast
      sdpmuxer observes encoded packets and writes the SDP file.

    Receiver pipeline:
      UDP multicast sockets -> sdpdemuxer(sdp-file=...) -> fakesink(s)
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_aac_encoder.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_net_sink.h"
#include "zstreamer/elements/zst_rtp_payloader.h"
#include "zstreamer/elements/zst_sdp_demuxer.h"
#include "zstreamer/elements/zst_sdp_muxer.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"
#include "zst_clock.h"

#define DEMO_VIDEO_PORT 5004
#define DEMO_AUDIO_PORT 5006

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t target_ns) {
    while (!g_stop) {
        uint64_t n = now_ns();
        if (n >= target_ns) return;
        uint64_t diff = target_ns - n;
        struct timespec ts;
        ts.tv_sec = (time_t)(diff / 1000000000ULL);
        ts.tv_nsec = (long)(diff % 1000000000ULL);
        nanosleep(&ts, NULL);
    }
}

static int write_sdp(zst_element_t* sdpmux, const char* path) {
    char sdp[4096];
    if (zst_element_get_property(sdpmux, "sdp", sdp, sizeof(sdp)) != ZST_OK) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    fwrite(sdp, 1, strlen(sdp), f);
    fclose(f);
    printf("Wrote SDP to %s:\n%s\n", path, sdp);
    return 0;
}

static int run_sender(const char* sdp_path, const char* group, int seconds) {
    zst_element_t* vsrc = zst_video_test_src_create();
    zst_element_t* asrc = zst_audio_test_src_create();
    zst_element_t* venc = zst_x264_encoder_create();
    zst_element_t* aenc = zst_aac_encoder_create();
    zst_element_t* sdpmux = zst_sdp_muxer_create();
    zst_element_t* vrtp = zst_rtp_payloader_create();
    zst_element_t* artp = zst_rtp_payloader_create();
    zst_element_t* vudp = zst_net_sink_create();
    zst_element_t* audp = zst_net_sink_create();
    if (!vsrc || !asrc || !venc || !aenc || !sdpmux || !vrtp || !artp || !vudp || !audp) {
        fprintf(stderr, "failed to create elements\n");
        return 1;
    }

    zst_clock_t* sys_clock = zst_clock_system_create();
    zst_element_set_clock(vsrc, sys_clock);
    zst_element_set_clock(asrc, sys_clock);

    zst_element_set_property(vsrc, "width", "640");
    zst_element_set_property(vsrc, "height", "360");
    zst_element_set_property(vsrc, "fps", "30");
    zst_element_set_property(vsrc, "use-clock", "true");
    zst_element_set_property(vsrc, "real-time-pacing", "true");
    zst_element_set_property(venc, "fps", "30/1");
    zst_element_set_property(venc, "preset", "ultrafast");
    zst_element_set_property(venc, "tune", "zerolatency");
    zst_element_set_property(asrc, "sample-rate", "48000");
    zst_element_set_property(asrc, "channels", "2");
    zst_element_set_property(asrc, "samples-per-buffer", "1024");
    zst_element_set_property(asrc, "use-clock", "true");
    zst_element_set_property(asrc, "real-time-pacing", "true");
    zst_element_set_property(aenc, "sample-rate", "48000");
    zst_element_set_property(aenc, "channels", "2");
    zst_element_set_property(sdpmux, "address", group);
    zst_element_set_property(sdpmux, "enable-audio", "true");
    zst_element_set_property(sdpmux, "sample-rate", "48000");
    zst_element_set_property(sdpmux, "channels", "2");
    zst_element_set_property(sdpmux, "video-port", "5004");
    zst_element_set_property(sdpmux, "audio-port", "5006");
    zst_element_set_property(sdpmux, "video-payload-type", "96");
    zst_element_set_property(sdpmux, "audio-payload-type", "97");

    zst_element_set_property(vrtp, "codec", "h264");
    zst_element_set_property(vrtp, "payload-type", "96");
    zst_element_set_property(vrtp, "ssrc", "0x53545056");
    zst_element_set_property(artp, "codec", "aac");
    zst_element_set_property(artp, "payload-type", "97");
    zst_element_set_property(artp, "ssrc", "0x53545041");
    zst_element_set_property(artp, "sample-rate", "48000");

    zst_element_set_property(vudp, "protocol", "udp");
    zst_element_set_property(vudp, "host", group);
    zst_element_set_property(vudp, "port", "5004");
    zst_element_set_property(vudp, "timestamp-pacing", "true");
    zst_element_set_property(audp, "protocol", "udp");
    zst_element_set_property(audp, "host", group);
    zst_element_set_property(audp, "port", "5006");
    zst_element_set_property(audp, "timestamp-pacing", "true");

    zst_pad_link(zst_element_get_pad(vrtp, "src"), zst_element_get_pad(vudp, "sink"));
    zst_pad_link(zst_element_get_pad(artp, "src"), zst_element_get_pad(audp, "sink"));

    zst_element_set_state(vsrc, ZST_STATE_PLAYING);
    zst_element_set_state(asrc, ZST_STATE_PLAYING);
    zst_element_set_state(venc, ZST_STATE_PLAYING);
    zst_element_set_state(aenc, ZST_STATE_PLAYING);
    zst_element_set_state(sdpmux, ZST_STATE_PLAYING);
    if (zst_element_set_state(vudp, ZST_STATE_PLAYING) != ZST_OK ||
        zst_element_set_state(audp, ZST_STATE_PLAYING) != ZST_OK ||
        zst_element_set_state(vrtp, ZST_STATE_PLAYING) != ZST_OK ||
        zst_element_set_state(artp, ZST_STATE_PLAYING) != ZST_OK) {
        fprintf(stderr, "failed to start RTP/UDP output for %s\n", group);
        return 1;
    }

    zst_pad_t* sdpmux_video = zst_element_get_pad(sdpmux, "video");
    zst_pad_t* sdpmux_audio = zst_element_get_pad(sdpmux, "audio");
    zst_pad_t* vrtp_sink_pad = zst_element_get_pad(vrtp, "sink");
    zst_pad_t* artp_sink_pad = zst_element_get_pad(artp, "sink");
    int wrote_sdp = 0;
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)seconds * 1000000000ULL;
    uint64_t next_v = start;
    uint64_t next_a = start;
    uint64_t v_period = 1000000000ULL / 30ULL;
    uint64_t a_period = 1024ULL * 1000000000ULL / 48000ULL;
    uint64_t v_pkts = 0, a_pkts = 0;

    printf("Sending RTP multicast to %s video:%d audio:%d for %d sec\n",
           group, DEMO_VIDEO_PORT, DEMO_AUDIO_PORT, seconds);

    while (!g_stop && now_ns() < end) {
        uint64_t n = now_ns();
        if (n >= next_v) {
            zst_buffer_t *raw = NULL, *enc = NULL;
            if (vsrc->ops->process(vsrc, NULL, &raw) == ZST_OK && raw) {
                if (venc->ops->process(venc, raw, &enc) == ZST_OK && enc) {
                    if (sdpmux_video) sdpmux_video->push(sdpmux_video, enc);
                    if (!wrote_sdp) {
                        write_sdp(sdpmux, sdp_path);
                        wrote_sdp = 1;
                    }
                    if (vrtp_sink_pad) vrtp_sink_pad->push(vrtp_sink_pad, enc);
                    v_pkts++;
                    zst_buffer_unref(enc);
                }
                zst_buffer_unref(raw);
            }
            next_v += v_period;
        }
        if (n >= next_a) {
            zst_buffer_t *raw = NULL, *enc = NULL;
            if (asrc->ops->process(asrc, NULL, &raw) == ZST_OK && raw) {
                if (aenc->ops->process(aenc, raw, &enc) == ZST_OK && enc) {
                    if (sdpmux_audio) sdpmux_audio->push(sdpmux_audio, enc);
                    if (!wrote_sdp) {
                        write_sdp(sdpmux, sdp_path);
                        wrote_sdp = 1;
                    }
                    if (artp_sink_pad) artp_sink_pad->push(artp_sink_pad, enc);
                    a_pkts++;
                    zst_buffer_unref(enc);
                }
                zst_buffer_unref(raw);
            }
            next_a += a_period;
        }
        uint64_t next = next_v < next_a ? next_v : next_a;
        sleep_until(next);
    }

    printf("Sender done: video access-units=%llu audio access-units=%llu\n",
           (unsigned long long)v_pkts, (unsigned long long)a_pkts);
    zst_element_set_state(vrtp, ZST_STATE_NULL);
    zst_element_set_state(artp, ZST_STATE_NULL);
    zst_element_set_state(vudp, ZST_STATE_NULL);
    zst_element_set_state(audp, ZST_STATE_NULL);
    zst_element_set_clock(vsrc, NULL);
    zst_element_set_clock(asrc, NULL);
    zst_clock_unref(sys_clock);

    zst_element_destroy(vsrc);
    zst_element_destroy(asrc);
    zst_element_destroy(venc);
    zst_element_destroy(aenc);
    zst_element_destroy(sdpmux);
    zst_element_destroy(vrtp);
    zst_element_destroy(artp);
    zst_element_destroy(vudp);
    zst_element_destroy(audp);
    return 0;
}

static int make_receiver(const char* group, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        close(fd);
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP");
        close(fd);
        return -1;
    }
    return fd;
}

static zst_pad_t* find_src_pad_prefix(zst_element_t* el, const char* prefix) {
    if (!el || !prefix) return NULL;
    size_t n = strlen(prefix);
    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        if (strncmp(el->src_pads[i]->name, prefix, n) == 0) return el->src_pads[i];
    }
    return NULL;
}

static int run_receiver(const char* sdp_path, const char* group, int seconds) {
    zst_element_t* demux = zst_sdp_demuxer_create();
    zst_element_t* vfakesink = zst_fake_sink_create();
    zst_element_t* afakesink = zst_fake_sink_create();
    if (!demux || !vfakesink || !afakesink) return 1;

    zst_element_set_property(demux, "sdp-file", sdp_path);
    zst_element_set_state(demux, ZST_STATE_PLAYING);
    zst_element_set_property(vfakesink, "push-per-second", "true");
    zst_element_set_property(afakesink, "push-per-second", "true");
    zst_element_set_property(vfakesink, "log-period", "1");
    zst_element_set_property(afakesink, "log-period", "1");
    zst_element_set_state(vfakesink, ZST_STATE_PLAYING);
    zst_element_set_state(afakesink, ZST_STATE_PLAYING);

    zst_pad_t* vsrc = find_src_pad_prefix(demux, "video_");
    zst_pad_t* asrc = find_src_pad_prefix(demux, "audio_");
    if (vsrc) zst_pad_link(vsrc, zst_element_get_pad(vfakesink, "sink"));
    if (asrc) zst_pad_link(asrc, zst_element_get_pad(afakesink, "sink"));

    int vfd = make_receiver(group, DEMO_VIDEO_PORT);
    int afd = make_receiver(group, DEMO_AUDIO_PORT);
    if (vfd < 0 || afd < 0) return 1;

    zst_pad_t* sink = zst_element_get_pad(demux, "sink");
    uint64_t start = now_ns();
    uint64_t end = start + (uint64_t)seconds * 1000000000ULL;
    uint64_t last_print = start;
    uint64_t last_v = 0, last_a = 0;
    char val[64];

    printf("Receiving RTP multicast from %s video:%d audio:%d using %s\n",
           group, DEMO_VIDEO_PORT, DEMO_AUDIO_PORT, sdp_path);

    while (!g_stop && now_ns() < end) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(vfd, &rfds);
        FD_SET(afd, &rfds);
        int maxfd = vfd > afd ? vfd : afd;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0) {
            for (int which = 0; which < 2; which++) {
                int fd = which == 0 ? vfd : afd;
                if (!FD_ISSET(fd, &rfds)) continue;
                uint8_t pkt[1600];
                ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
                if (n <= 0) continue;
                zst_buffer_t* b = zst_buffer_create(ZST_BUFFER_USER);
                if (!b) continue;
                uint8_t* copy = malloc((size_t)n);
                if (!copy) {
                    zst_buffer_unref(b);
                    continue;
                }
                memcpy(copy, pkt, (size_t)n);
                b->memory.data = copy;
                b->memory.size = (size_t)n;
                b->memory.priv = copy;
                b->memory.release = free;
                sink->push(sink, b);
                zst_buffer_unref(b);
            }
        }

        uint64_t n = now_ns();
        if (n - last_print >= 1000000000ULL) {
            uint64_t vtotal = 0, atotal = 0;
            if (zst_element_get_property(vfakesink, "total-buffers", val, sizeof(val)) == ZST_OK) vtotal = strtoull(val, NULL, 10);
            if (zst_element_get_property(afakesink, "total-buffers", val, sizeof(val)) == ZST_OK) atotal = strtoull(val, NULL, 10);
            printf("FPS: video=%.1f audio-packets=%.1f totals: video=%llu audio=%llu\n",
                   (double)(vtotal - last_v), (double)(atotal - last_a),
                   (unsigned long long)vtotal, (unsigned long long)atotal);
            last_v = vtotal;
            last_a = atotal;
            last_print = n;
        }
    }

    close(vfd);
    close(afd);
    zst_element_destroy(demux);
    zst_element_destroy(vfakesink);
    zst_element_destroy(afakesink);
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s send [sdp-file] [multicast-group] [seconds]\n"
            "  %s recv [sdp-file] [multicast-group] [seconds]\n"
            "\nDefaults: sdp-file=/tmp/zstreamer-demo.sdp group=239.255.42.42 seconds=10\n",
            argv0, argv0);
}

int main(int argc, char** argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const char* mode = argc > 1 ? argv[1] : NULL;
    const char* sdp = argc > 2 ? argv[2] : "/tmp/zstreamer-demo.sdp";
    const char* group = argc > 3 ? argv[3] : "239.255.42.42";
    int seconds = argc > 4 ? atoi(argv[4]) : 10;
    if (seconds <= 0) seconds = 10;

    if (!mode) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(mode, "send") == 0) return run_sender(sdp, group, seconds);
    if (strcmp(mode, "recv") == 0) return run_receiver(sdp, group, seconds);

    usage(argv[0]);
    return 2;
}
