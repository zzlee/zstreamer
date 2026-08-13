/*=============================================================================
    demo_srt_perf.c — SRT Client & Server Performance Test & Demo

    Demonstrates high-performance end-to-end video/audio streaming over SRT:
      - Video Source: videotestsrc (1920x1080 30FPS, YUV420P)
      - Audio Source: audiotestsrc (1kHz sine tone, 48kHz, 16-bit, 2ch)
      - Video Encoder: x264 (H.264, 4.0 Mbps target, zerolatency)
      - Audio Encoder: AAC (128 Kbps target)
      - Muxer: MPEG-TS (tsmux)
      - Network: SRT (srtsink listener / srtsrc caller)
      - Demuxer: MPEG-TS (tsdemux)
      - Decoders: H.264 decoder (h264dec) + AAC decoder (aacdec)
      - Sinks: fakesink with real-time performance statistics
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_log.h"
#include "zst_clock.h"
#include "zst_queue.h"

#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"
#include "zstreamer/elements/zst_aac_encoder.h"
#include "zstreamer/elements/zst_mpegts_muxer.h"
#include "zstreamer/elements/zst_srt_sink.h"
#include "zstreamer/elements/zst_srt_source.h"
#include "zstreamer/elements/zst_mpegts_demuxer.h"
#include "zstreamer/elements/zst_h264_decoder.h"
#include "zstreamer/elements/zst_aac_decoder.h"
#include "zstreamer/elements/zst_fake_sink.h"

/* ── Element Helper Constructor ──────────────────────────────────────────── */

static zst_element_t* make_el(const char* name, zst_element_t* (*create_fn)(void))
{
    zst_element_t* el = zst_element_factory_make(name);
    if (!el && create_fn) {
        el = create_fn();
    }
    return el;
}

/* ── Mode Selection ──────────────────────────────────────────────────────── */

typedef enum {
    MODE_LOOPBACK,  /* Run Server & Client in one process */
    MODE_SERVER,    /* Run SRT Sender/Server only */
    MODE_CLIENT     /* Run SRT Receiver/Client only */
} app_mode_t;

typedef struct {
    app_mode_t mode;
    char       server_uri[512];
    char       client_uri[512];
    uint32_t   video_width;
    uint32_t   video_height;
    uint32_t   video_fps;
    uint64_t   video_bitrate;
    uint32_t   audio_sample_rate;
    uint32_t   audio_channels;
    uint64_t   audio_bitrate;
    double     audio_frequency;
    int        duration_sec;
} app_config_t;

static volatile bool g_running = true;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = false;
}

/* ── Performance Statistics Tracker ──────────────────────────────────────── */

typedef struct {
    pthread_mutex_t lock;

    /* Video Encoder Stats */
    uint64_t v_enc_src_frames;
    uint64_t v_enc_out_frames;
    uint64_t v_enc_out_bytes;
    uint64_t v_enc_idr_frames;

    /* Audio Encoder Stats */
    uint64_t a_enc_src_buffers;
    uint64_t a_enc_out_packets;
    uint64_t a_enc_out_bytes;

    /* SRT Network Stats */
    uint64_t srt_sent_packets;
    uint64_t srt_sent_bytes;
    uint64_t srt_recv_packets;
    uint64_t srt_recv_bytes;

    /* Video Decoder Stats */
    uint64_t v_dec_out_frames;
    uint64_t v_dec_out_bytes;

    /* Audio Decoder Stats */
    uint64_t a_dec_out_frames;
    uint64_t a_dec_out_bytes;

    /* Latency & PTS Tracking */
    zst_time_t last_v_src_pts;
    double     total_v_e2e_lat_ms;
    uint64_t   v_e2e_samples;
    double     min_v_e2e_lat_ms;
    double     max_v_e2e_lat_ms;

    /* Windowed snapshots for rates */
    uint64_t win_start_us;
    uint64_t win_v_enc_frames;
    uint64_t win_v_enc_bytes;
    uint64_t win_a_enc_bytes;
    uint64_t win_srt_sent_bytes;
    uint64_t win_srt_recv_bytes;
    uint64_t win_v_dec_frames;
    uint64_t win_v_dec_bytes;
    uint64_t win_a_dec_bytes;
    
    uint64_t global_start_us;
} perf_stats_t;

static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void perf_stats_init(perf_stats_t* s)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    s->min_v_e2e_lat_ms = 999999.0;
    s->global_start_us = get_time_us();
    s->win_start_us = s->global_start_us;
}

static void perf_stats_destroy(perf_stats_t* s)
{
    pthread_mutex_destroy(&s->lock);
}

/* ── Probe Callbacks ─────────────────────────────────────────────────────── */

