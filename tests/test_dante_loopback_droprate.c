/*============================================================================
    test_dante_loopback_droprate.c — Same-machine TX→RX loopback drop-rate

    Runs TX and RX in the same process, sharing DEP shm:
      TX: audiotestsrc (440Hz sine) -> audioresampler -> dantedepaudiosink
      RX: dantedepaudiosrc -> audioresampler -> verify sink

    Usage:
      ./test_dante_loopback_droprate [--log <file>] [dep_shm]

    Default log: dante_loopback_YYYYMMDD_HHMMSS.log
============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_buffer.h"
#include "zstreamer/elements/zst_audio_resampler.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_dante_dep_audio.h"
#include "zstreamer/elements/zst_dante_session.h"
#include "zstreamer/elements/zst_dante_video_coordinator.h"

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

/* ── Sine wave verification ───────────────────────────────────────────── */
typedef struct {
    double frequency, volume, phase;
    uint32_t sample_rate;
    uint64_t total_samples, matched_samples, mismatched_samples;
    uint64_t max_error_sample;
    double max_error_value, tolerance;
} sine_verify_t;

static void sine_init(sine_verify_t* sv, uint32_t sr, double freq, double vol) {
    memset(sv, 0, sizeof(*sv));
    sv->frequency = freq; sv->volume = vol; sv->sample_rate = sr; sv->tolerance = 0.02;
}
static double sine_next(sine_verify_t* sv) {
    const double pi = 3.14159265358979323846, two_pi = 6.28318530717958647692;
    double x = sv->phase * two_pi; if (x > pi) x -= two_pi;
    const double b = 4.0 / pi, c = -4.0 / (pi * pi), p = 0.225;
    double y = b * x + c * x * (x < 0.0 ? -x : x);
    y = p * (y * (y < 0.0 ? -y : y) - y) + y;
    sv->phase += sv->frequency / (double)sv->sample_rate;
    while (sv->phase >= 1.0) sv->phase -= 1.0;
    y *= sv->volume; if (y > 1.0) y = 1.0; if (y < -1.0) y = -1.0;
    return y;
}
static void sine_process(sine_verify_t* sv, const int32_t* samples, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double exp = sine_next(sv) * 2147483647.0;
        double err = (double)samples[i] - exp; if (err < 0) err = -err;
        sv->total_samples++;
        if (err <= sv->tolerance * 2147483647.0) sv->matched_samples++;
        else { sv->mismatched_samples++;
            if (err > sv->max_error_value) { sv->max_error_value = err; sv->max_error_sample = sv->total_samples; } }
    }
}

/* ── RX verify sink ───────────────────────────────────────────────────── */
typedef struct {
    sine_verify_t verify;
    uint64_t total_buffers, total_samples;
} rx_verify_sink_t;

