/*=============================================================================
    test_srt_perf.c — Automated unit/integration test for SRT client & server
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_plugin.h"
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

static zst_element_t* make_el(const char* name, zst_element_t* (*create_fn)(void))
{
    zst_element_t* el = zst_element_factory_make(name);
    if (!el && create_fn) {
        el = create_fn();
    }
    return el;
}

typedef struct {
    pthread_mutex_t lock;
    uint64_t v_enc_frames;
    uint64_t a_enc_packets;
    uint64_t srt_tx_bytes;
    uint64_t srt_rx_bytes;
    uint64_t v_dec_frames;
    uint64_t a_dec_frames;
} test_stats_t;

static zst_pad_probe_return_t probe_venc_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->v_enc_frames++;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t probe_aenc_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->a_enc_packets++;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t probe_srttx_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->srt_tx_bytes += buf->memory.size;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t probe_srtrx_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->srt_rx_bytes += buf->memory.size;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t probe_vdec_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->v_dec_frames++;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t probe_adec_cb(zst_pad_t* pad, zst_buffer_t* buf, zst_pad_probe_type_t type, void* udata)
{
    (void)pad; (void)type;
    test_stats_t* st = udata;
    if (buf) {
        pthread_mutex_lock(&st->lock);
        st->a_dec_frames++;
        pthread_mutex_unlock(&st->lock);
    }
    return ZST_PAD_PROBE_OK;
}

typedef struct {
    zst_pipeline_t* pipe;
    zst_element_t*  h264dec;
    zst_element_t*  aacdec;
    zst_element_t*  q_vdec;
    zst_element_t*  q_adec;
    bool v_linked;
    bool a_linked;
} client_test_ctx_t;

static void handle_events(zst_pipeline_t* pipe, client_test_ctx_t* ctx)
{
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    if (!bus) return;
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK && ev) {
        if (ev->type == ZST_EVENT_PAD_ADDED) {
            zst_pad_t* pad = ev->as.pad_added.pad;
            if (pad && pad->name) {
                if (strcmp(pad->name, "video_0") == 0 && !ctx->v_linked) {
                    zst_pipeline_reconfigure_begin(pipe);
                    zst_pipeline_link_pads_dynamic(pipe, pad, zst_element_get_pad(ctx->q_vdec, "sink"));
                    zst_pad_link(zst_element_get_pad(ctx->q_vdec, "src"), zst_element_get_pad(ctx->h264dec, "sink"));
                    zst_pipeline_reconfigure_end(pipe);
                    ctx->v_linked = true;
                } else if (strcmp(pad->name, "audio_0") == 0 && !ctx->a_linked) {
                    zst_pipeline_reconfigure_begin(pipe);
                    zst_pipeline_link_pads_dynamic(pipe, pad, zst_element_get_pad(ctx->q_adec, "sink"));
                    zst_pad_link(zst_element_get_pad(ctx->q_adec, "src"), zst_element_get_pad(ctx->aacdec, "sink"));
                    zst_pipeline_reconfigure_end(pipe);
                    ctx->a_linked = true;
                }
            }
        }
        zst_event_destroy(ev);
        ev = NULL;
    }
}

int main(void)
{
    printf("[test_srt_perf] Initializing SRT end-to-end performance test...\n");

    zst_plugin_registry_init();
    zst_plugin_registry_scan_env();

    test_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    pthread_mutex_init(&stats.lock, NULL);

    /* ── 1. Create Server Pipeline ── */
    zst_pipeline_t* srv_pipe = zst_pipeline_create();
    assert(srv_pipe != NULL);
    zst_scheduler_config_t sched_cfg = { .mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 4 };
    zst_scheduler_t* srv_sched = zst_scheduler_create(&sched_cfg);
    assert(srv_sched != NULL);

    zst_element_t* vsrc = make_el("videotestsrc", zst_video_test_src_create);
    zst_element_set_property_uint(vsrc, "width", 1920);
    zst_element_set_property_uint(vsrc, "height", 1080);
    zst_element_set_property_uint(vsrc, "fps", 30);
    zst_element_set_property_string(vsrc, "pattern", "bars");
    zst_element_set_property_string(vsrc, "pixel-format", "YUV420P");
    zst_element_set_property_bool(vsrc, "use-clock", false);

    zst_element_t* q_vsrc = zst_queue_element_create(NULL);
    zst_element_t* x264enc = make_el("x264enc", zst_x264_encoder_create);
    zst_element_set_property_int(x264enc, "bitrate", 4000000);
    zst_element_set_property_string(x264enc, "preset", "ultrafast");
    zst_element_set_property_string(x264enc, "tune", "zerolatency");
    zst_element_set_property_int(x264enc, "gop-size", 30);

    zst_element_t* q_venc = zst_queue_element_create(NULL);

    zst_element_t* asrc = make_el("audiotestsrc", zst_audio_test_src_create);
    zst_element_set_property_uint(asrc, "sample-rate", 48000);
    zst_element_set_property_uint(asrc, "channels", 2);
    zst_element_set_property_string(asrc, "sample-format", "S16LE");
    zst_element_set_property_double(asrc, "frequency", 1000.0);
    zst_element_set_property_bool(asrc, "use-clock", false);

    zst_element_t* q_asrc = zst_queue_element_create(NULL);
    zst_element_t* aacenc = make_el("aacenc", zst_aac_encoder_create);
    zst_element_set_property_int(aacenc, "bitrate", 128000);

    zst_element_t* q_aenc = zst_queue_element_create(NULL);

    zst_element_t* tsmux = make_el("tsmux", zst_mpegts_muxer_create);
    zst_element_set_property_uint(tsmux, "width", 1920);
    zst_element_set_property_uint(tsmux, "height", 1080);
    zst_element_set_property_uint(tsmux, "fps", 30);
    zst_element_set_property_uint(tsmux, "sample-rate", 48000);
    zst_element_set_property_uint(tsmux, "channels", 2);

    zst_element_t* q_mux = zst_queue_element_create(NULL);
    zst_element_t* srtsink = make_el("srtsink", zst_srt_sink_create);
    assert(srtsink != NULL);
    zst_element_set_property_string(srtsink, "uri", "srt://127.0.0.1:9899?mode=listener&latency=100");

    zst_pad_add_probe(zst_element_get_pad(x264enc, "src"), ZST_PAD_PROBE_PRE_BUFFER, probe_venc_cb, &stats);
    zst_pad_add_probe(zst_element_get_pad(aacenc, "src"), ZST_PAD_PROBE_PRE_BUFFER, probe_aenc_cb, &stats);
    zst_pad_add_probe(zst_element_get_pad(srtsink, "sink"), ZST_PAD_PROBE_PRE_BUFFER, probe_srttx_cb, &stats);

    zst_pipeline_add(srv_pipe, vsrc);
    zst_pipeline_add(srv_pipe, q_vsrc);
    zst_pipeline_add(srv_pipe, x264enc);
    zst_pipeline_add(srv_pipe, q_venc);
    zst_pipeline_add(srv_pipe, asrc);
    zst_pipeline_add(srv_pipe, q_asrc);
    zst_pipeline_add(srv_pipe, aacenc);
    zst_pipeline_add(srv_pipe, q_aenc);
    zst_pipeline_add(srv_pipe, tsmux);
    zst_pipeline_add(srv_pipe, q_mux);
    zst_pipeline_add(srv_pipe, srtsink);

    zst_pad_link(zst_element_get_pad(vsrc, "src"), zst_element_get_pad(q_vsrc, "sink"));
    zst_pad_link(zst_element_get_pad(q_vsrc, "src"), zst_element_get_pad(x264enc, "sink"));
    zst_pad_link(zst_element_get_pad(x264enc, "src"), zst_element_get_pad(q_venc, "sink"));
    zst_pad_link(zst_element_get_pad(q_venc, "src"), zst_element_get_pad(tsmux, "video"));

    zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(q_asrc, "sink"));
    zst_pad_link(zst_element_get_pad(q_asrc, "src"), zst_element_get_pad(aacenc, "sink"));
    zst_pad_link(zst_element_get_pad(aacenc, "src"), zst_element_get_pad(q_aenc, "sink"));
    zst_pad_link(zst_element_get_pad(q_aenc, "src"), zst_element_get_pad(tsmux, "audio"));

    zst_pad_link(zst_element_get_pad(tsmux, "src"), zst_element_get_pad(q_mux, "sink"));
    zst_pad_link(zst_element_get_pad(q_mux, "src"), zst_element_get_pad(srtsink, "sink"));

    zst_scheduler_attach(srv_sched, srv_pipe);
    assert(zst_pipeline_set_state(srv_pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(srv_sched) == ZST_OK);

    usleep(150000);

    /* ── 2. Create Client Pipeline ── */
    zst_pipeline_t* cli_pipe = zst_pipeline_create();
    assert(cli_pipe != NULL);
    zst_scheduler_t* cli_sched = zst_scheduler_create(&sched_cfg);
    assert(cli_sched != NULL);

    zst_element_t* srtsrc = make_el("srtsrc", zst_srt_source_create);
    assert(srtsrc != NULL);
    zst_element_set_property_string(srtsrc, "uri", "srt://127.0.0.1:9899?mode=caller&latency=100");

    zst_element_t* q_srtrx = zst_queue_element_create(NULL);
    zst_element_t* tsdemux = make_el("tsdemux", zst_mpegts_demuxer_create);

    zst_element_t* q_vdec = zst_queue_element_create(NULL);
    zst_element_t* h264dec = make_el("h264dec", zst_h264_decoder_create);
    zst_element_t* q_vdout = zst_queue_element_create(NULL);
    zst_element_t* vsink  = make_el("fakesink", zst_fake_sink_create);

    zst_element_t* q_adec = zst_queue_element_create(NULL);
    zst_element_t* aacdec = make_el("aacdec", zst_aac_decoder_create);
    zst_element_t* q_adout = zst_queue_element_create(NULL);
    zst_element_t* asink  = make_el("fakesink", zst_fake_sink_create);

    zst_pad_add_probe(zst_element_get_pad(srtsrc, "src"), ZST_PAD_PROBE_PRE_BUFFER, probe_srtrx_cb, &stats);
    zst_pad_add_probe(zst_element_get_pad(h264dec, "src"), ZST_PAD_PROBE_PRE_BUFFER, probe_vdec_cb, &stats);
    zst_pad_add_probe(zst_element_get_pad(aacdec, "src"), ZST_PAD_PROBE_PRE_BUFFER, probe_adec_cb, &stats);

    zst_pipeline_add(cli_pipe, srtsrc);
    zst_pipeline_add(cli_pipe, q_srtrx);
    zst_pipeline_add(cli_pipe, tsdemux);
    zst_pipeline_add(cli_pipe, q_vdec);
    zst_pipeline_add(cli_pipe, h264dec);
    zst_pipeline_add(cli_pipe, q_vdout);
    zst_pipeline_add(cli_pipe, vsink);
    zst_pipeline_add(cli_pipe, q_adec);
    zst_pipeline_add(cli_pipe, aacdec);
    zst_pipeline_add(cli_pipe, q_adout);
    zst_pipeline_add(cli_pipe, asink);

    zst_pad_link(zst_element_get_pad(srtsrc, "src"), zst_element_get_pad(q_srtrx, "sink"));
    zst_pad_link(zst_element_get_pad(q_srtrx, "src"), zst_element_get_pad(tsdemux, "sink"));
    zst_pad_link(zst_element_get_pad(h264dec, "src"), zst_element_get_pad(q_vdout, "sink"));
    zst_pad_link(zst_element_get_pad(q_vdout, "src"), zst_element_get_pad(vsink, "sink"));
    zst_pad_link(zst_element_get_pad(aacdec, "src"), zst_element_get_pad(q_adout, "sink"));
    zst_pad_link(zst_element_get_pad(q_adout, "src"), zst_element_get_pad(asink, "sink"));

    client_test_ctx_t ctx = {
        .pipe = cli_pipe,
        .h264dec = h264dec,
        .aacdec = aacdec,
        .q_vdec = q_vdec,
        .q_adec = q_adec,
        .v_linked = false,
        .a_linked = false
    };

    zst_scheduler_attach(cli_sched, cli_pipe);
    assert(zst_pipeline_set_state(cli_pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(cli_sched) == ZST_OK);

    /* ── 3. Run streaming for 3 seconds ── */
    printf("[test_srt_perf] Streaming video & audio over SRT for 3 seconds...\n");
    for (int i = 0; i < 60; i++) {
        usleep(50000);
        handle_events(cli_pipe, &ctx);
    }

    pthread_mutex_lock(&stats.lock);
    printf("[test_srt_perf] Video Encoded Frames : %" PRIu64 "\n", stats.v_enc_frames);
    printf("[test_srt_perf] Audio Encoded Packets: %" PRIu64 "\n", stats.a_enc_packets);
    printf("[test_srt_perf] SRT Sent Bytes      : %" PRIu64 "\n", stats.srt_tx_bytes);
    printf("[test_srt_perf] SRT Received Bytes  : %" PRIu64 "\n", stats.srt_rx_bytes);
    printf("[test_srt_perf] Video Decoded Frames : %" PRIu64 "\n", stats.v_dec_frames);
    printf("[test_srt_perf] Audio Decoded Frames : %" PRIu64 "\n", stats.a_dec_frames);

    assert(stats.v_enc_frames > 0);
    assert(stats.a_enc_packets > 0);
    assert(stats.srt_tx_bytes > 0);
    assert(stats.srt_rx_bytes > 0);
    pthread_mutex_unlock(&stats.lock);

    /* ── 4. Clean Shutdown ── */
    zst_scheduler_stop(cli_sched);
    zst_pipeline_set_state(cli_pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(cli_sched);
    zst_pipeline_destroy(cli_pipe);

    zst_scheduler_stop(srv_sched);
    zst_pipeline_set_state(srv_pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(srv_sched);
    zst_pipeline_destroy(srv_pipe);

    pthread_mutex_destroy(&stats.lock);
    zst_plugin_registry_deinit();

    printf("[test_srt_perf] TEST PASSED SUCCESSFULLY!\n");
    return 0;
}
