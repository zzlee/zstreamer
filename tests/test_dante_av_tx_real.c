/*============================================================================
    test_dante_av_tx_real.c — Dante Audio TX real-hardware integration test

    Audio-only pipeline:
      audiotestsrc → audioresampler → dantedepaudiosink (DEP shared memory)

    Runs indefinitely until Ctrl+C. Designed for step-by-step hardware debugging.

    Usage:
      ./test_dante_av_tx_real [dep_shm_name]

    Defaults:
      dep_shm_name = DanteEP
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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── DEP ABI (from dante_dep_abi.h, duplicated here for standalone diag) ── */
#define DEP_HEADER_MAGIC UINT32_C(0x50525354)
#define DEP_ENCODING_PCM32 UINT32_C(32)

typedef struct {
    uint32_t magic;
    uint32_t object_bytes;
    uint32_t metadata_bytes;
    uint32_t flags;
    uint32_t tx_offset;
    uint32_t rx_offset;
    uint32_t timing_offset;
    uint32_t reset_serial;
    uint32_t sample_rate;
    uint32_t encoding;
    uint32_t samples_per_channel;
    uint32_t bytes_per_channel;
    uint32_t tx_channels;
    uint32_t rx_channels;
    uint32_t audio_reserved[2];
    uint32_t epoch_seconds;
    uint32_t epoch_samples;
    uint32_t samples_per_period;
    uint32_t period_alignment;
    uint64_t period_count;
    uint32_t drift_ppb;
    uint32_t monotonic_alignment;
    uint64_t monotonic_ns;
} dep_diag_header_t;

/* ── Globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static zst_scheduler_t*      g_scheduler;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_scheduler) zst_scheduler_stop(g_scheduler);
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */
static int set_str(zst_element_t* e, const char* name, const char* val)
{
    if (zst_element_set_property_string(e, name, val) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%s FAILED on %s\n",
            name, val, e->ops ? e->ops->name : "?");
    return 0;
}

static int set_uint(zst_element_t* e, const char* name, uint32_t val)
{
    if (zst_element_set_property_uint(e, name, val) == ZST_OK) return 1;
    fprintf(stderr, "  [WARN] set %s=%u FAILED on %s\n",
            name, val, e->ops ? e->ops->name : "?");
    return 0;
}

static const char* state_name(zst_state_t s)
{
    switch (s) {
    case ZST_STATE_NULL:    return "NULL";
    case ZST_STATE_READY:   return "READY";
    case ZST_STATE_PAUSED:  return "PAUSED";
    case ZST_STATE_PLAYING: return "PLAYING";
    default:                return "UNKNOWN";
    }
}

/* ── Print DEP sink element properties ─────────────────────────────────── */
static void print_dep_status(zst_element_t* dep_sink)
{
    char val[128] = {0};
    const char* keys[] = {
        "active", "sample-rate", "period-count", "reset-count",
        "overrun-count", "underflow-count", NULL
    };
    printf("  DEP sink element:\n");
    for (int i = 0; keys[i]; ++i) {
        if (zst_element_get_property(dep_sink, keys[i], val, sizeof(val)) == ZST_OK)
            printf("    %-18s = %s\n", keys[i], val);
        else
            printf("    %-18s = (unavailable)\n", keys[i]);
    }
    printf("    %-18s = %s\n", "element-state",
           state_name((zst_state_t)atomic_load(&dep_sink->state)));
}

/* ── Print all element states ──────────────────────────────────────────── */
static void print_pipeline_states(zst_pipeline_t* p)
{
    printf("  Pipeline element states:\n");
    for (uint32_t i = 0; i < p->nb_elements; ++i) {
        zst_element_t* el = p->elements[i];
        printf("    %-25s = %s\n",
               el->ops ? el->ops->name : "?",
               state_name((zst_state_t)atomic_load(&el->state)));
    }
}

