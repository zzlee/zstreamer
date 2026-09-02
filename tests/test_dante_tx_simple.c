/**
 * @file test_dante_tx_simple.c
 * @brief Standalone DEP TX test — modeled after qcap-demos/sc6f0-dante-tx.
 *
 * Connects to DVR socket, sends start with 1 TX video channel,
 * handles DVR flow messages, generates test video+audio, sends via DEP SHM.
 *
 * Usage:
 *   ./test_dante_tx_simple          # run forever (Ctrl+C to stop)
 *   ./test_dante_tx_simple --no-video  # audio-only (no DVR video channels)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <poll.h>
#include <math.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_clock.h"

/* ── DEP audio include ─────────────────────────────────────────────── */
#include "dep_audio.h"

/* ── Forward declarations for zstreamer elements ───────────────────── */
extern zst_element_t* zst_audio_test_src_create(void);
extern zst_element_t* zst_dante_dep_audio_sink_create(void);

/* ── Globals ────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ── DVR socket helpers (simplified from Audinate reference) ────────── */
#define DVR_SOCKET_PATH "/var/run/dante/dvr"

static int dvr_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, DVR_SOCKET_PATH, sizeof(DVR_SOCKET_PATH));

    int retries = 20;
    while (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (--retries <= 0) { perror("connect dvr"); close(fd); return -1; }
        fprintf(stderr, "[DVR] retrying connect...\n");
        sleep(1);
    }
    fprintf(stderr, "[DVR] connected to %s\n", DVR_SOCKET_PATH);
    return fd;
}

static int dvr_send_start(int fd, int n_tx_video, int n_rx_video)
{
    char msg[2048];
    int off = 0;
    off += snprintf(msg + off, sizeof(msg) - off,
        "{\"action\":\"start\",\"parameters\":{\"txVideoChannels\":[");
    for (int i = 0; i < n_tx_video; i++) {
        if (i > 0) off += snprintf(msg + off, sizeof(msg) - off, ",");
        off += snprintf(msg + off, sizeof(msg) - off,
            "{\"subtypes\":[\"H264\"]}");
    }
    off += snprintf(msg + off, sizeof(msg) - off, "],\"rxVideoChannels\":[");
    for (int i = 0; i < n_rx_video; i++) {
        if (i > 0) off += snprintf(msg + off, sizeof(msg) - off, ",");
        off += snprintf(msg + off, sizeof(msg) - off,
            "{\"subtypes\":[\"H264\"]}");
    }
    off += snprintf(msg + off, sizeof(msg) - off, "]}}");
    ssize_t sent = write(fd, msg, off + 1);
    fprintf(stderr, "[DVR TX] start (%d bytes): %s\n", off, msg);
    return (sent > 0) ? 0 : -1;
}

static int dvr_send_stop(int fd)
{
    const char* msg = "{\"action\":\"stop\",\"parameters\":{}}";
    ssize_t sent = write(fd, msg, strlen(msg) + 1);
    fprintf(stderr, "[DVR TX] stop\n");
    return (sent > 0) ? 0 : -1;
}

/* ── DVR receive thread ────────────────────────────────────────────── */

typedef struct {
    int fd;
    int n_tx_video;
    int n_rx_video;
    volatile int flow_active;
    int flow_index;
    int flow_port;
    char flow_ip[64];
} dvr_context_t;

