/*============================================================================
    test_dante_tx_droprate.c — Dante TX drop-rate monitor

    Measures audio drop rate on the TX path:
      audiotestsrc (440Hz sine, 48kHz, S32LE, vol=0.8)
        -> audioresampler (48kHz/S32LE)
        -> dantedepaudiosink (DEP shm)

    Monitors:
      - period-count:    total periods written to DEP FIFO
      - overrun-count:   number of overrun events (FIFO full)
      - dropped-frames:  total audio frames dropped due to overruns
      - drop-rate:       dropped_frames / written_frames (%)

    Runs indefinitely until Ctrl+C. Logs to file.

    Usage:
      ./test_dante_tx_droprate [--log <file>] [dvr_socket] [dep_shm]

    Default log: dante_tx_droprate_YYYYMMDD_HHMMSS.log
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

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

/* ── Globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static zst_scheduler_t*      g_scheduler;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */
static int set_str(zst_element_t* e, const char* n, const char* v) {
    if (zst_element_set_property_string(e, n, v) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%s FAILED\n", n, v); return 0;
}
static int set_uint(zst_element_t* e, const char* n, uint32_t v) {
    if (zst_element_set_property_uint(e, n, v) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%u FAILED\n", n, v); return 0;
}

static void print_dep_sink(zst_element_t* el) {
    char v[64];
    const char* keys[] = {
        "active", "sample-rate", "period-count", "overrun-count",
        "underflow-count", "dropped-frames", NULL
    };
    printf("  TX dep_sink:\n");
    for (int i = 0; keys[i]; ++i) {
        if (zst_element_get_property(el, keys[i], v, sizeof(v)) == ZST_OK)
            printf("    %-18s = %s\n", keys[i], v);
    }
}

static void drain_bus(zst_bus_t* bus) {
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 0) == ZST_OK && ev) {
        if (ev->type == ZST_EVENT_ERROR)
            fprintf(stderr, "  [BUS ERR] %s\n", ev->as.error.message);
        else if (ev->type == ZST_EVENT_WARNING)
            fprintf(stderr, "  [BUS WARN] %s\n", ev->as.error.message);
        zst_event_destroy(ev);
    }
}

/* ── Tee logging ───────────────────────────────────────────────────────── */
static int g_log_fd = -1;

static void tee_child_func(int read_fd, int term_fd, FILE* log_fp) {
    char buf[8196];
    ssize_t n;
    while ((n = read(read_fd, buf, sizeof(buf))) > 0) {
        (void)write(term_fd, buf, (size_t)n);
        (void)fwrite(buf, 1, (size_t)n, log_fp);
        fflush(log_fp);
    }
}

static int setup_tee(const char* log_path, int original_fd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
    FILE* log_fp = fopen(log_path, "a");
    if (!log_fp) { close(pipefd[0]); close(pipefd[1]); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[1]);
        tee_child_func(pipefd[0], original_fd, log_fp);
        fclose(log_fp);
        close(pipefd[0]);
        _exit(0);
    }
    close(pipefd[0]);
    return pipefd[1];
}