static zst_pad_probe_return_t
probe_v_src_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->v_enc_src_frames++;
    s->last_v_src_pts = buf->pts;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_v_enc_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->v_enc_out_frames++;
    s->win_v_enc_frames++;
    s->v_enc_out_bytes += buf->memory.size;
    s->win_v_enc_bytes += buf->memory.size;
    if (buf->flags & ZST_BUFFER_FLAG_KEYFRAME) {
        s->v_enc_idr_frames++;
    }
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_a_src_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->a_enc_src_buffers++;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_a_enc_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->a_enc_out_packets++;
    s->a_enc_out_bytes += buf->memory.size;
    s->win_a_enc_bytes += buf->memory.size;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_srt_sent_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->srt_sent_packets++;
    s->srt_sent_bytes += buf->memory.size;
    s->win_srt_sent_bytes += buf->memory.size;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_srt_recv_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->srt_recv_packets++;
    s->srt_recv_bytes += buf->memory.size;
    s->win_srt_recv_bytes += buf->memory.size;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_v_dec_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    uint64_t now_us = get_time_us();

    pthread_mutex_lock(&s->lock);
    s->v_dec_out_frames++;
    s->win_v_dec_frames++;
    s->v_dec_out_bytes += buf->memory.size;
    s->win_v_dec_bytes += buf->memory.size;

    if (buf->pts > 0 && s->last_v_src_pts > 0 && buf->pts <= s->last_v_src_pts) {
        double lat_ms = (double)(now_us * 1000ULL - buf->pts) / 1000000.0;
        if (lat_ms > 0 && lat_ms < 10000.0) {
            s->total_v_e2e_lat_ms += lat_ms;
            s->v_e2e_samples++;
            if (lat_ms < s->min_v_e2e_lat_ms) s->min_v_e2e_lat_ms = lat_ms;
            if (lat_ms > s->max_v_e2e_lat_ms) s->max_v_e2e_lat_ms = lat_ms;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_a_dec_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* user_data)
{
    (void)pad; (void)type;
    perf_stats_t* s = user_data;
    if (!buf) return ZST_PAD_PROBE_OK;

    pthread_mutex_lock(&s->lock);
    s->a_dec_out_frames++;
    s->a_dec_out_bytes += buf->memory.size;
    s->win_a_dec_bytes += buf->memory.size;
    pthread_mutex_unlock(&s->lock);
    return ZST_PAD_PROBE_OK;
}

/* ── Statistics Display Function ──────────────────────────────────────────── */

static void perf_stats_report(perf_stats_t* s, const app_config_t* cfg, bool final_report)
{
    uint64_t now_us = get_time_us();

    pthread_mutex_lock(&s->lock);

    double win_sec = (double)(now_us - s->win_start_us) / 1000000.0;
    if (win_sec <= 0.001) win_sec = 0.001;

    double total_sec = (double)(now_us - s->global_start_us) / 1000000.0;
    if (total_sec <= 0.001) total_sec = 0.001;

    /* Rates over current window */
    double v_enc_fps    = (double)s->win_v_enc_frames / win_sec;
    double v_enc_mbps   = ((double)s->win_v_enc_bytes * 8.0) / (win_sec * 1000000.0);
    double a_enc_kbps   = ((double)s->win_a_enc_bytes * 8.0) / (win_sec * 1000.0);
    double srt_tx_mbps  = ((double)s->win_srt_sent_bytes * 8.0) / (win_sec * 1000000.0);
    double srt_rx_mbps  = ((double)s->win_srt_recv_bytes * 8.0) / (win_sec * 1000000.0);
    double v_dec_fps    = (double)s->win_v_dec_frames / win_sec;
    double v_dec_mbps   = ((double)s->win_v_dec_bytes * 8.0) / (win_sec * 1000000.0);

    /* Cumulative Averages */
    double v_enc_mbps_avg = ((double)s->v_enc_out_bytes * 8.0) / (total_sec * 1000000.0);
    double a_enc_kbps_avg = ((double)s->a_enc_out_bytes * 8.0) / (total_sec * 1000.0);
    double v_dec_mbps_avg = ((double)s->v_dec_out_bytes * 8.0) / (total_sec * 1000000.0);

    double avg_lat_ms = (s->v_e2e_samples > 0) ? (s->total_v_e2e_lat_ms / (double)s->v_e2e_samples) : 0.0;
    double min_lat_ms = (s->v_e2e_samples > 0) ? s->min_v_e2e_lat_ms : 0.0;
    double max_lat_ms = (s->v_e2e_samples > 0) ? s->max_v_e2e_lat_ms : 0.0;

    printf("\n");
    if (final_report) {
        printf("╔════════════════════════════════════════════════════════════════════════════════╗\n");
        printf("║                    FINAL SRT PERFORMANCE SUMMARY REPORT                       ║\n");
        printf("╚════════════════════════════════════════════════════════════════════════════════╝\n");
    } else {
        printf("┌────────────────────────────────────────────────────────────────────────────────┐\n");
        printf("│                  zstreamer SRT Performance Statistics (Live)                  │\n");
        printf("└────────────────────────────────────────────────────────────────────────────────┘\n");
    }

    printf("  [Config] Video: %ux%u @ %u FPS | Target Bitrate: %.2f Mbps (x264)\n",
           cfg->video_width, cfg->video_height, cfg->video_fps, (double)cfg->video_bitrate / 1000000.0);
    printf("           Audio: %u Hz, %u ch | Target Bitrate: %.2f Kbps (AAC)\n",
           cfg->audio_sample_rate, cfg->audio_channels, (double)cfg->audio_bitrate / 1000.0);
    printf("           SRT URI: %s\n", cfg->server_uri);
    printf("──────────────────────────────────────────────────────────────────────────────────\n");
    printf("  [Time Elapsed] %6.2f s   |  [Window] %4.2f s\n", total_sec, win_sec);
    printf("──────────────────────────────────────────────────────────────────────────────────\n");

    if (cfg->mode == MODE_LOOPBACK || cfg->mode == MODE_SERVER) {
        printf("  ▶ VIDEO ENCODER  : %5.1f FPS  | Bitrate: %5.2f Mbps (Total: %" PRIu64 " frames, %" PRIu64 " IDRs)\n",
               v_enc_fps, v_enc_mbps, s->v_enc_out_frames, s->v_enc_idr_frames);
        printf("  ▶ AUDIO ENCODER  : %6.1f Kbps | Total Packets: %" PRIu64 "\n",
               a_enc_kbps, s->a_enc_out_packets);
        printf("  ▶ SRT SENDER     : %5.2f Mbps | Sent: %" PRIu64 " pkts (%" PRIu64 " bytes)\n",
               srt_tx_mbps, s->srt_sent_packets, s->srt_sent_bytes);
    }

    if (cfg->mode == MODE_LOOPBACK || cfg->mode == MODE_CLIENT) {
        printf("  ◀ SRT RECEIVER   : %5.2f Mbps | Recv: %" PRIu64 " pkts (%" PRIu64 " bytes)\n",
               srt_rx_mbps, s->srt_recv_packets, s->srt_recv_bytes);
        printf("  ◀ VIDEO DECODER  : %5.1f FPS  | Bitrate: %5.2f Mbps (Total Decoded: %" PRIu64 " frames)\n",
               v_dec_fps, v_dec_mbps, s->v_dec_out_frames);
        printf("  ◀ AUDIO DECODER  : Total Decoded: %" PRIu64 " frames\n", s->a_dec_out_frames);
    }

    if (s->v_e2e_samples > 0) {
        printf("  ★ LATENCY & SYNC : E2E Latency: %5.2f ms (Min: %5.2f ms, Max: %5.2f ms)\n",
               avg_lat_ms, min_lat_ms, max_lat_ms);
    }

    if (final_report) {
        printf("──────────────────────────────────────────────────────────────────────────────────\n");
        printf("  OVERALL AVERAGE METRICS:\n");
        printf("    - Video Enc Bitrate Avg : %5.2f Mbps\n", v_enc_mbps_avg);
        printf("    - Audio Enc Bitrate Avg : %6.1f Kbps\n", a_enc_kbps_avg);
        printf("    - Video Dec Bitrate Avg : %5.2f Mbps\n", v_dec_mbps_avg);
        printf("==================================================================================\n");
    }

    /* Reset window variables */
    s->win_start_us = now_us;
    s->win_v_enc_frames = 0;
    s->win_v_enc_bytes = 0;
    s->win_a_enc_bytes = 0;
    s->win_srt_sent_bytes = 0;
    s->win_srt_recv_bytes = 0;
    s->win_v_dec_frames = 0;
    s->win_v_dec_bytes = 0;
    s->win_a_dec_bytes = 0;

    pthread_mutex_unlock(&s->lock);
}

/* ── Context for Client Dynamic Pad Resolution ───────────────────────────── */

typedef struct {
    zst_pipeline_t* pipe;
    zst_element_t*  h264dec;
    zst_element_t*  aacdec;
    zst_element_t*  v_sink;
    zst_element_t*  a_sink;
    zst_element_t*  q_vdec;
    zst_element_t*  q_adec;
    bool            v_linked;
    bool            a_linked;
} client_context_t;

/* ── Server (Sender) Pipeline Setup ─────────────────────────────────────── */

static zst_pipeline_t*
create_server_pipeline(const app_config_t* cfg, zst_scheduler_t** sched_out, perf_stats_t* stats)
{
    zst_pipeline_t* pipe = zst_pipeline_create();
    if (!pipe) return NULL;

    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    *sched_out = zst_scheduler_create(&sched_cfg);
    if (!*sched_out) {
        zst_pipeline_destroy(pipe);
        return NULL;
    }

    /* 1. Video Source: videotestsrc 1920x1080 30FPS */
    zst_element_t* vsrc = make_el("videotestsrc", zst_video_test_src_create);
    zst_element_set_property_uint(vsrc, "width", cfg->video_width);
    zst_element_set_property_uint(vsrc, "height", cfg->video_height);
    zst_element_set_property_uint(vsrc, "fps", cfg->video_fps);
    zst_element_set_property_string(vsrc, "pattern", "bars");
    zst_element_set_property_string(vsrc, "pixel-format", "YUV420P");
    zst_element_set_property_bool(vsrc, "use-clock", false);

    zst_element_t* q_vsrc = zst_queue_element_create(NULL);

    /* 2. Video Encoder: x264 4Mbps zerolatency */
    zst_element_t* x264enc = make_el("x264enc", zst_x264_encoder_create);
    zst_element_set_property_int(x264enc, "bitrate", (int)cfg->video_bitrate);
    zst_element_set_property_string(x264enc, "preset", "ultrafast");
    zst_element_set_property_string(x264enc, "tune", "zerolatency");
    zst_element_set_property_int(x264enc, "gop-size", (int)cfg->video_fps);

    zst_element_t* q_venc = zst_queue_element_create(NULL);

    /* 3. Audio Source: audiotestsrc 1k tone, 48K, 16bit, 2ch */
    zst_element_t* asrc = make_el("audiotestsrc", zst_audio_test_src_create);
    zst_element_set_property_uint(asrc, "sample-rate", cfg->audio_sample_rate);
    zst_element_set_property_uint(asrc, "channels", cfg->audio_channels);
    zst_element_set_property_string(asrc, "sample-format", "S16LE");
    zst_element_set_property_string(asrc, "wave", "stereo-tone");
    zst_element_set_property_double(asrc, "frequency", cfg->audio_frequency);
    zst_element_set_property_bool(asrc, "use-clock", false);

    zst_element_t* q_asrc = zst_queue_element_create(NULL);

    /* 4. Audio Encoder: AAC 128Kbps */
    zst_element_t* aacenc = make_el("aacenc", zst_aac_encoder_create);
    zst_element_set_property_int(aacenc, "bitrate", (int)cfg->audio_bitrate);
    zst_element_set_property_int(aacenc, "sample-rate", (int)cfg->audio_sample_rate);
    zst_element_set_property_int(aacenc, "channels", (int)cfg->audio_channels);

    zst_element_t* q_aenc = zst_queue_element_create(NULL);

    /* 5. MPEG-TS Muxer: tsmux */
    zst_element_t* tsmux = make_el("tsmux", zst_mpegts_muxer_create);
    zst_element_set_property_uint(tsmux, "width", cfg->video_width);
    zst_element_set_property_uint(tsmux, "height", cfg->video_height);
    zst_element_set_property_uint(tsmux, "fps", cfg->video_fps);
    zst_element_set_property_uint(tsmux, "sample-rate", cfg->audio_sample_rate);
    zst_element_set_property_uint(tsmux, "channels", cfg->audio_channels);

    zst_element_t* q_mux = zst_queue_element_create(NULL);

    /* 6. SRT Sink: srtsink listener */
    zst_element_t* srtsink = make_el("srtsink", zst_srt_sink_create);
    zst_element_set_property_string(srtsink, "uri", cfg->server_uri);

    /* Attach probes for performance tracking */
    zst_pad_t* pad_vsrc_out = zst_element_get_pad(vsrc, "src");
    zst_pad_t* pad_venc_out = zst_element_get_pad(x264enc, "src");
    zst_pad_t* pad_asrc_out = zst_element_get_pad(asrc, "src");
    zst_pad_t* pad_aenc_out = zst_element_get_pad(aacenc, "src");
    zst_pad_t* pad_srtsink_in = zst_element_get_pad(srtsink, "sink");

    if (pad_vsrc_out) zst_pad_add_probe(pad_vsrc_out, ZST_PAD_PROBE_PRE_BUFFER, probe_v_src_cb, stats);
    if (pad_venc_out) zst_pad_add_probe(pad_venc_out, ZST_PAD_PROBE_PRE_BUFFER, probe_v_enc_cb, stats);
    if (pad_asrc_out) zst_pad_add_probe(pad_asrc_out, ZST_PAD_PROBE_PRE_BUFFER, probe_a_src_cb, stats);
    if (pad_aenc_out) zst_pad_add_probe(pad_aenc_out, ZST_PAD_PROBE_PRE_BUFFER, probe_a_enc_cb, stats);
    if (pad_srtsink_in) zst_pad_add_probe(pad_srtsink_in, ZST_PAD_PROBE_PRE_BUFFER, probe_srt_sent_cb, stats);

    /* Add elements to pipeline */
    zst_pipeline_add(pipe, vsrc);
    zst_pipeline_add(pipe, q_vsrc);
    zst_pipeline_add(pipe, x264enc);
    zst_pipeline_add(pipe, q_venc);
    zst_pipeline_add(pipe, asrc);
    zst_pipeline_add(pipe, q_asrc);
    zst_pipeline_add(pipe, aacenc);
    zst_pipeline_add(pipe, q_aenc);
    zst_pipeline_add(pipe, tsmux);
    zst_pipeline_add(pipe, q_mux);
    zst_pipeline_add(pipe, srtsink);

    /* Link Video Path */
    zst_pad_link(zst_element_get_pad(vsrc, "src"), zst_element_get_pad(q_vsrc, "sink"));
    zst_pad_link(zst_element_get_pad(q_vsrc, "src"), zst_element_get_pad(x264enc, "sink"));
    zst_pad_link(zst_element_get_pad(x264enc, "src"), zst_element_get_pad(q_venc, "sink"));
    zst_pad_link(zst_element_get_pad(q_venc, "src"), zst_element_get_pad(tsmux, "video"));

    /* Link Audio Path */
    zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(q_asrc, "sink"));
    zst_pad_link(zst_element_get_pad(q_asrc, "src"), zst_element_get_pad(aacenc, "sink"));
    zst_pad_link(zst_element_get_pad(aacenc, "src"), zst_element_get_pad(q_aenc, "sink"));
    zst_pad_link(zst_element_get_pad(q_aenc, "src"), zst_element_get_pad(tsmux, "audio"));

    /* Link Muxer -> Queue -> SRT Sink */
    zst_pad_link(zst_element_get_pad(tsmux, "src"), zst_element_get_pad(q_mux, "sink"));
    zst_pad_link(zst_element_get_pad(q_mux, "src"), zst_element_get_pad(srtsink, "sink"));

    zst_scheduler_attach(*sched_out, pipe);
    return pipe;
}