/* ── Bus event listener ─────────────────────────────────────────────────── */
static void drain_bus(zst_bus_t* bus)
{
    zst_event_t* event = NULL;
    while (zst_bus_pop(bus, &event, 0) == ZST_OK && event) {
        switch (event->type) {
        case ZST_EVENT_ERROR:
            fprintf(stderr, "  [BUS ERROR] %s\n", event->as.error.message);
            break;
        case ZST_EVENT_WARNING:
            fprintf(stderr, "  [BUS WARN]  %s\n", event->as.error.message);
            break;
        case ZST_EVENT_DANTE_FLOW_CREATED:
            printf("  [BUS] DVR flow created (dir=%d, idx=%u, ch=%u, port=%u)\n",
                   event->as.dante_flow.flow.direction,
                   event->as.dante_flow.flow.flow_index,
                   event->as.dante_flow.flow.channel_index,
                   event->as.dante_flow.flow.port);
            break;
        case ZST_EVENT_DANTE_FLOW_DELETED:
            printf("  [BUS] DVR flow deleted (idx=%u)\n",
                   event->as.dante_flow.flow.flow_index);
            break;
        default:
            break;
        }
        zst_event_destroy(event);
    }
}

/* ── Inspect DEP shared memory header directly ─────────────────────────── */
static int inspect_dep_shm(const char* name)
{
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "/dev/shm/%s", name);

    printf("[SHM INSPECT] %s:\n", path);

    if (stat(path, &st) != 0) {
        fprintf(stderr, "  [MISSING] %s: %s\n", path, strerror(errno));
        return 0;
    }
    printf("  file size:   %ld bytes\n", (long)st.st_size);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "  [ERROR] open: %s\n", strerror(errno));
        return 0;
    }

    size_t map_size = (size_t)st.st_size;
    if (map_size < sizeof(dep_diag_header_t)) {
        fprintf(stderr, "  [ERROR] file too small for header (%zu < %zu)\n",
                map_size, sizeof(dep_diag_header_t));
        close(fd);
        return 0;
    }

    void* map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "  [ERROR] mmap: %s\n", strerror(errno));
        return 0;
    }

    dep_diag_header_t* h = (dep_diag_header_t*)map;
    printf("  magic:       0x%08X %s\n", h->magic,
           h->magic == DEP_HEADER_MAGIC ? "(OK 'PRST')" : "(MISMATCH!)");
    printf("  encoding:    %u %s\n", h->encoding,
           h->encoding == DEP_ENCODING_PCM32 ? "(PCM32 OK)" : "(unexpected)");
    printf("  sample_rate: %u Hz\n", h->sample_rate);
    printf("  samples/ch:  %u\n", h->samples_per_channel);
    printf("  samples/period: %u\n", h->samples_per_period);
    printf("  period_count: %llu\n", (unsigned long long)h->period_count);
    printf("  tx_channels: %u\n", h->tx_channels);
    printf("  rx_channels: %u\n", h->rx_channels);
    printf("  tx_offset:   %u\n", h->tx_offset);
    printf("  rx_offset:   %u\n", h->rx_offset);
    printf("  timing_offset: %u\n", h->timing_offset);
    printf("  reset_serial: %u (even=%s)\n", h->reset_serial,
           (h->reset_serial & 1) ? "RESET IN PROGRESS" : "stable");
    printf("  flags:       0x%08X%s\n", h->flags,
           (h->flags & 1) ? " (SEPARATE layout)" : "");
    printf("  object_bytes: %u\n", h->object_bytes);
    printf("  metadata_bytes: %u\n", h->metadata_bytes);
    printf("  epoch:       %u.%09u\n", h->epoch_seconds, h->epoch_samples);
    printf("  drift_ppb:   %d\n", (int)h->drift_ppb);
    printf("  monotonic_ns: %llu\n", (unsigned long long)h->monotonic_ns);

    /* Validation summary */
    int valid = 1;
    if (h->magic != DEP_HEADER_MAGIC) {
        fprintf(stderr, "  [FAIL] magic mismatch (expected 0x%08X)\n", DEP_HEADER_MAGIC);
        valid = 0;
    }
    if (h->encoding != DEP_ENCODING_PCM32) {
        fprintf(stderr, "  [FAIL] encoding not PCM32 (%u)\n", h->encoding);
        valid = 0;
    }
    if (h->sample_rate == 0) {
        fprintf(stderr, "  [FAIL] sample_rate is 0\n");
        valid = 0;
    }
    if (h->samples_per_period == 0) {
        fprintf(stderr, "  [FAIL] samples_per_period is 0\n");
        valid = 0;
    }
    if (h->reset_serial & 1) {
        fprintf(stderr, "  [WARN] reset_serial is odd (daemon is resetting)\n");
    }
    if (valid)
        printf("  [OK] Header validation passed.\n");
    else
        printf("  [FAIL] Header validation FAILED — context_connect() will reject this.\n");

    /* Read period_count again to see if it's advancing */
    uint64_t pc1 = h->period_count;
    munmap(map, map_size);

    /* Brief re-read to check if period_count is advancing */
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        void* map2 = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (map2 != MAP_FAILED) {
            dep_diag_header_t* h2 = (dep_diag_header_t*)map2;
            uint64_t pc2 = h2->period_count;
            printf("  period_count: %llu → %llu (%s)\n",
                   (unsigned long long)pc1, (unsigned long long)pc2,
                   pc2 > pc1 ? "ADVANCING" : "STATIC (no progress)");
            munmap(map2, map_size);
        }
    }

    return valid;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Main                                                                    */
/* ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char** argv)
{
    const char* dep_shm_name = argc > 1 ? argv[1] : "DanteEP";
    uint32_t sample_rate     = 48000;
    uint32_t channels        = 2;
    uint32_t samples_per_buf = 1024;

    printf("=== Dante Audio TX Real-Hardware Test ===\n");
    printf("DEP shm:      %s\n", dep_shm_name);
    printf("Audio:        %u Hz, %u ch, %u samples/buf, S32LE\n",
           sample_rate, channels, samples_per_buf);
    printf("Ctrl+C to stop.\n");
    printf("========================================\n\n");

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── Step 0: Inspect DEP shared memory directly ──────────────────── */
    inspect_dep_shm(dep_shm_name);
    printf("\n");

    /* ── Step 1: Create elements ─────────────────────────────────────── */
    printf("[STEP 1] Creating elements...\n");
    zst_pipeline_t*  pipeline  = zst_pipeline_create();
    zst_element_t*   audio_src = zst_audio_test_src_create();
    zst_element_t*   resampler = zst_audio_resampler_create(
                                     (int)sample_rate, (int)channels, "S32LE");
    zst_element_t*   dep_sink  = zst_dante_dep_audio_sink_create();
    zst_clock_t*     clock     = zst_clock_system_create();

    if (!pipeline || !audio_src || !resampler || !dep_sink || !clock) {
        fprintf(stderr, "[FATAL] element creation failed\n");
        return 1;
    }
    printf("  [OK] All elements created.\n");

    /* ── Step 2: Configure elements ──────────────────────────────────── */
    printf("[STEP 2] Configuring elements...\n");

    set_uint(audio_src, "sample-rate",        sample_rate);
    set_uint(audio_src, "channels",           channels);
    set_str(audio_src, "sample-format",       "S32LE");
    set_uint(audio_src, "samples-per-buffer", samples_per_buf);
    set_str(audio_src, "use-clock",           "true");
    set_str(audio_src, "real-time-pacing",    "true");

    set_str(dep_sink, "shm-name", dep_shm_name);
    {
        char ch_str[64];
        snprintf(ch_str, sizeof(ch_str), "0,1");
        set_str(dep_sink, "channels", ch_str);
    }
    {
        char rate_str[32];
        snprintf(rate_str, sizeof(rate_str), "%u", sample_rate);
        set_str(dep_sink, "expected-sample-rate", rate_str);
    }

    zst_element_set_clock(audio_src, clock);
    printf("  [OK] Configuration done.\n");

    /* ── Step 3: Add elements to pipeline ────────────────────────────── */
    printf("[STEP 3] Adding elements to pipeline...\n");
    if (zst_pipeline_add(pipeline, audio_src) != ZST_OK ||
        zst_pipeline_add(pipeline, resampler) != ZST_OK ||
        zst_pipeline_add(pipeline, dep_sink)  != ZST_OK) {
        fprintf(stderr, "[FATAL] pipeline add failed\n");
        return 1;
    }
    printf("  [OK] %u elements added.\n", pipeline->nb_elements);

    /* ── Step 4: Link pads ───────────────────────────────────────────── */
    printf("[STEP 4] Linking pads...\n");

    zst_pad_t* src_pad  = zst_element_get_pad(audio_src, "src");
    zst_pad_t* res_sink = zst_element_get_pad(resampler, "sink");
    zst_pad_t* res_src  = zst_element_get_pad(resampler, "src");
    zst_pad_t* dep_pad  = zst_element_get_pad(dep_sink, "sink");

    if (!src_pad || !res_sink || !res_src || !dep_pad) {
        fprintf(stderr, "[FATAL] missing pad (src=%p res_sink=%p "
                "res_src=%p dep=%p)\n",
                (void*)src_pad, (void*)res_sink, (void*)res_src, (void*)dep_pad);
        return 1;
    }

    if (zst_pad_link(src_pad, res_sink) != ZST_OK) {
        fprintf(stderr, "[FATAL] link audio_src -> resampler failed\n");
        return 1;
    }
    if (zst_pad_link(res_src, dep_pad) != ZST_OK) {
        fprintf(stderr, "[FATAL] link resampler -> dep_sink failed\n");
        return 1;
    }
    printf("  [OK] audio_src -> resampler -> dep_sink linked.\n");

    /* ── Step 5: Start pipeline ──────────────────────────────────────── */
    printf("[STEP 5] Setting pipeline to PLAYING...\n");
    zst_pipeline_set_clock(pipeline, clock);
    zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING);

    printf("  Pipeline element states:\n");
    print_pipeline_states(pipeline);
    printf("\n");

    /* ── Step 6: DEP sink pre-scheduler status ───────────────────────── */
    printf("[STEP 6] DEP sink status (before scheduler):\n");
    print_dep_status(dep_sink);

    /* Re-inspect SHM header after pipeline open/start */
    printf("\n  Re-inspecting DEP SHM after pipeline start:\n");
    inspect_dep_shm(dep_shm_name);
    printf("\n");

    /* ── Step 7: Create and run scheduler ────────────────────────────── */
    printf("[STEP 7] Starting scheduler...\n");
    zst_scheduler_config_t sched_cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD };
    g_scheduler = zst_scheduler_create(&sched_cfg);
    if (!g_scheduler) {
        fprintf(stderr, "[FATAL] scheduler create failed\n");
        return 1;
    }
    zst_scheduler_attach(g_scheduler, pipeline);

    pthread_t sched_thread;
    pthread_create(&sched_thread, NULL, (void* (*)(void*))zst_scheduler_run, g_scheduler);
    printf("  [OK] Scheduler thread started.\n\n");

    /* ── Status polling loop ─────────────────────────────────────────── */
    uint64_t start_ms = monotonic_ms();
    uint64_t last_status = start_ms;
    uint64_t last_period_count = 0;

    while (!g_stop) {
        usleep(500000); /* 500ms */

        uint64_t now = monotonic_ms();
        uint64_t elapsed_sec = (now - start_ms) / 1000;

        if (now - last_status >= 2000) {
            drain_bus(pipeline->bus);
            printf("[%lus]\n", (unsigned long)elapsed_sec);
            print_dep_status(dep_sink);

            /* Check if period_count is advancing */
            char val[64] = {0};
            if (zst_element_get_property(dep_sink, "period-count", val, sizeof(val)) == ZST_OK) {
                uint64_t pc = (uint64_t)strtoull(val, NULL, 10);
                if (pc > last_period_count) {
                    printf("    [OK] period_count advancing: %llu -> %llu\n",
                           (unsigned long long)last_period_count, (unsigned long long)pc);
                } else if (last_period_count > 0 || pc > 0) {
                    printf("    [WARN] period_count STUCK at %llu\n",
                           (unsigned long long)pc);
                }
                last_period_count = pc;
            }
            printf("\n");
            last_status = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────────────── */
    printf("\n[STOP] Shutting down...\n");
    zst_scheduler_stop(g_scheduler);
    pthread_join(sched_thread, NULL);
    printf("[STOP] Scheduler stopped.\n");

    zst_pipeline_set_state(pipeline, ZST_STATE_NULL);
    printf("[STOP] Pipeline set to NULL.\n");

    printf("[STOP] Final DEP status:\n");
    print_dep_status(dep_sink);

    zst_scheduler_destroy(g_scheduler);
    g_scheduler = NULL;
    zst_pipeline_destroy(pipeline);
    zst_clock_unref(clock);

    printf("[STOP] Done.\n");
    return 0;
}