static void* dvr_rx_thread(void* arg)
{
    dvr_context_t* ctx = (dvr_context_t*)arg;
    char buf[4096];

    while (!g_stop) {
        struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 200);
        if (ready <= 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        ssize_t n = read(ctx->fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';

        /* Simple string search for action type (no json-c dependency) */
        if (strstr(buf, "requestConfiguration")) {
            fprintf(stderr, "[DVR RX] requestConfiguration (skip)\n");
        } else if (strstr(buf, "createVideoUnicastTxFlow")) {
            fprintf(stderr, "[DVR RX] createVideoUnicastTxFlow\n");
            ctx->flow_active = 1;
        } else if (strstr(buf, "createVideoMulticastTxFlow")) {
            fprintf(stderr, "[DVR RX] createVideoMulticastTxFlow\n");
            ctx->flow_active = 1;
        } else if (strstr(buf, "deleteTxFlow")) {
            fprintf(stderr, "[DVR RX] deleteTxFlow\n");
            ctx->flow_active = 0;
        } else if (strstr(buf, "createVideoUnicastRxFlow")) {
            fprintf(stderr, "[DVR RX] createVideoUnicastRxFlow\n");
        } else if (strstr(buf, "deleteRxFlow")) {
            fprintf(stderr, "[DVR RX] deleteRxFlow\n");
        } else {
            /* Log raw message for debugging */
            fprintf(stderr, "[DVR RX] (%zd bytes) %.200s\n", n, buf);
        }
    }
    fprintf(stderr, "[DVR RX] thread exiting\n");
    return NULL;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
    int no_video = 0;
    int n_tx_video = 1;
    int n_rx_video = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-video") == 0) { no_video = 1; n_tx_video = 0; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "=== Dante TX Simple Test ===\n");
    fprintf(stderr, "DVR: %s\n", DVR_SOCKET_PATH);
    fprintf(stderr, "Video: %s (%d TX channels)\n", no_video ? "off" : "640x480@30 H264", n_tx_video);
    fprintf(stderr, "Audio: 48000Hz 2ch S32LE (440Hz sine)\n");
    fprintf(stderr, "Ctrl+C to stop.\n");
    fprintf(stderr, "============================\n\n");

    /* ── Step 1: Connect DVR socket ─────────────────────────────────── */
    int dvr_fd = -1;
    dvr_context_t dvr_ctx;
    memset(&dvr_ctx, 0, sizeof(dvr_ctx));
    dvr_ctx.n_tx_video = n_tx_video;
    dvr_ctx.n_rx_video = n_rx_video;

    if (!no_video || 1) { /* Always connect DVR to stabilize apec */
        dvr_fd = dvr_connect();
        if (dvr_fd < 0) {
            fprintf(stderr, "[WARN] DVR connection failed, continuing without video flow\n");
        } else {
            if (dvr_send_start(dvr_fd, n_tx_video, n_rx_video) < 0) {
                fprintf(stderr, "[WARN] DVR send start failed\n");
                close(dvr_fd);
                dvr_fd = -1;
            }
            dvr_ctx.fd = dvr_fd;
        }
    }

    /* ── Step 2: Start DVR receive thread ───────────────────────────── */
    pthread_t dvr_tid;
    if (dvr_fd >= 0) {
        pthread_create(&dvr_tid, NULL, dvr_rx_thread, &dvr_ctx);
    }

    /* ── Step 3: Create zstreamer pipeline ──────────────────────────── */
    zst_pipeline_t* pipeline = zst_pipeline_create();
    zst_clock_t* clock = zst_clock_system_create();
    zst_pipeline_set_clock(pipeline, clock);

    /* Audio: audiotestsrc → dantedepaudiosink */
    zst_element_t* audio_src = zst_audio_test_src_create();
    zst_element_t* dep_sink = zst_dante_dep_audio_sink_create();

    /* Configure elements */
    zst_element_set_property(audio_src, "sample-rate", "48000");
    zst_element_set_property(audio_src, "channels", "2");
    zst_element_set_property(audio_src, "samples-per-buffer", "4096");
    zst_element_set_property(audio_src, "waveform", "sine");
    zst_element_set_property(audio_src, "frequency", "440.0");
    zst_element_set_property(audio_src, "volume", "0.8");

    zst_element_set_property(dep_sink, "shm-name", "DanteEP");
    zst_element_set_property(dep_sink, "channels", "0,1");
    zst_element_set_property(dep_sink, "expected-sample-rate", "48000");
    zst_element_set_property(dep_sink, "queue-periods", "4");
    zst_element_set_property(dep_sink, "reconnect-interval-ms", "100");

    /* Add to pipeline — audio is pushed manually (dep_sink writes to SHM) */
    zst_pipeline_add(pipeline, audio_src);
    zst_pipeline_add(pipeline, dep_sink);
    fprintf(stderr, "[OK] Elements added to pipeline\n");
    fflush(stderr);

    /* ── Step 4: Set state to PLAYING ───────────────────────────────── */
    zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING);
    fprintf(stderr, "[OK] Pipeline PLAYING\n\n");

    /* ── Step 5: Create scheduler and start ─────────────────────────── */
    zst_scheduler_config_t sched_cfg;
    memset(&sched_cfg, 0, sizeof(sched_cfg));
    sched_cfg.mode = ZST_SCHEDULER_SINGLE_THREAD;
    sched_cfg.worker_threads = 0;
    zst_scheduler_t* scheduler = zst_scheduler_create(&sched_cfg);
    zst_scheduler_attach(scheduler, pipeline);
    zst_scheduler_run(scheduler);

    fprintf(stderr, "[OK] Scheduler running\n\n");

    /* ── Step 6: Monitor loop ───────────────────────────────────────── */
    uint64_t last_active = 0;
    uint32_t interval_ms = 2000;
    uint32_t elapsed_ms = 0;

    while (!g_stop) {
        /* Manual audio push: audiotestsrc -> dep_sink (like demo_dante_av_tx.c) */
        {
            zst_buffer_t* audio = NULL;
            if (audio_src->ops->process(audio_src, NULL, &audio) == ZST_OK && audio) {
                (void)dep_sink->ops->process(dep_sink, audio, NULL);
                zst_buffer_unref(audio);
            }
        }
        usleep(interval_ms * 1000);
        elapsed_ms += interval_ms;

        /* Read DEP sink stats */
        char buf_periods[32] = "0", buf_overruns[32] = "0", buf_underruns[32] = "0", buf_active[32] = "0";
        zst_element_get_property(dep_sink, "period-count", buf_periods, sizeof(buf_periods));
        zst_element_get_property(dep_sink, "overrun-count", buf_overruns, sizeof(buf_overruns));
        zst_element_get_property(dep_sink, "underflow-count", buf_underruns, sizeof(buf_underruns));
        zst_element_get_property(dep_sink, "active", buf_active, sizeof(buf_active));

        fprintf(stderr, "[%ums] DEP: periods=%s active=%s overruns=%s underruns=%s  DVR: flow=%s\n",
                elapsed_ms, buf_periods, buf_active, buf_overruns, buf_underruns,
                dvr_ctx.flow_active ? "ACTIVE" : "none");
    }

cleanup:
    fprintf(stderr, "\n[STOP] Shutting down...\n");

    /* Stop scheduler */
    zst_scheduler_stop(scheduler);
    zst_scheduler_destroy(scheduler);

    /* Stop pipeline */
    zst_pipeline_set_state(pipeline, ZST_STATE_NULL);
    fprintf(stderr, "[STOP] Pipeline set to NULL\n");

    /* Final stats */
    char fin_periods[32] = "0", fin_overruns[32] = "0", fin_underruns[32] = "0";
    zst_element_get_property(dep_sink, "period-count", fin_periods, sizeof(fin_periods));
    zst_element_get_property(dep_sink, "overrun-count", fin_overruns, sizeof(fin_overruns));
    zst_element_get_property(dep_sink, "underflow-count", fin_underruns, sizeof(fin_underruns));
    fprintf(stderr, "[STOP] DEP final: periods=%s overruns=%s underruns=%s\n",
            fin_periods, fin_overruns, fin_underruns);

    /* Disconnect DVR */
    if (dvr_fd >= 0) {
        dvr_send_stop(dvr_fd);
        close(dvr_fd);
        pthread_join(dvr_tid, NULL);
    }

    /* Destroy pipeline */
    zst_pipeline_destroy(pipeline);

    fprintf(stderr, "[STOP] Done.\n");
    return 0;
}