/* ── Client (Receiver) Pipeline Setup ─────────────────────────────────────── */

static zst_pipeline_t*
create_client_pipeline(const app_config_t* cfg, zst_scheduler_t** sched_out, perf_stats_t* stats, client_context_t* ctx)
{
    zst_pipeline_t* pipe = zst_pipeline_create();
    if (!pipe) return NULL;

    zst_scheduler_config_t sched_cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    *sched_out = zst_scheduler_create(&sched_cfg);
    if (!*sched_out) {
        zst_pipeline_destroy(pipe);
        return NULL;
    }

    /* 1. SRT Source: srtsrc caller */
    zst_element_t* srtsrc = make_el("srtsrc", zst_srt_source_create);
    zst_element_set_property_string(srtsrc, "uri", cfg->client_uri);

    zst_element_t* q_srtrx = zst_queue_element_create(NULL);

    /* 2. MPEG-TS Demuxer: tsdemux (dynamic pads: video_0, audio_0) */
    zst_element_t* tsdemux = make_el("tsdemux", zst_mpegts_demuxer_create);

    /* 3. Video Decoding Branch */
    zst_element_t* q_vdec = zst_queue_element_create(NULL);
    zst_element_t* h264dec = make_el("h264dec", zst_h264_decoder_create);
    zst_element_t* q_vdout = zst_queue_element_create(NULL);
    zst_element_t* v_sink  = make_el("fakesink", zst_fake_sink_create);

    /* 4. Audio Decoding Branch */
    zst_element_t* q_adec = zst_queue_element_create(NULL);
    zst_element_t* aacdec = make_el("aacdec", zst_aac_decoder_create);
    zst_element_t* q_adout = zst_queue_element_create(NULL);
    zst_element_t* a_sink  = make_el("fakesink", zst_fake_sink_create);

    /* Attach probes to monitor receiving & decoding */
    zst_pad_t* pad_srtsrc_out = zst_element_get_pad(srtsrc, "src");
    zst_pad_t* pad_h264dec_out = zst_element_get_pad(h264dec, "src");
    zst_pad_t* pad_aacdec_out  = zst_element_get_pad(aacdec, "src");

    if (pad_srtsrc_out) zst_pad_add_probe(pad_srtsrc_out, ZST_PAD_PROBE_PRE_BUFFER, probe_srt_recv_cb, stats);
    if (pad_h264dec_out) zst_pad_add_probe(pad_h264dec_out, ZST_PAD_PROBE_PRE_BUFFER, probe_v_dec_cb, stats);
    if (pad_aacdec_out) zst_pad_add_probe(pad_aacdec_out, ZST_PAD_PROBE_PRE_BUFFER, probe_a_dec_cb, stats);

    /* Add elements to pipeline */
    zst_pipeline_add(pipe, srtsrc);
    zst_pipeline_add(pipe, q_srtrx);
    zst_pipeline_add(pipe, tsdemux);
    zst_pipeline_add(pipe, q_vdec);
    zst_pipeline_add(pipe, h264dec);
    zst_pipeline_add(pipe, q_vdout);
    zst_pipeline_add(pipe, v_sink);
    zst_pipeline_add(pipe, q_adec);
    zst_pipeline_add(pipe, aacdec);
    zst_pipeline_add(pipe, q_adout);
    zst_pipeline_add(pipe, a_sink);

    /* Link static stages */
    zst_pad_link(zst_element_get_pad(srtsrc, "src"), zst_element_get_pad(q_srtrx, "sink"));
    zst_pad_link(zst_element_get_pad(q_srtrx, "src"), zst_element_get_pad(tsdemux, "sink"));

    /* Link decoder output chains */
    zst_pad_link(zst_element_get_pad(h264dec, "src"), zst_element_get_pad(q_vdout, "sink"));
    zst_pad_link(zst_element_get_pad(q_vdout, "src"), zst_element_get_pad(v_sink, "sink"));

    zst_pad_link(zst_element_get_pad(aacdec, "src"), zst_element_get_pad(q_adout, "sink"));
    zst_pad_link(zst_element_get_pad(q_adout, "src"), zst_element_get_pad(a_sink, "sink"));

    ctx->pipe = pipe;
    ctx->h264dec = h264dec;
    ctx->aacdec = aacdec;
    ctx->v_sink = v_sink;
    ctx->a_sink = a_sink;
    ctx->q_vdec = q_vdec;
    ctx->q_adec = q_adec;
    ctx->v_linked = false;
    ctx->a_linked = false;

    zst_scheduler_attach(*sched_out, pipe);
    return pipe;
}

