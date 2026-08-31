/*============================================================================
    test_dante_av_txrx_real.c — Dante A/V TX+RX real-hardware integration test

    Audio loopback (self-contained):
      TX: audiotestsrc → audioresampler → dantedepaudiosink (DEP shm)
      RX: dantedepaudiosrc → audioresampler → fake_sink

    Video TX (DVR-managed):
      videotestsrc → x264enc → dantevideocoordinator → RTP via DVR

    Video RX (DVR-managed, when flow exists):
      dantevideocoordinator(RX) → h264decoder → fake_sink

    Runs indefinitely until Ctrl+C.

    Usage:
      ./test_dante_av_txrx_real [dvr_socket_path] [dep_shm_name]

    Defaults:
      dvr_socket_path = /var/run/dante/dvr
      dep_shm_name    = DanteEP
============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zstreamer/elements/zst_audio_resampler.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"
#include "zstreamer/elements/zst_dante_session.h"
#include "zstreamer/elements/zst_dante_video_coordinator.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_x264_encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── DEP ABI ────────────────────────────────────────────────────────────── */
#define DEP_HEADER_MAGIC UINT32_C(0x50525354)
#define DEP_ENCODING_PCM32 UINT32_C(32)

typedef struct {
    uint32_t magic, object_bytes, metadata_bytes, flags;
    uint32_t tx_offset, rx_offset, timing_offset, reset_serial;
    uint32_t sample_rate, encoding, samples_per_channel, bytes_per_channel;
    uint32_t tx_channels, rx_channels, audio_reserved[2];
    uint32_t epoch_seconds, epoch_samples, samples_per_period, period_alignment;
    uint64_t period_count;
    uint32_t drift_ppb, monotonic_alignment;
    uint64_t monotonic_ns;
} dep_diag_header_t;

/* ── Globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static zst_scheduler_t*      g_scheduler;

static void on_signal(int sig) { (void)sig; g_stop = 1; if (g_scheduler) zst_scheduler_stop(g_scheduler); }
static uint64_t monotonic_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }

/* ── Helpers ───────────────────────────────────────────────────────────── */
static int set_str(zst_element_t* e, const char* n, const char* v) {
    if (zst_element_set_property_string(e, n, v) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%s FAILED on %s\n", n, v, e->ops ? e->ops->name : "?");
    return 0;
}
static int set_uint(zst_element_t* e, const char* n, uint32_t v) {
    if (zst_element_set_property_uint(e, n, v) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%u FAILED on %s\n", n, v, e->ops ? e->ops->name : "?");
    return 0;
}
static const char* state_name(zst_state_t s) {
    switch (s) { case ZST_STATE_NULL: return "NULL"; case ZST_STATE_READY: return "READY";
    case ZST_STATE_PAUSED: return "PAUSED"; case ZST_STATE_PLAYING: return "PLAYING"; default: return "?"; }
}

/* ── Print element properties ──────────────────────────────────────────── */
static void print_dep_ep_status(zst_element_t* el, const char* label) {
    char v[64];
    const char* keys[] = {"active","sample-rate","period-count","overrun-count","underflow-count",NULL};
    printf("  %s:\n", label);
    for (int i = 0; keys[i]; ++i) {
        if (zst_element_get_property(el, keys[i], v, sizeof(v)) == ZST_OK)
            printf("    %-18s = %s\n", keys[i], v);
    }
}

static void print_pipeline_states(zst_pipeline_t* p) {
    printf("  Pipeline states (%u elements):\n", p->nb_elements);
    for (uint32_t i = 0; i < p->nb_elements; ++i) {
        zst_element_t* el = p->elements[i];
        printf("    %-30s = %s\n", el->ops ? el->ops->name : "?",
               state_name((zst_state_t)atomic_load(&el->state)));
    }
}

/* ── Bus events ────────────────────────────────────────────────────────── */
static void drain_bus(zst_bus_t* bus, zst_element_t* coord,
                      int* tx_flows, int* rx_flows, int* errors) {
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 0) == ZST_OK && ev) {
        switch (ev->type) {
        case ZST_EVENT_ERROR:
            fprintf(stderr, "  [BUS ERROR] %s\n", ev->as.error.message);
            if (errors) (*errors)++;
            break;
        case ZST_EVENT_WARNING:
            fprintf(stderr, "  [BUS WARN]  %s\n", ev->as.error.message);
            break;
        case ZST_EVENT_DANTE_FLOW_CREATED:
            if (ev->as.dante_flow.flow.direction == ZST_DANTE_FLOW_TX) {
                if (coord && zst_dante_video_coordinator_apply_flow(coord, &ev->as.dante_flow.flow) == ZST_OK) {
                    printf("  [BUS] TX video flow created (idx=%u, ch=%u, port=%u)\n",
                           ev->as.dante_flow.flow.flow_index, ev->as.dante_flow.flow.channel_index,
                           ev->as.dante_flow.flow.port);
                    if (tx_flows) (*tx_flows)++;
                }
            } else {
                printf("  [BUS] RX video flow created (idx=%u)\n", ev->as.dante_flow.flow.flow_index);
                if (rx_flows) (*rx_flows)++;
            }
            break;
        case ZST_EVENT_DANTE_FLOW_DELETED:
            if (coord) (void)zst_dante_video_coordinator_remove_flow(coord, &ev->as.dante_flow.flow);
            printf("  [BUS] DVR flow deleted (idx=%u)\n", ev->as.dante_flow.flow.flow_index);
            break;
        default: break;
        }
        zst_event_destroy(ev);
    }
}