static void setup_log_tee(const char* log_path) {
    int orig_stdout = dup(STDOUT_FILENO);
    int orig_stderr = dup(STDERR_FILENO);
    if (orig_stdout < 0 || orig_stderr < 0) return;
    int tee_out = setup_tee(log_path, orig_stdout);
    int tee_err = setup_tee(log_path, orig_stderr);
    if (tee_out >= 0) dup2(tee_out, STDOUT_FILENO);
    if (tee_err >= 0) dup2(tee_err, STDERR_FILENO);
    if (tee_out >= 0) g_log_fd = tee_out;
    else if (tee_err >= 0) g_log_fd = tee_err;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Main                                                                    */
/* ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char** argv)
{
    /* ── Parse arguments ─────────────────────────────────────────────── */
    const char* dvr_path = "/var/run/dante/dvr";
    const char* dep_name = "DanteEP";
    const char* log_path = NULL;

    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--log") == 0 && arg_idx + 1 < argc) {
            log_path = argv[++arg_idx]; arg_idx++;
        } else {
            break;
        }
    }
    if (arg_idx < argc) dvr_path = argv[arg_idx++];
    if (arg_idx < argc) dep_name = argv[arg_idx++];

    /* ── Auto-generate log file name ─────────────────────────────────── */
    char auto_log[256];
    if (!log_path) {
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
        snprintf(auto_log, sizeof(auto_log), "dante_tx_droprate_%s.log", ts);
        log_path = auto_log;
    }

    /* ── Setup log tee (before any output) ───────────────────────────── */
    setup_log_tee(log_path);
    fprintf(stderr, "[LOG] Tee output to: %s\n", log_path);

    /* Ignore SIGPIPE — tee child may die first on Ctrl+C */
    signal(SIGPIPE, SIG_IGN);

    uint32_t sr = 48000, ch = 2, spb = 4096;

    printf("=== Dante TX Drop-Rate Monitor ===\n");
    printf("DVR: %s | DEP: %s\n", dvr_path, dep_name);
    printf("Log: %s\n", log_path);
    printf("Audio: %uHz %uch S32LE (buf=%u) 440Hz sine vol=0.8\n", sr, ch, spb);
    printf("Ctrl+C to stop.\n");
    printf("====================================\n\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ── Create elements ─────────────────────────────────────────────── */
    printf("[1] Creating elements...\n");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_clock_t*    clock = zst_clock_system_create();

    zst_element_t* audio_src  = zst_audio_test_src_create();
    zst_element_t* resampler  = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* dep_sink   = zst_dante_dep_audio_sink_create();
    zst_element_t* session    = zst_dante_session_create(dvr_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();

    if (!pipe || !clock || !audio_src || !resampler || !dep_sink || !session || !coordinator) {
        fprintf(stderr, "[FATAL] element creation failed\n"); return 1;
    }
    printf("  [OK] All elements created.\n");

    /* ── Configure ───────────────────────────────────────────────────── */
    printf("[2] Configuring...\n");

    set_uint(audio_src, "sample-rate", sr);
    set_uint(audio_src, "channels", ch);
    set_str(audio_src, "sample-format", "S32LE");
    set_uint(audio_src, "samples-per-buffer", spb);
    set_str(audio_src, "use-clock", "true");
    set_str(audio_src, "real-time-pacing", "true");

    set_str(dep_sink, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(dep_sink, "channels", c); }
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(dep_sink, "expected-sample-rate", r); }
    set_uint(dep_sink, "queue-periods", 256);

    set_uint(session, "tx-video-channels", 1);
    set_uint(session, "rx-video-channels", 0);

    zst_element_set_clock(audio_src, clock);
    printf("  [OK] Configuration done.\n");

    /* ── Add to pipeline ─────────────────────────────────────────────── */
    printf("[3] Adding to pipeline...\n");
    zst_pipeline_add(pipe, audio_src);
    zst_pipeline_add(pipe, resampler);
    zst_pipeline_add(pipe, dep_sink);
    zst_pipeline_add(pipe, session);
    zst_pipeline_add(pipe, coordinator);
    printf("  [OK] %u elements.\n", pipe->nb_elements);

    /* ── Link audio ──────────────────────────────────────────────────── */
    printf("[4] Linking: audio_src -> resampler -> dep_sink\n");
    zst_pad_link(zst_element_get_pad(audio_src, "src"),
                 zst_element_get_pad(resampler, "sink"));
    zst_pad_link(zst_element_get_pad(resampler, "src"),
                 zst_element_get_pad(dep_sink, "sink"));
    printf("  [OK] Linked.\n");

    /* ── Start pipeline ──────────────────────────────────────────────── */
    printf("[5] Starting pipeline...\n");
    zst_pipeline_set_clock(pipe, clock);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    session->bus = pipe->bus;
    if (zst_element_set_state(session, ZST_STATE_PLAYING) != ZST_OK)
        fprintf(stderr, "  [WARN] DVR session start failed\n");
    else
        printf("  [OK] DVR session PLAYING\n");

    /* ── Scheduler ───────────────────────────────────────────────────── */
    printf("[6] Starting scheduler...\n");
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, pipe);
    pthread_t sthr;
    pthread_create(&sthr, NULL, (void* (*)(void*))zst_scheduler_run, g_scheduler);
    printf("  [OK] Scheduler running.\n\n");

    /* ── Monitor loop ────────────────────────────────────────────────── */
    uint64_t t0 = monotonic_ms(), last_report = t0;
    uint64_t last_periods = 0, last_overruns = 0, last_dropped = 0;
    uint32_t interval_sec = 2;

    printf("  %-6s  %-12s  %-10s  %-12s  %-10s  %-10s  %-10s  %-10s\n",
           "Time", "Periods", "ΔPeriods", "Overruns", "ΔOverruns",
           "Dropped", "ΔDropped", "Drop%");
    printf("  %-6s  %-12s  %-10s  %-12s  %-10s  %-10s  %-10s  %-10s\n",
           "------", "----------", "--------", "----------",
           "---------", "--------", "---------", "-------");

    while (!g_stop) {
        usleep(200000);
        uint64_t now = monotonic_ms();
        drain_bus(pipe->bus);

        if (now - last_report >= (uint64_t)interval_sec * 1000) {
            char v[64];
            uint64_t cur_periods = 0, cur_overruns = 0, cur_dropped = 0;

            if (zst_element_get_property(dep_sink, "period-count",
                                         v, sizeof(v)) == ZST_OK)
                cur_periods = strtoull(v, NULL, 10);
            if (zst_element_get_property(dep_sink, "overrun-count",
                                         v, sizeof(v)) == ZST_OK)
                cur_overruns = strtoull(v, NULL, 10);
            if (zst_element_get_property(dep_sink, "dropped-frames",
                                         v, sizeof(v)) == ZST_OK)
                cur_dropped = strtoull(v, NULL, 10);

            uint64_t delta_periods  = cur_periods  - last_periods;
            uint64_t delta_overruns = cur_overruns - last_overruns;
            uint64_t delta_dropped = cur_dropped  - last_dropped;

            /* written_frames = periods × samples_per_buffer */
            uint64_t written_this = delta_periods * spb;
            double drop_pct = written_this > 0
                ? (double)delta_dropped * 100.0 / (double)written_this : 0.0;
            /* Cumulative drop rate */
            uint64_t total_written = cur_periods * spb;
            double cum_drop_pct = total_written > 0
                ? (double)cur_dropped * 100.0 / (double)total_written : 0.0;

            printf("  %4lus   %12llu  %10llu  %12llu  %10llu  %10llu  %10llu  %8.4f%%\n",
                   (unsigned long)((now - t0) / 1000),
                   (unsigned long long)cur_periods,
                   (unsigned long long)delta_periods,
                   (unsigned long long)cur_overruns,
                   (unsigned long long)delta_overruns,
                   (unsigned long long)cur_dropped,
                   (unsigned long long)delta_dropped,
                   drop_pct);

            if (delta_periods > 0) {
                printf("    [DETAIL] period_rate=%.0f/s overrun_rate=%.1f/s drop_frames=%llu written=%llu cum_drop=%.6f%%\n",
                       (double)delta_periods / (double)interval_sec,
                       (double)delta_overruns / (double)interval_sec,
                       (unsigned long long)cur_dropped,
                       (unsigned long long)total_written,
                       cum_drop_pct);
            }

            /* Also print full dep_sink status periodically */
            if ((now - t0) / 1000 >= 10 && ((now - t0) / 1000) % 10 == 0) {
                print_dep_sink(dep_sink);
            }

            last_periods  = cur_periods;
            last_overruns = cur_overruns;
            last_dropped  = cur_dropped;
            last_report = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────────────── */
    printf("\n[STOP] Shutting down...\n");
    zst_scheduler_stop(g_scheduler);
    pthread_join(sthr, NULL);
    (void)zst_element_set_state(session, ZST_STATE_NULL);
    (void)zst_pipeline_set_state(pipe, ZST_STATE_NULL);

    printf("[STOP] Final:\n");
    print_dep_sink(dep_sink);

    /* Final drop-rate summary */
    {
        char v[64];
        uint64_t total_periods = 0, total_overruns = 0, total_dropped = 0;
        if (zst_element_get_property(dep_sink, "period-count",
                                     v, sizeof(v)) == ZST_OK)
            total_periods = strtoull(v, NULL, 10);
        if (zst_element_get_property(dep_sink, "overrun-count",
                                     v, sizeof(v)) == ZST_OK)
            total_overruns = strtoull(v, NULL, 10);
        if (zst_element_get_property(dep_sink, "dropped-frames",
                                     v, sizeof(v)) == ZST_OK)
            total_dropped = strtoull(v, NULL, 10);

        uint64_t total_written = total_periods * spb;
        double cum_pct = total_written > 0
            ? (double)total_dropped * 100.0 / (double)total_written : 0.0;

        printf("\n=== TX Drop-Rate Summary ===\n");
        printf("  Total periods:      %llu\n", (unsigned long long)total_periods);
        printf("  Total written:      %llu frames\n", (unsigned long long)total_written);
        printf("  Overrun events:     %llu\n", (unsigned long long)total_overruns);
        printf("  Dropped frames:     %llu\n", (unsigned long long)total_dropped);
        printf("  Drop rate:          %.6f%%\n", cum_pct);
        if (total_dropped == 0)
            printf("  [PASS] Zero drops\n");
        else if (cum_pct < 0.01)
            printf("  [PASS] Drop rate negligible (%.6f%%)\n", cum_pct);
        else if (cum_pct < 1.0)
            printf("  [WARN] Drop rate low (%.4f%%)\n", cum_pct);
        else
            printf("  [FAIL] Drop rate high (%.4f%%)\n", cum_pct);
        printf("===========================\n");
    }

    (void)zst_dante_video_coordinator_attach_session(coordinator, NULL);
    session->bus = NULL;
    zst_scheduler_destroy(g_scheduler); g_scheduler = NULL;
    zst_pipeline_destroy(pipe);
    zst_clock_unref(clock);

    printf("[STOP] Done.\n");
    return 0;
}