/* ── Process Client Pipeline Bus Events (Dynamic Pads Link) ─────────────── */

static void handle_client_bus_events(zst_pipeline_t* pipe, client_context_t* ctx)
{
    if (!pipe || !ctx) return;
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    if (!bus) return;

    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK && ev) {
        if (ev->type == ZST_EVENT_PAD_ADDED) {
            zst_pad_t* new_pad = ev->as.pad_added.pad;
            if (new_pad && new_pad->name) {
                if (strcmp(new_pad->name, "video_0") == 0 && !ctx->v_linked) {
                    ZST_LOG_INFO("demo", "Discovered dynamic stream video_0 -> linking to H.264 decoder");
                    zst_pipeline_reconfigure_begin(pipe);
                    zst_pipeline_link_pads_dynamic(pipe, new_pad, zst_element_get_pad(ctx->q_vdec, "sink"));
                    zst_pad_link(zst_element_get_pad(ctx->q_vdec, "src"), zst_element_get_pad(ctx->h264dec, "sink"));
                    zst_pipeline_reconfigure_end(pipe);
                    ctx->v_linked = true;
                } else if (strcmp(new_pad->name, "audio_0") == 0 && !ctx->a_linked) {
                    ZST_LOG_INFO("demo", "Discovered dynamic stream audio_0 -> linking to AAC decoder");
                    zst_pipeline_reconfigure_begin(pipe);
                    zst_pipeline_link_pads_dynamic(pipe, new_pad, zst_element_get_pad(ctx->q_adec, "sink"));
                    zst_pad_link(zst_element_get_pad(ctx->q_adec, "src"), zst_element_get_pad(ctx->aacdec, "sink"));
                    zst_pipeline_reconfigure_end(pipe);
                    ctx->a_linked = true;
                }
            }
        } else if (ev->type == ZST_EVENT_ERROR) {
            ZST_LOG_ERROR("demo", "Client Bus Error: %s", ev->as.error.message);
        }
        zst_event_destroy(ev);
        ev = NULL;
    }
}