/* ── Inspect DEP SHM header ────────────────────────────────────────────── */
static void inspect_dep_shm(const char* name) {
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    printf("[SHM] %s: ", path);
    if (stat(path, &st) != 0) { fprintf(stderr, "MISSING\n"); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open failed\n"); return; }
    void* map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return; }
    dep_diag_header_t* h = (dep_diag_header_t*)map;
    printf("magic=0x%08X %s, rate=%u, period=%u, pc=%llu, serial=%u (%s)\n",
           h->magic, h->magic == DEP_HEADER_MAGIC ? "OK" : "BAD",
           h->sample_rate, h->samples_per_period,
           (unsigned long long)h->period_count, h->reset_serial,
           (h->reset_serial & 1) ? "RESETTING" : "stable");
    munmap(map, st.st_size);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Main                                                                    */
/* ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char** argv)
{
    const char* dvr_path = argc > 1 ? argv[1] : "/var/run/dante/dvr";
    const char* dep_name = argc > 2 ? argv[2] : "DanteEP";
    uint32_t sr = 48000, ch = 2, spb = 4096;
    uint32_t vw = 640, vh = 480, vfps = 30;

    printf("=== Dante A/V TX+RX Real-Hardware Test ===\n");
    printf("DVR: %s | DEP: %s\n", dvr_path, dep_name);
    printf("Audio: %uHz %uch S32LE (buf=%u) | Video: %ux%u@%ufps\n", sr, ch, spb, vw, vh, vfps);
    printf("Ctrl+C to stop.\n");
    printf("===========================================\n\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    inspect_dep_shm(dep_name);
    printf("\n");

    /* ── Create elements ─────────────────────────────────────────────── */
    printf("[1] Creating elements...\n");
    zst_pipeline_t* pipe = zst_pipeline_create();

    /* TX audio */
    zst_element_t* tx_audio_src = zst_audio_test_src_create();
    zst_element_t* tx_resampler = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* tx_dep_sink  = zst_dante_dep_audio_sink_create();

    /* RX audio */
    zst_element_t* rx_dep_src   = zst_dante_dep_audio_source_create();
    zst_element_t* rx_resampler = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* rx_sink      = zst_fake_sink_create();

    /* Video TX */
    zst_element_t* session     = zst_dante_session_create(dvr_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();
    zst_element_t* video_src   = zst_video_test_src_create();
    zst_element_t* encoder     = zst_x264_encoder_create();

    zst_clock_t* clock = zst_clock_system_create();

    if (!pipe || !tx_audio_src || !tx_resampler || !tx_dep_sink ||
        !rx_dep_src || !rx_resampler || !rx_sink ||
        !session || !coordinator || !video_src || !encoder || !clock) {
        fprintf(stderr, "[FATAL] element creation failed\n");
        return 1;
    }
    printf("  [OK] 11 elements created.\n");

    /* ── Configure ───────────────────────────────────────────────────── */
    printf("[2] Configuring...\n");

    /* TX audio */
    set_uint(tx_audio_src, "sample-rate",        sr);
    set_uint(tx_audio_src, "channels",           ch);
    set_str(tx_audio_src, "sample-format",       "S32LE");
    set_uint(tx_audio_src, "samples-per-buffer", spb);
    set_str(tx_audio_src, "use-clock",           "true");
    set_str(tx_audio_src, "real-time-pacing",    "true");

    set_str(tx_dep_sink, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(tx_dep_sink, "channels", c); }
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(tx_dep_sink, "expected-sample-rate", r); }
    set_uint(tx_dep_sink, "queue-periods", 256);

    /* RX audio */
    set_str(rx_dep_src, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(rx_dep_src, "channels", c); }
    set_uint(rx_dep_src, "queue-periods", 256);
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(rx_dep_src, "expected-sample-rate", r); }

    /* Video */
    set_uint(session, "tx-video-channels", 1);
    set_uint(session, "rx-video-channels", 0);
    set_uint(video_src, "width",  vw);
    set_uint(video_src, "height", vh);
    set_uint(video_src, "fps",    vfps);
    set_str(video_src, "use-clock",        "true");
    set_str(video_src, "real-time-pacing", "true");
    set_str(encoder, "preset", "ultrafast");
    set_str(encoder, "tune",   "zerolatency");
    { char f[32]; snprintf(f, sizeof(f), "%u/1", vfps); set_str(encoder, "fps", f); }

    zst_element_set_clock(tx_audio_src, clock);
    zst_element_set_clock(video_src, clock);
    printf("  [OK] Configuration done.\n");

    /* ── Add to pipeline ─────────────────────────────────────────────── */
    printf("[3] Adding to pipeline...\n");
    zst_pipeline_add(pipe, coordinator);
    zst_pipeline_add(pipe, video_src);
    zst_pipeline_add(pipe, encoder);
    zst_pipeline_add(pipe, tx_audio_src);
    zst_pipeline_add(pipe, tx_resampler);
    zst_pipeline_add(pipe, tx_dep_sink);
    zst_pipeline_add(pipe, rx_dep_src);
    zst_pipeline_add(pipe, rx_resampler);
    zst_pipeline_add(pipe, rx_sink);
    printf("  [OK] %u elements.\n", pipe->nb_elements);

    /* ── Link TX audio ───────────────────────────────────────────────── */
    printf("[4] Linking TX audio...\n");
    zst_pad_link(zst_element_get_pad(tx_audio_src, "src"),
                 zst_element_get_pad(tx_resampler, "sink"));
    zst_pad_link(zst_element_get_pad(tx_resampler, "src"),
                 zst_element_get_pad(tx_dep_sink, "sink"));
    printf("  [OK] tx_audio_src -> tx_resampler -> tx_dep_sink\n");

    /* ── Link RX audio ───────────────────────────────────────────────── */
    printf("[5] Linking RX audio...\n");
    zst_pad_link(zst_element_get_pad(rx_dep_src, "src"),
                 zst_element_get_pad(rx_resampler, "sink"));
    zst_pad_link(zst_element_get_pad(rx_resampler, "src"),
                 zst_element_get_pad(rx_sink, "sink"));
    printf("  [OK] rx_dep_src -> rx_resampler -> rx_sink\n");

    /* ── Link video TX ───────────────────────────────────────────────── */
    printf("[6] Linking video TX...\n");
    zst_dante_video_coordinator_attach_session(coordinator, session);
    zst_pad_t* tx_pad = zst_dante_video_coordinator_request_tx_input_pad(coordinator, 0);
    if (tx_pad) {
        zst_pad_link(zst_element_get_pad(encoder, "src"), tx_pad);
        printf("  [OK] video_src -> encoder -> coordinator\n");
    } else {
        fprintf(stderr, "  [WARN] TX pad request failed\n");
    }

    /* ── Start pipeline ──────────────────────────────────────────────── */
    printf("[7] Starting pipeline...\n");
    zst_pipeline_set_clock(pipe, clock);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    session->bus = pipe->bus;
    if (zst_element_set_state(session, ZST_STATE_PLAYING) != ZST_OK)
        fprintf(stderr, "  [WARN] DVR session start failed\n");
    else
        printf("  [OK] DVR session PLAYING\n");

    print_pipeline_states(pipe);

    /* ── Scheduler ───────────────────────────────────────────────────── */
    printf("\n[8] Starting scheduler...\n");
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, pipe);
    pthread_t sched_thread;
    pthread_create(&sched_thread, NULL, (void* (*)(void*))zst_scheduler_run, g_scheduler);
    printf("  [OK] Scheduler running.\n\n");

    /* ── Monitoring loop ─────────────────────────────────────────────── */
    uint64_t t0 = monotonic_ms(), last_print = t0;
    uint64_t last_tx_pc = 0, last_rx_pc = 0;
    int tx_flows = 0, rx_flows = 0, bus_errors = 0;

    while (!g_stop) {
        usleep(200000); /* 200ms */

        uint64_t now = monotonic_ms();
        drain_bus(pipe->bus, coordinator, &tx_flows, &rx_flows, &bus_errors);

        if (now - last_print >= 3000) {
            printf("[%lus] tx_flows=%d rx_flows=%d errors=%d\n",
                   (unsigned long)((now - t0) / 1000), tx_flows, rx_flows, bus_errors);

            print_dep_ep_status(tx_dep_sink, "TX dep_sink");

            /* TX period count */
            char v[64];
            if (zst_element_get_property(tx_dep_sink, "period-count", v, sizeof(v)) == ZST_OK) {
                uint64_t pc = strtoull(v, NULL, 10);
                printf("    tx_period_delta: %llu\n", (unsigned long long)(pc - last_tx_pc));
                last_tx_pc = pc;
            }

            print_dep_ep_status(rx_dep_src, "RX dep_src");

            /* RX period count */
            if (zst_element_get_property(rx_dep_src, "period-count", v, sizeof(v)) == ZST_OK) {
                uint64_t pc = strtoull(v, NULL, 10);
                printf("    rx_period_delta: %llu\n", (unsigned long long)(pc - last_rx_pc));
                last_rx_pc = pc;
            }

            printf("\n");
            last_print = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────────────── */
    printf("\n[STOP] Shutting down...\n");
    zst_scheduler_stop(g_scheduler);
    pthread_join(sched_thread, NULL);

    (void)zst_element_set_state(session, ZST_STATE_NULL);
    (void)zst_pipeline_set_state(pipe, ZST_STATE_NULL);

    printf("[STOP] Final status:\n");
    print_dep_ep_status(tx_dep_sink, "TX dep_sink");
    print_dep_ep_status(rx_dep_src, "RX dep_src");
    printf("  tx_flows=%d rx_flows=%d errors=%d\n", tx_flows, rx_flows, bus_errors);

    (void)zst_dante_video_coordinator_attach_session(coordinator, NULL);
    session->bus = NULL;
    zst_scheduler_destroy(g_scheduler); g_scheduler = NULL;
    zst_pipeline_destroy(pipe);
    zst_clock_unref(clock);

    printf("[STOP] Done.\n");
    return 0;
}