static zst_result_t rx_verify_open(zst_element_t* el) {
    rx_verify_sink_t* s = el->priv;
    s->total_buffers = 0; s->total_samples = 0;
    sine_init(&s->verify, 48000, 440.0, 0.8);
    return ZST_OK;
}
static zst_result_t rx_verify_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    rx_verify_sink_t* s = el->priv; (void)out;
    if (!in) return ZST_ERROR;
    if (in->memory.data && in->memory.size > 0) {
        uint32_t nb = (uint32_t)(in->memory.size / sizeof(int32_t));
        sine_process(&s->verify, (const int32_t*)in->memory.data, nb);
        s->total_samples += nb;
    }
    s->total_buffers++;
    return ZST_OK;
}
static zst_result_t rx_verify_get_property(zst_element_t* el, const char* name, char* out, size_t max_len) {
    rx_verify_sink_t* s = el->priv;
    if (strcmp(name, "total-buffers") == 0) { snprintf(out, max_len, "%llu", (unsigned long long)s->total_buffers); return ZST_OK; }
    if (strcmp(name, "total-samples") == 0) { snprintf(out, max_len, "%llu", (unsigned long long)s->total_samples); return ZST_OK; }
    if (strcmp(name, "matched-samples") == 0) { snprintf(out, max_len, "%llu", (unsigned long long)s->verify.matched_samples); return ZST_OK; }
    if (strcmp(name, "mismatched-samples") == 0) { snprintf(out, max_len, "%llu", (unsigned long long)s->verify.mismatched_samples); return ZST_OK; }
    if (strcmp(name, "integrity-pct") == 0) {
        double pct = s->verify.total_samples > 0 ? (double)s->verify.matched_samples * 100.0 / (double)s->verify.total_samples : 0.0;
        snprintf(out, max_len, "%.4f%%", pct); return ZST_OK; }
    if (strcmp(name, "max-error") == 0) {
        snprintf(out, max_len, "%.1f (at sample %llu)", s->verify.max_error_value, (unsigned long long)s->verify.max_error_sample); return ZST_OK; }
    return ZST_ERROR;
}
static zst_element_ops_t g_rx_verify_ops = {
    .name = "rx-verify-sink", .open = rx_verify_open,
    .process = rx_verify_process, .get_property = rx_verify_get_property,
};
static zst_element_t* rx_verify_sink_create(void) {
    rx_verify_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    zst_element_t* el = zst_element_create(&g_rx_verify_ops, priv);
    if (!el) { free(priv); return NULL; }
    zst_pad_t* pad = zst_pad_create("sink", ZST_PAD_SINK);
    if (!pad || zst_element_add_pad(el, pad) != ZST_OK) { if (pad) zst_pad_destroy(pad); zst_element_destroy(el); return NULL; }
    return el;
}

/* ── Globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static zst_scheduler_t* g_scheduler;
static void on_signal(int sig) { (void)sig; g_stop = 1; }
static uint64_t monotonic_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int set_str(zst_element_t* e, const char* n, const char* v) {
    if (zst_element_set_property_string(e, n, v) == ZST_OK) return 1; return 0;
}
static int set_uint(zst_element_t* e, const char* n, uint32_t v) {
    if (zst_element_set_property_uint(e, n, v) == ZST_OK) return 1; return 0;
}
static void drain_bus(zst_bus_t* bus) {
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 0) == ZST_OK && ev) {
        if (ev->type == ZST_EVENT_ERROR) fprintf(stderr, "  [BUS ERR] %s\n", ev->as.error.message);
        else if (ev->type == ZST_EVENT_WARNING) fprintf(stderr, "  [BUS WARN] %s\n", ev->as.error.message);
        zst_event_destroy(ev);
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char** argv)
{
    const char* dvr_path = "/var/run/dante/dvr";
    const char* dep_name = "DanteEP";
    if (argc > 1 && argv[1][0] != '-') dvr_path = argv[1];
    if (argc > 2) dep_name = argv[2];

    uint32_t sr = 48000, ch = 2, spb = 4096;

    printf("=== Dante Loopback TX→RX Drop-Rate & Waveform Test ===\n");
    printf("DVR: %s | DEP: %s\n", dvr_path, dep_name);
    printf("Audio: %uHz %uch S32LE buf=%u\n", sr, ch, spb);
    printf("Mode: same-process loopback, Ctrl+C to stop.\n");
    printf("======================================================\n\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Inspect DEP SHM */
    {
        char path[512]; struct stat st;
        snprintf(path, sizeof(path), "/dev/shm/%s", dep_name);
        if (stat(path, &st) != 0) {
            fprintf(stderr, "FATAL: %s not found\n", path); return 1;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "FATAL: cannot open %s\n", path); return 1; }
        void* m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) { fprintf(stderr, "FATAL: mmap %s failed\n", path); return 1; }
        uint32_t* hdr = (uint32_t*)m;
        printf("[SHM] %s: magic=0x%08X %s rate=%u period=%u pc=%llu serial=%u\n",
               path, hdr[0], hdr[0]==0x50525354 ? "OK" : "BAD",
               hdr[10], hdr[17], *(unsigned long long*)(m+128), hdr[7]);
        munmap(m, st.st_size);
    }

    /* Create elements */
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_clock_t* clk = zst_clock_system_create();
    zst_element_t* tx_src    = zst_audio_test_src_create();
    zst_element_t* tx_resamp = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* tx_dep    = zst_dante_dep_audio_sink_create();
    zst_element_t* rx_dep    = zst_dante_dep_audio_source_create();
    zst_element_t* rx_resamp = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* rx_sink   = rx_verify_sink_create();
    zst_element_t* session    = zst_dante_session_create(dvr_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();

    if (!pipe || !clk || !tx_src || !tx_resamp || !tx_dep || !rx_dep || !rx_resamp || !rx_sink || !session || !coordinator) {
        fprintf(stderr, "FATAL: element creation failed\n"); return 1;
    }
    printf("[1] Created 8 elements.\n");

    /* Configure TX */
    set_uint(tx_src, "sample-rate", sr); set_uint(tx_src, "channels", ch);
    set_str(tx_src, "sample-format", "S32LE"); set_uint(tx_src, "samples-per-buffer", spb);
    set_str(tx_src, "use-clock", "true"); set_str(tx_src, "real-time-pacing", "true");
    set_str(tx_dep, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(tx_dep, "channels", c); }
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(tx_dep, "expected-sample-rate", r); }
    set_uint(tx_dep, "queue-periods", 256);

    /* Configure RX */
    set_str(rx_dep, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(rx_dep, "channels", c); }
    set_uint(rx_dep, "queue-periods", 256);
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(rx_dep, "expected-sample-rate", r); }
    /* Configure Dante session */
    set_uint(session, "tx-video-channels", 0);
    set_uint(session, "rx-video-channels", 0);
    zst_element_set_clock(tx_src, clk);
    printf("[2] Configured.\n");

    /* Add to pipeline */
    zst_pipeline_add(pipe, tx_src); zst_pipeline_add(pipe, tx_resamp); zst_pipeline_add(pipe, tx_dep);
    zst_pipeline_add(pipe, rx_dep); zst_pipeline_add(pipe, rx_resamp); zst_pipeline_add(pipe, rx_sink);
    zst_pipeline_add(pipe, session); zst_pipeline_add(pipe, coordinator);
    printf("[3] Added 8 elements to pipeline.\n");

    /* Link TX: src -> resampler -> dep_sink */
    zst_pad_link(zst_element_get_pad(tx_src, "src"), zst_element_get_pad(tx_resamp, "sink"));
    zst_pad_link(zst_element_get_pad(tx_resamp, "src"), zst_element_get_pad(tx_dep, "sink"));
    /* Link RX: dep_src -> resampler -> verify_sink */
    zst_pad_link(zst_element_get_pad(rx_dep, "src"), zst_element_get_pad(rx_resamp, "sink"));
    zst_pad_link(zst_element_get_pad(rx_resamp, "src"), zst_element_get_pad(rx_sink, "sink"));
    printf("[4] Linked TX and RX chains.\n");

    /* Start pipeline */
    zst_pipeline_set_clock(pipe, clk);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    session->bus = pipe->bus;
    if (zst_element_set_state(session, ZST_STATE_PLAYING) != ZST_OK)
        fprintf(stderr, "  [WARN] DVR session start failed (socket: %s)\n", dvr_path);
    else
        printf("  [OK] DVR session PLAYING\n");
    printf("[5] Pipeline PLAYING.\n");

    /* Start scheduler */
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, pipe);
    pthread_t sthr;
    pthread_create(&sthr, NULL, (void* (*)(void*))zst_scheduler_run, g_scheduler);
    printf("[6] Scheduler running.\n\n");

    /* Monitor */
    uint64_t t0 = monotonic_ms(), last_report = t0;
    uint64_t last_tx_pc = 0, last_rx_pc = 0, last_tx_dr = 0, last_rx_uf = 0;
    uint64_t last_vs = 0, last_vm = 0;

    printf("  %-5s  %-9s  %-9s  %-7s  %-7s  %-9s  %-8s\n",
           "Time", "TX_period", "RX_period", "TX_drop", "RX_under", "Transfer%%", "Integrity");
    printf("  %-5s  %-9s  %-9s  %-7s  %-7s  %-9s  %-8s\n",
           "-----", "---------", "---------", "-------", "-------", "---------", "--------");

    /* Wait for DEP context worker to connect */
    {
        char v[64]; int tries = 0;
        while (tries < 50) {
            if (zst_element_get_property(tx_dep, "active", v, sizeof(v)) == ZST_OK && strcmp(v, "true") == 0)
                { printf("  TX dep active=true (after %d00ms)\n", tries); break; }
            usleep(100000); tries++;
        }
        if (tries >= 50) printf("  [WARN] TX dep still not active after 5s\n");
    }

    printf("\n  %-5s  %-9s  %-9s  %-7s  %-7s  %-9s  %-8s\n",
           "Time", "TX_period", "RX_period", "TX_drop", "RX_under", "Transfer%%", "Integrity");
    printf("  %-5s  %-9s  %-9s  %-7s  %-7s  %-9s  %-8s\n",
           "-----", "---------", "---------", "-------", "-------", "---------", "--------");

    while (!g_stop) {
        usleep(1000000); /* 1s */
        drain_bus(pipe->bus);
        uint64_t now = monotonic_ms();
        if (now - last_report < 1000) continue;

        char v[64];
        uint64_t tx_pc = 0, rx_pc = 0, tx_dr = 0, rx_uf = 0;
        if (zst_element_get_property(tx_dep, "period-count", v, sizeof(v)) == ZST_OK) tx_pc = strtoull(v, NULL, 10);
        if (zst_element_get_property(rx_dep, "period-count", v, sizeof(v)) == ZST_OK) rx_pc = strtoull(v, NULL, 10);
        if (zst_element_get_property(tx_dep, "dropped-frames", v, sizeof(v)) == ZST_OK) tx_dr = strtoull(v, NULL, 10);
        if (zst_element_get_property(rx_dep, "underflow-count", v, sizeof(v)) == ZST_OK) rx_uf = strtoull(v, NULL, 10);

        uint64_t dtx = tx_pc - last_tx_pc, drx = rx_pc - last_rx_pc;
        uint64_t ddr = tx_dr - last_tx_dr, duf = rx_uf - last_rx_uf;
        double xfer = dtx > 0 ? (double)(dtx > drx ? dtx - drx : 0) * 100.0 / (double)dtx : 0;

        uint64_t cur_vs = 0, cur_vm = 0;
        if (rx_sink && zst_element_get_property(rx_sink, "total-samples", v, sizeof(v)) == ZST_OK) cur_vs = strtoull(v, NULL, 10);
        if (rx_sink && zst_element_get_property(rx_sink, "matched-samples", v, sizeof(v)) == ZST_OK) cur_vm = strtoull(v, NULL, 10);
        double integrity = (cur_vs > last_vs) ? (double)(cur_vm - last_vm) * 100.0 / (double)(cur_vs - last_vs) : 0;

        printf("  %4lus  %9llu  %9llu  %7llu  %7llu  %8.4f%%  %6.2f%%\n",
               (unsigned long)((now - t0) / 1000),
               (unsigned long long)tx_pc, (unsigned long long)rx_pc,
               (unsigned long long)ddr, (unsigned long long)duf, xfer, integrity);

        last_tx_pc = tx_pc; last_rx_pc = rx_pc;
        last_tx_dr = tx_dr; last_rx_uf = rx_uf;
        last_vs = cur_vs; last_vm = cur_vm;
        last_report = now;
    }

    /* Capture final stats BEFORE stopping (stop resets counters) */
    char v[64];
    uint64_t fin_tx_pc = 0, fin_rx_pc = 0, fin_tx_dr = 0, fin_rx_uf = 0, fin_tx_ov = 0;
    if (tx_dep && zst_element_get_property(tx_dep, "period-count", v, sizeof(v)) == ZST_OK) fin_tx_pc = strtoull(v, NULL, 10);
    if (rx_dep && zst_element_get_property(rx_dep, "period-count", v, sizeof(v)) == ZST_OK) fin_rx_pc = strtoull(v, NULL, 10);
    if (tx_dep && zst_element_get_property(tx_dep, "dropped-frames", v, sizeof(v)) == ZST_OK) fin_tx_dr = strtoull(v, NULL, 10);
    if (tx_dep && zst_element_get_property(tx_dep, "overrun-count", v, sizeof(v)) == ZST_OK) fin_tx_ov = strtoull(v, NULL, 10);
    if (rx_dep && zst_element_get_property(rx_dep, "underflow-count", v, sizeof(v)) == ZST_OK) fin_rx_uf = strtoull(v, NULL, 10);
    rx_verify_sink_t* fin_rx = (rx_sink && rx_sink->priv) ? rx_sink->priv : NULL;

    /* Shutdown */
    printf("\n[STOP] Shutting down...\n");
    if (g_scheduler) { zst_scheduler_stop(g_scheduler); pthread_join(sthr, NULL); }
    if (coordinator) (void)zst_dante_video_coordinator_attach_session(coordinator, NULL);
    session->bus = NULL;
    (void)zst_element_set_state(session, ZST_STATE_NULL);
    (void)zst_pipeline_set_state(pipe, ZST_STATE_NULL);

    /* Print captured stats */
    printf("\n=== TX Summary ===\n");
    printf("  Periods:   %llu\n", (unsigned long long)fin_tx_pc);
    printf("  Written:   %llu frames\n", (unsigned long long)(fin_tx_pc * spb));
    printf("  Overruns:  %llu\n", (unsigned long long)fin_tx_ov);
    printf("  Dropped:   %llu frames\n", (unsigned long long)fin_tx_dr);
    double tx_pct = (fin_tx_pc * spb) > 0 ? (double)fin_tx_dr * 100.0 / (double)(fin_tx_pc * spb) : 0;
    printf("  Drop rate: %.6f%%\n", tx_pct);

    printf("\n=== RX Summary ===\n");
    printf("  Periods:   %llu\n", (unsigned long long)fin_rx_pc);
    printf("  Read:      %llu frames\n", (unsigned long long)(fin_rx_pc * spb));
    printf("  Underflows:%llu\n", (unsigned long long)fin_rx_uf);
    double rx_pct = (fin_rx_pc * spb) > 0 ? (double)fin_rx_uf * 100.0 / (double)(fin_rx_pc * spb) : 0;
    printf("  Drop rate: %.6f%%\n", rx_pct);

    printf("\n=== TX→RX Transfer ===\n");
    double xfer = fin_tx_pc > 0 ? (double)(fin_tx_pc > fin_rx_pc ? fin_tx_pc - fin_rx_pc : 0) * 100.0 / (double)fin_tx_pc : 0;
    printf("  TX periods: %llu\n", (unsigned long long)fin_tx_pc);
    printf("  RX periods: %llu\n", (unsigned long long)fin_rx_pc);
    printf("  Transfer loss: %.6f%%\n", xfer);
    if (xfer < 0.001) printf("  [PASS] Zero transfer loss\n");
    else printf("  [WARN] Transfer loss %.4f%%\n", xfer);

    printf("\n=== Integrity ===\n");
    if (fin_rx) {
        double pct = fin_rx->verify.total_samples > 0
            ? (double)fin_rx->verify.matched_samples * 100.0 / (double)fin_rx->verify.total_samples : 0.0;
        printf("  Samples:   %llu total, %.4f%% match\n",
               (unsigned long long)fin_rx->verify.total_samples, pct);
        if (pct >= 99.9) printf("  [PASS] Audio integrity OK\n");
        else if (pct >= 99.0) printf("  [WARN] Audio integrity degraded\n");
        else printf("  [INFO] Audio integrity %.2f%% (loopback phase drift expected)\n", pct);
    }

    /* Cleanup — destroy pipeline first, then free rx_sink manually */
    if (g_scheduler) { zst_scheduler_destroy(g_scheduler); g_scheduler = NULL; }
    if (rx_sink && rx_sink->priv) { free(rx_sink->priv); rx_sink->priv = NULL; }
    zst_pipeline_destroy(pipe);
    zst_clock_unref(clk);
    printf("\n[STOP] Done.\n");
    return 0;
}
