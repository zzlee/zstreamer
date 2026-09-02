/*============================================================================
    test_dante_rx_droprate.c — Dante RX drop-rate monitor

    Reads audio from DEP shared memory and measures:
      - period-count:     total periods read from DEP FIFO
      - underflow-count:  number of underflow events (FIFO empty when read)
      - dropped-frames:   total audio frames dropped due to underflows
      - drop-rate:        dropped_frames / expected_frames (%)
      - integrity:        sine wave match vs TX-generated pattern

    RX side:
      dantedepaudiosrc -> audioresampler -> rx-verify-sink
        (verifies 440Hz sine, vol=0.8)

    Designed to run on a separate machine from the TX side.

    Usage:
      ./test_dante_rx_droprate [--log <file>] [dvr_socket] [dep_shm]

    Default log: dante_rx_droprate_YYYYMMDD_HHMMSS.log
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
#include <sys/wait.h>

/* ── DEP ABI (for SHM inspection) ─────────────────────────────────────── */
#define DEP_HEADER_MAGIC UINT32_C(0x50525354)
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

/* ── Sine wave verification ───────────────────────────────────────────── */
typedef struct {
    double frequency;
    double volume;
    uint32_t sample_rate;
    double phase;
    uint64_t total_samples;
    uint64_t matched_samples;
    uint64_t mismatched_samples;
    uint64_t max_error_sample;
    double   max_error_value;
    double   tolerance;
} sine_verify_state_t;

static void sine_verify_init(sine_verify_state_t* sv, uint32_t sr, double freq, double vol) {
    memset(sv, 0, sizeof(*sv));
    sv->frequency = freq;
    sv->volume = vol;
    sv->sample_rate = sr;
    sv->tolerance = 0.02;
}

static double sine_verify_next(sine_verify_state_t* sv) {
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double x = sv->phase * two_pi;
    if (x > pi) x -= two_pi;
    const double b = 4.0 / pi;
    const double c = -4.0 / (pi * pi);
    const double p = 0.225;
    double y = b * x + c * x * (x < 0.0 ? -x : x);
    y = p * (y * (y < 0.0 ? -y : y) - y) + y;
    double step = sv->sample_rate > 0 ? sv->frequency / (double)sv->sample_rate : 0.0;
    sv->phase += step;
    while (sv->phase >= 1.0) sv->phase -= 1.0;
    while (sv->phase < 0.0) sv->phase += 1.0;
    y *= sv->volume;
    if (y > 1.0) y = 1.0;
    if (y < -1.0) y = -1.0;
    return y;
}

static void sine_verify_process(sine_verify_state_t* sv, const int32_t* samples, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double expected = sine_verify_next(sv) * 2147483647.0;
        double actual = (double)samples[i];
        double err = actual - expected;
        double abs_err = err < 0.0 ? -err : err;
        sv->total_samples++;
        double tol = sv->tolerance * 2147483647.0;
        if (abs_err <= tol)
            sv->matched_samples++;
        else {
            sv->mismatched_samples++;
            if (abs_err > sv->max_error_value) {
                sv->max_error_value = abs_err;
                sv->max_error_sample = sv->total_samples;
            }
        }
    }
}

/* ── Globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static zst_scheduler_t*      g_scheduler;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

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

static void print_dep_ep(zst_element_t* el, const char* label) {
    char v[64];
    const char* keys[] = {
        "active", "sample-rate", "period-count", "overrun-count",
        "underflow-count", "dropped-frames", NULL
    };
    printf("  %s:\n", label);
    for (int i = 0; keys[i]; ++i)
        if (zst_element_get_property(el, keys[i], v, sizeof(v)) == ZST_OK)
            printf("    %-18s = %s\n", keys[i], v);
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

static void inspect_dep(const char* name) {
    char path[512]; struct stat st;
    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    printf("[SHM] %s: ", path);
    if (stat(path, &st) != 0) { fprintf(stderr, "MISSING\n"); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open fail\n"); return; }
    void* m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { fprintf(stderr, "mmap fail\n"); return; }
    dep_diag_header_t* h = m;
    printf("magic=0x%08X %s rate=%u period=%u pc=%llu serial=%u\n",
           h->magic, h->magic == DEP_HEADER_MAGIC ? "OK" : "BAD",
           h->sample_rate, h->samples_per_period,
           (unsigned long long)h->period_count, h->reset_serial);
    munmap(m, st.st_size);
}

/* ── RX verify sink ───────────────────────────────────────────────────── */
typedef struct {
    sine_verify_state_t verify;
    uint64_t total_buffers;
    uint64_t total_samples;
} rx_verify_sink_t;