/* ── CLI Usage & Help ────────────────────────────────────────────────────── */

static void print_usage(const char* prog_name)
{
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -m, --mode <loopback|server|client>  Run mode (default: loopback)\n");
    printf("  -u, --uri <srt_uri>                  SRT URI (default: srt://127.0.0.1:9854)\n");
    printf("  -W, --width <pixels>                 Video resolution width (default: 1920)\n");
    printf("  -H, --height <pixels>                Video resolution height (default: 1080)\n");
    printf("  -f, --fps <rate>                     Video framerate (default: 30)\n");
    printf("  -b, --video-bitrate <bps>            x264 target bitrate (default: 4000000 = 4Mbps)\n");
    printf("  -r, --audio-rate <Hz>                Audio sample rate (default: 48000)\n");
    printf("  -c, --audio-channels <num>           Audio channels (default: 2)\n");
    printf("  -a, --audio-bitrate <bps>            AAC target bitrate (default: 128000 = 128Kbps)\n");
    printf("  -d, --duration <seconds>             Run duration in seconds (0=infinite, default: 10)\n");
    printf("  -h, --help                           Display this help message\n");
}

/* ── Main Entry Point ────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    zst_plugin_registry_init();
    zst_plugin_registry_scan_env();

    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = MODE_LOOPBACK;
    snprintf(cfg.server_uri, sizeof(cfg.server_uri), "srt://127.0.0.1:9854?mode=listener&latency=100");
    snprintf(cfg.client_uri, sizeof(cfg.client_uri), "srt://127.0.0.1:9854?mode=caller&latency=100");
    cfg.video_width = 1920;
    cfg.video_height = 1080;
    cfg.video_fps = 30;
    cfg.video_bitrate = 4000000;    /* 4 Mbps */
    cfg.audio_sample_rate = 48000;
    cfg.audio_channels = 2;
    cfg.audio_bitrate = 128000;     /* 128 Kbps */
    cfg.audio_frequency = 1000.0;   /* 1 kHz tone */
    cfg.duration_sec = 10;          /* Run 10s by default */

    static struct option long_options[] = {
        {"mode",          required_argument, 0, 'm'},
        {"uri",           required_argument, 0, 'u'},
        {"width",         required_argument, 0, 'W'},
        {"height",        required_argument, 0, 'H'},
        {"fps",           required_argument, 0, 'f'},
        {"video-bitrate", required_argument, 0, 'b'},
        {"audio-rate",    required_argument, 0, 'r'},
        {"audio-channels",required_argument, 0, 'c'},
        {"audio-bitrate", required_argument, 0, 'a'},
        {"duration",      required_argument, 0, 'd'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "m:u:W:H:f:b:r:c:a:d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "server") == 0) cfg.mode = MODE_SERVER;
            else if (strcmp(optarg, "client") == 0) cfg.mode = MODE_CLIENT;
            else cfg.mode = MODE_LOOPBACK;
            break;
        case 'u':
            if (strstr(optarg, "mode=")) {
                snprintf(cfg.server_uri, sizeof(cfg.server_uri), "%s", optarg);
                snprintf(cfg.client_uri, sizeof(cfg.client_uri), "%s", optarg);
            } else {
                snprintf(cfg.server_uri, sizeof(cfg.server_uri), "%s?mode=listener&latency=100", optarg);
                snprintf(cfg.client_uri, sizeof(cfg.client_uri), "%s?mode=caller&latency=100", optarg);
            }
            break;
        case 'W': cfg.video_width = (uint32_t)atoi(optarg); break;
        case 'H': cfg.video_height = (uint32_t)atoi(optarg); break;
        case 'f': cfg.video_fps = (uint32_t)atoi(optarg); break;
        case 'b': cfg.video_bitrate = (uint64_t)atoll(optarg); break;
        case 'r': cfg.audio_sample_rate = (uint32_t)atoi(optarg); break;
        case 'c': cfg.audio_channels = (uint32_t)atoi(optarg); break;
        case 'a': cfg.audio_bitrate = (uint64_t)atoll(optarg); break;
        case 'd': cfg.duration_sec = atoi(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    zst_log_set_level(ZST_LOG_LEVEL_INFO);

    printf("================================================================================\n");
    printf("        zstreamer SRT Client & Server Streaming Performance Benchmark\n");
    printf("================================================================================\n");
    printf(" Mode         : %s\n", (cfg.mode == MODE_LOOPBACK) ? "Integrated Loopback (Server + Client)" :
                                    (cfg.mode == MODE_SERVER)   ? "SRT Server (Sender Only)" : "SRT Client (Receiver Only)");
    printf(" Video Source : videotestsrc (%ux%u, %u FPS, pattern=bars, YUV420P)\n",
           cfg.video_width, cfg.video_height, cfg.video_fps);
    printf(" Video Encoder: x264 (Bitrate: %" PRIu64 " bps / %.2f Mbps, preset=ultrafast, zerolatency)\n",
           cfg.video_bitrate, (double)cfg.video_bitrate / 1000000.0);
    printf(" Audio Source : audiotestsrc (1kHz tone, %u Hz, 16-bit, %u ch)\n",
           cfg.audio_sample_rate, cfg.audio_channels);
    printf(" Audio Encoder: AAC (Bitrate: %" PRIu64 " bps / %.2f Kbps)\n",
           cfg.audio_bitrate, (double)cfg.audio_bitrate / 1000.0);
    printf(" SRT URI      : %s\n", cfg.server_uri);
    printf(" Target Time  : %d seconds\n", cfg.duration_sec);
    printf("================================================================================\n\n");

    perf_stats_t stats;
    perf_stats_init(&stats);

    zst_pipeline_t* server_pipe = NULL;
    zst_scheduler_t* server_sched = NULL;

    zst_pipeline_t* client_pipe = NULL;
    zst_scheduler_t* client_sched = NULL;
    client_context_t client_ctx;
    memset(&client_ctx, 0, sizeof(client_ctx));

    /* 1. Build and Start Server Pipeline */
    if (cfg.mode == MODE_LOOPBACK || cfg.mode == MODE_SERVER) {
        ZST_LOG_INFO("demo", "Building SRT Server (Sender) Pipeline...");
        server_pipe = create_server_pipeline(&cfg, &server_sched, &stats);
        if (!server_pipe || !server_sched) {
            ZST_LOG_ERROR("demo", "Failed to create server pipeline!");
            return 1;
        }

        ZST_LOG_INFO("demo", "Starting SRT Server Pipeline...");
        if (zst_pipeline_set_state(server_pipe, ZST_STATE_PLAYING) != ZST_OK ||
            zst_scheduler_run(server_sched) != ZST_OK) {
            ZST_LOG_ERROR("demo", "Failed to start server scheduler!");
            return 1;
        }
        /* Short pause to allow SRT listener socket binding */
        usleep(150000);
    }

    /* 2. Build and Start Client Pipeline */
    if (cfg.mode == MODE_LOOPBACK || cfg.mode == MODE_CLIENT) {
        ZST_LOG_INFO("demo", "Building SRT Client (Receiver) Pipeline...");
        client_pipe = create_client_pipeline(&cfg, &client_sched, &stats, &client_ctx);
        if (!client_pipe || !client_sched) {
            ZST_LOG_ERROR("demo", "Failed to create client pipeline!");
            return 1;
        }

        ZST_LOG_INFO("demo", "Starting SRT Client Pipeline...");
        if (zst_pipeline_set_state(client_pipe, ZST_STATE_PLAYING) != ZST_OK ||
            zst_scheduler_run(client_sched) != ZST_OK) {
            ZST_LOG_ERROR("demo", "Failed to start client scheduler!");
            return 1;
        }
    }

    /* 3. Main Streaming Loop & Performance Reporting */
    uint64_t start_us = get_time_us();
    uint64_t report_interval_us = 1000000ULL; /* 1.0 second */
    uint64_t last_report_us = start_us;

    ZST_LOG_INFO("demo", "Streaming started. Measuring performance...");

    while (g_running) {
        usleep(50000); /* 50ms sleep */
        uint64_t now_us = get_time_us();

        /* Process client dynamic pad addition events */
        if (client_pipe) {
            handle_client_bus_events(client_pipe, &client_ctx);
        }

        /* Periodic statistics report every 1s */
        if (now_us - last_report_us >= report_interval_us) {
            perf_stats_report(&stats, &cfg, false);
            last_report_us = now_us;
        }

        /* Check duration limit */
        if (cfg.duration_sec > 0) {
            if ((now_us - start_us) >= (uint64_t)cfg.duration_sec * 1000000ULL) {
                ZST_LOG_INFO("demo", "Reached requested duration of %d seconds. Stopping...", cfg.duration_sec);
                break;
            }
        }
    }

    /* 4. Final Performance Report & Clean Shutdown */
    perf_stats_report(&stats, &cfg, true);

    ZST_LOG_INFO("demo", "Tearing down pipelines...");

    if (client_sched) {
        zst_scheduler_stop(client_sched);
        zst_pipeline_set_state(client_pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(client_sched);
        zst_pipeline_destroy(client_pipe);
    }

    if (server_sched) {
        zst_scheduler_stop(server_sched);
        zst_pipeline_set_state(server_pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(server_sched);
        zst_pipeline_destroy(server_pipe);
    }

    perf_stats_destroy(&stats);
    zst_plugin_registry_deinit();

    printf("\nSRT Performance Benchmark completed successfully.\n");
    return 0;
}