static zst_result_t rx_verify_open(zst_element_t* el) {
    rx_verify_sink_t* s = el->priv;
    s->total_buffers = 0;
    s->total_samples = 0;
    sine_verify_init(&s->verify, 48000, 440.0, 0.8);
    return ZST_OK;
}

static zst_result_t rx_verify_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    rx_verify_sink_t* s = el->priv;
    (void)out;
    if (!in) return ZST_ERROR;
    if (in->memory.data && in->memory.size > 0) {
        uint32_t nb = (uint32_t)(in->memory.size / sizeof(int32_t));
        sine_verify_process(&s->verify, (const int32_t*)in->memory.data, nb);
        s->total_samples += nb;
    }
    s->total_buffers++;
    return ZST_OK;
}

static zst_result_t rx_verify_get_property(zst_element_t* el, const char* name,
                                           char* out, size_t max_len) {
    rx_verify_sink_t* s = el->priv;
    if (strcmp(name, "total-buffers") == 0) {
        snprintf(out, max_len, "%llu", (unsigned long long)s->total_buffers); return ZST_OK;
    } else if (strcmp(name, "total-samples") == 0) {
        snprintf(out, max_len, "%llu", (unsigned long long)s->total_samples); return ZST_OK;
    } else if (strcmp(name, "matched-samples") == 0) {
        snprintf(out, max_len, "%llu", (unsigned long long)s->verify.matched_samples); return ZST_OK;
    } else if (strcmp(name, "mismatched-samples") == 0) {
        snprintf(out, max_len, "%llu", (unsigned long long)s->verify.mismatched_samples); return ZST_OK;
    } else if (strcmp(name, "integrity-pct") == 0) {
        double pct = s->verify.total_samples > 0
            ? (double)s->verify.matched_samples * 100.0 / (double)s->verify.total_samples : 0.0;
        snprintf(out, max_len, "%.4f%%", pct); return ZST_OK;
    } else if (strcmp(name, "max-error") == 0) {
        snprintf(out, max_len, "%.1f (at sample %llu)", s->verify.max_error_value,
                 (unsigned long long)s->verify.max_error_sample); return ZST_OK;
    }
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
    if (!pad || zst_element_add_pad(el, pad) != ZST_OK) {
        if (pad) zst_pad_destroy(pad);
        zst_element_destroy(el);
        return NULL;
    }
    return el;
}

/* ── Tee logging ───────────────────────────────────────────────────────── */
static int g_log_fd = -1;

static void tee_child_func(int read_fd, int term_fd, FILE* log_fp) {
    char buf[8196]; ssize_t n;
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
        fclose(log_fp); close(pipefd[0]); _exit(0);
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

    char auto_log[256];
    if (!log_path) {
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
        snprintf(auto_log, sizeof(auto_log), "dante_rx_droprate_%s.log", ts);
        log_path = auto_log;
    }

    setup_log_tee(log_path);
    fprintf(stderr, "[LOG] Tee output to: %s\n", log_path);
    signal(SIGPIPE, SIG_IGN);

    uint32_t sr = 48000, ch = 2, spb = 4096;

    printf("=== Dante RX Drop-Rate Monitor ===\n");
    printf("DVR: %s | DEP: %s\n", dvr_path, dep_name);
    printf("Log: %s\n", log_path);
    printf("Audio: %uHz %uch S32LE (buf=%u) 440Hz sine verify\n", sr, ch, spb);
    printf("Ctrl+C to stop.\n");
    printf("====================================\n\n");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    inspect_dep(dep_name); printf("\n");

    /* ── Create elements ─────────────────────────────────────────────── */
    printf("[1] Creating elements...\n");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_clock_t*    clock = zst_clock_system_create();
    zst_element_t* dep_src    = zst_dante_dep_audio_source_create();
    zst_element_t* resampler  = zst_audio_resampler_create((int)sr, (int)ch, "S32LE");
    zst_element_t* rx_sink    = rx_verify_sink_create();
    zst_element_t* session    = zst_dante_session_create(dvr_path);
    zst_element_t* coordinator = zst_dante_video_coordinator_create();

    if (!pipe || !clock || !dep_src || !resampler || !rx_sink || !session || !coordinator) {
        fprintf(stderr, "[FATAL] element creation failed\n"); return 1;
    }
    printf("  [OK] All elements created.\n");

    /* ── Configure ───────────────────────────────────────────────────── */
    printf("[2] Configuring...\n");

    set_str(dep_src, "shm-name", dep_name);
    { char c[32]; snprintf(c, sizeof(c), "0,1"); set_str(dep_src, "channels", c); }
    set_uint(dep_src, "queue-periods", 256);
    { char r[32]; snprintf(r, sizeof(r), "%u", sr); set_str(dep_src, "expected-sample-rate", r); }

    set_uint(session, "tx-video-channels", 0);
    set_uint(session, "rx-video-channels", 1);
    printf("  [OK] Configuration done.\n");

    /* ── Add to pipeline ─────────────────────────────────────────────── */
    printf("[3] Adding to pipeline...\n");
    zst_pipeline_add(pipe, dep_src);
    zst_pipeline_add(pipe, resampler);
    zst_pipeline_add(pipe, rx_sink);
    zst_pipeline_add(pipe, session);
    zst_pipeline_add(pipe, coordinator);
    printf("  [OK] %u elements.\n", pipe->nb_elements);

    /* ── Link ────────────────────────────────────────────────────────── */
    printf("[4] Linking: dep_src -> resampler -> rx_verify_sink\n");
    zst_pad_link(zst_element_get_pad(dep_src, "src"),
                 zst_element_get_pad(resampler, "sink"));
    zst_pad_link(zst_element_get_pad(resampler, "src"),
                 zst_element_get_pad(rx_sink, "sink"));
    printf("  [OK] Linked.\n");

    /* ── Start ───────────────────────────────────────────────────────── */
    printf("[5] Starting pipeline...\n");
    zst_pipeline_set_clock(pipe, clock);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    session->bus = pipe->bus;
    if (zst_element_set_state(session, ZST_STATE_PLAYING) != ZST_OK)
        fprintf(stderr, "  [WARN] DVR session start failed\n");
    else
        printf("  [OK] DVR session PLAYING\n");

    printf("[6] Starting scheduler...\n");
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    g_scheduler = zst_scheduler_create(&cfg);
    zst_scheduler_attach(g_scheduler, pipe);
    pthread_t sthr;
    pthread_create(&sthr, NULL, (void* (*)(void*))zst_scheduler_run, g_scheduler);
    printf("  [OK] Scheduler running.\n\n");

    /* ── Monitor ─────────────────────────────────────────────────────── */
    uint64_t t0 = monotonic_ms(), last_report = t0;
    uint64_t last_periods = 0, last_underflows = 0, last_dropped = 0;
    uint64_t last_verify_samples = 0, last_verify_matched = 0;
    uint32_t interval_sec = 2;

    printf("  %-6s  %-12s  %-10s  %-12s  %-10s  %-10s  %-10s  %-10s  %-8s\n",
           "Time", "Periods", "ΔPeriods", "Underflows", "ΔUnderflows",
           "Dropped", "ΔDropped", "Drop%", "Integrity");
    printf("  %-6s  %-12s  %-10s  %-12s  %-10s  %-10s  %-10s  %-10s  %-8s\n",
           "------", "----------", "--------", "----------",
           "---------", "--------", "---------", "-------", "--------");

    while (!g_stop) {
        usleep(200000);
        uint64_t now = monotonic_ms();
        drain_bus(pipe->bus);

        if (now - last_report >= (uint64_t)interval_sec * 1000) {
            char v[64];
            uint64_t cur_periods = 0, cur_underflows = 0, cur_dropped = 0;

            if (zst_element_get_property(dep_src, "period-count",
                                         v, sizeof(v)) == ZST_OK)
                cur_periods = strtoull(v, NULL, 10);
            if (zst_element_get_property(dep_src, "underflow-count",
                                         v, sizeof(v)) == ZST_OK)
                cur_underflows = strtoull(v, NULL, 10);
            if (zst_element_get_property(dep_src, "dropped-frames",
                                         v, sizeof(v)) == ZST_OK)
                cur_dropped = strtoull(v, NULL, 10);

            uint64_t delta_periods   = cur_periods   - last_periods;
            uint64_t delta_underflows = cur_underflows - last_underflows;
            uint64_t delta_dropped   = cur_dropped   - last_dropped;

            uint64_t written_this = delta_periods * spb;
            double drop_pct = written_this > 0
                ? (double)delta_dropped * 100.0 / (double)written_this : 0.0;
            uint64_t total_read = cur_periods * spb;
            double cum_drop_pct = total_read > 0
                ? (double)cur_dropped * 100.0 / (double)total_read : 0.0;

            /* Integrity from verify sink */
            uint64_t cur_vs = 0, cur_vm = 0;
            if (rx_sink) {
                if (zst_element_get_property(rx_sink, "total-samples",
                                             v, sizeof(v)) == ZST_OK)
                    cur_vs = strtoull(v, NULL, 10);
                if (zst_element_get_property(rx_sink, "matched-samples",
                                             v, sizeof(v)) == ZST_OK)
                    cur_vm = strtoull(v, NULL, 10);
            }
            uint64_t delta_vs = cur_vs - last_verify_samples;
            double integrity = delta_vs > 0
                ? (double)(cur_vm - last_verify_matched) * 100.0 / (double)delta_vs : 0.0;

            printf("  %4lus   %12llu  %10llu  %12llu  %10llu  %10llu  %10llu  %8.4f%%  %6.2f%%\n",
                   (unsigned long)((now - t0) / 1000),
                   (unsigned long long)cur_periods,
                   (unsigned long long)delta_periods,
                   (unsigned long long)cur_underflows,
                   (unsigned long long)delta_underflows,
                   (unsigned long long)cur_dropped,
                   (unsigned long long)delta_dropped,
                   drop_pct,
                   integrity);

            if (delta_periods > 0) {
                printf("    [DETAIL] period_rate=%.0f/s underflow_rate=%.1f/s "
                       "drop_frames=%llu read=%llu cum_drop=%.6f%%\n",
                       (double)delta_periods / (double)interval_sec,
                       (double)delta_underflows / (double)interval_sec,
                       (unsigned long long)cur_dropped,
                       (unsigned long long)total_read,
                       cum_drop_pct);
            }

            /* Full dep_src status every 10s */
            if ((now - t0) / 1000 >= 10 && ((now - t0) / 1000) % 10 == 0)
                print_dep_ep(dep_src, "RX dep_src");

            last_periods = cur_periods;
            last_underflows = cur_underflows;
            last_dropped = cur_dropped;
            last_verify_samples = cur_vs;
            last_verify_matched = cur_vm;
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
    print_dep_ep(dep_src, "RX dep_src");

    /* Final summary */
    {
        char v[64];
        uint64_t total_periods = 0, total_underflows = 0, total_dropped = 0;
        if (zst_element_get_property(dep_src, "period-count",
                                     v, sizeof(v)) == ZST_OK)
            total_periods = strtoull(v, NULL, 10);
        if (zst_element_get_property(dep_src, "underflow-count",
                                     v, sizeof(v)) == ZST_OK)
            total_underflows = strtoull(v, NULL, 10);
        if (zst_element_get_property(dep_src, "dropped-frames",
                                     v, sizeof(v)) == ZST_OK)
            total_dropped = strtoull(v, NULL, 10);

        uint64_t total_read = total_periods * spb;
        double cum_pct = total_read > 0
            ? (double)total_dropped * 100.0 / (double)total_read : 0.0;

        printf("\n=== RX Drop-Rate Summary ===\n");
        printf("  Total periods read:  %llu\n", (unsigned long long)total_periods);
        printf("  Total frames read:   %llu\n", (unsigned long long)total_read);
        printf("  Underflow events:    %llu\n", (unsigned long long)total_underflows);
        printf("  Dropped frames:      %llu\n", (unsigned long long)total_dropped);
        printf("  Drop rate:           %.6f%%\n", cum_pct);
        printf("===========================\n");
    }

    if (rx_sink) {
        rx_verify_sink_t* s = rx_sink->priv;
        double pct = s->verify.total_samples > 0
            ? (double)s->verify.matched_samples * 100.0 / (double)s->verify.total_samples : 0.0;
        printf("\n=== RX Audio Integrity Report ===\n");
        printf("  Total samples:   %llu\n", (unsigned long long)s->verify.total_samples);
        printf("  Matched:         %llu\n", (unsigned long long)s->verify.matched_samples);
        printf("  Mismatched:      %llu\n", (unsigned long long)s->verify.mismatched_samples);
        printf("  Integrity:       %.4f%%\n", pct);
        if (s->verify.max_error_value > 0)
            printf("  Max error:       %.1f LSB (at sample %llu)\n",
                   s->verify.max_error_value,
                   (unsigned long long)s->verify.max_error_sample);
        printf("================================\n");
        if (pct >= 99.9)
            printf("  [PASS] Audio integrity OK (%.4f%%)\n", pct);
        else if (pct >= 99.0)
            printf("  [WARN] Audio integrity degraded (%.4f%%)\n", pct);
        else
            printf("  [FAIL] Audio integrity FAILED (%.4f%%)\n", pct);
    }

    (void)zst_dante_video_coordinator_attach_session(coordinator, NULL);
    session->bus = NULL;
    zst_scheduler_destroy(g_scheduler); g_scheduler = NULL;
    zst_pipeline_destroy(pipe);
    zst_clock_unref(clock);
    if (rx_sink) { free(rx_sink->priv); zst_element_destroy(rx_sink); }

    printf("[STOP] Done.\n");
    return 0;
}
