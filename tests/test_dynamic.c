#include "zst_pipeline.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zst_stream.h"
#include "zst_scheduler.h"
#include "zst_clock.h"
#include "zst_buffer.h"
#include "zst_plugin.h"
#include "zst_caps.h"
#include "zst_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

static int g_pad_added_count = 0;
static int g_pad_removed_count = 0;
static int g_stream_added_count = 0;
static int g_stream_removed_count = 0;
static int g_sink_event_count = 0;

static void mock_buf_destroy(zst_buffer_t* b) {
    if (b) {
        free(b->payload);
        b->payload = NULL;
    }
}


static void on_bus_event(zst_bus_t* bus, zst_event_t* ev, void* user_data)
{
    (void)bus; (void)user_data;
    if (ev->type == ZST_EVENT_PAD_ADDED) {
        g_pad_added_count++;
        printf("Bus: Pad added: %s\n", ev->as.pad_added.pad->name);
    } else if (ev->type == ZST_EVENT_PAD_REMOVED) {
        g_pad_removed_count++;
        printf("Bus: Pad removed: %s\n", ev->as.pad_removed.pad->name);
    } else if (ev->type == ZST_EVENT_STREAM_ADDED) {
        g_stream_added_count++;
        assert(ev->as.stream_status.stream.id == 100);
    } else if (ev->type == ZST_EVENT_STREAM_REMOVED) {
        g_stream_removed_count++;
        assert(ev->as.stream_removed.stream_id == 100);
    }
}

static zst_result_t sink_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    (void)el; (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_STREAM_START) {
        assert(event->as.stream_start.stream_id == 100);
        g_sink_event_count++;
    }
    return ZST_OK;
}

static zst_element_ops_t g_test_ops = {.name = "test"};
static zst_element_ops_t g_sink_ops = {.name = "sink", .event = sink_event};

static void test_dynamic_basics(void)
{
    g_pad_added_count = 0;
    g_pad_removed_count = 0;
    g_stream_added_count = 0;
    g_stream_removed_count = 0;
    g_sink_event_count = 0;

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);

    zst_element_t* el = zst_element_create(&g_test_ops, NULL);
    zst_pipeline_add(pipe, el);

    zst_pad_t* pad = zst_pad_create("src_0", ZST_PAD_SRC);
    zst_stream_info_t info = {
        .struct_size = sizeof(info),
        .id = 100,
        .kind = ZST_MEDIA_VIDEO,
        .name = "Test Stream"
    };

    zst_element_add_dynamic_pad(el, pad, &info);
    assert(el->nb_src_pads == 1);
    assert(zst_element_get_stream_count(el) == 1);

    zst_pad_t** snapshot = NULL;
    uint32_t count = 0;
    zst_element_snapshot_src_pads(el, &snapshot, &count);
    assert(count == 1);
    assert(snapshot[0] == pad);
    zst_element_pad_snapshot_free(snapshot, count);

    zst_stream_info_t query_info;
    assert(zst_element_get_stream_info(el, 0, &query_info) == ZST_OK);
    assert(query_info.id == 100);
    assert(query_info.kind == ZST_MEDIA_VIDEO);
    assert(strcmp(query_info.name, "Test Stream") == 0);
    zst_stream_info_clear(&query_info);
    assert(zst_element_get_stream_pad(el, 100) == pad);

    zst_pad_event_t* stream_start = zst_pad_event_new_stream_start(100);
    assert(zst_pad_push_event(pad, stream_start) == ZST_OK);
    zst_pad_event_unref(stream_start);

    zst_element_t* sink_el = zst_element_create(&g_sink_ops, NULL);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink_el, sink_pad);
    zst_pipeline_add(pipe, sink_el);
    assert(zst_pad_link(pad, sink_pad) == ZST_OK);
    assert(g_sink_event_count == 1);

    zst_element_remove_dynamic_pad(el, pad);
    assert(sink_pad->peer == NULL);
    assert(el->nb_src_pads == 0);

    /* Process bus events */
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        on_bus_event(bus, ev, NULL);
        zst_event_destroy(ev);
    }

    assert(g_pad_added_count == 1);
    assert(g_pad_removed_count == 1);
    assert(g_stream_added_count == 1);
    assert(g_stream_removed_count == 1);

    zst_pipeline_destroy(pipe);
    printf("test_dynamic_basics passed!\n");
}

static void test_pad_removed_playing(void)
{
    g_pad_removed_count = 0;

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);

    zst_element_t* src_el = zst_element_create(&g_test_ops, NULL);
    zst_element_t* sink_el = zst_element_create(&g_sink_ops, NULL);
    zst_pipeline_add(pipe, src_el);
    zst_pipeline_add(pipe, sink_el);

    zst_pad_t* src_pad = zst_pad_create("src_playing_0", ZST_PAD_SRC);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink_el, sink_pad);

    zst_stream_info_t info = {
        .struct_size = sizeof(info),
        .id = 100,
        .kind = ZST_MEDIA_VIDEO,
        .name = "Test Stream"
    };
    zst_element_add_dynamic_pad(src_el, src_pad, &info);
    assert(zst_pad_link(src_pad, sink_pad) == ZST_OK);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);

    // Remove pad while PLAYING
    assert(zst_element_remove_dynamic_pad(src_el, src_pad) == ZST_OK);
    assert(!zst_pad_is_linked(sink_pad));

    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_PAD_REMOVED) {
            g_pad_removed_count++;
        }
        zst_event_destroy(ev);
    }
    assert(g_pad_removed_count == 1);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_pipeline_destroy(pipe);
    printf("test_pad_removed_playing passed!\n");
}

static zst_result_t dummy_sink_event_caps(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    (void)el; (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_CAPS) {
        g_sink_event_count++;
        assert(event->as.caps.caps != NULL);
        assert(strcmp(event->as.caps.caps->structs->media_type, "video/x-h264") == 0);
    }
    return ZST_OK;
}

static zst_element_ops_t g_caps_sink_ops = {.name = "caps_sink", .event = dummy_sink_event_caps};

static void test_caps_changed_playing(void)
{
    g_sink_event_count = 0;

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);

    zst_element_t* src_el = zst_element_create(&g_test_ops, NULL);
    zst_element_t* sink_el = zst_element_create(&g_caps_sink_ops, NULL);
    zst_pipeline_add(pipe, src_el);
    zst_pipeline_add(pipe, sink_el);

    zst_pad_t* src_pad = zst_pad_create("src_0", ZST_PAD_SRC);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink_el, sink_pad);

    zst_stream_info_t info = {
        .struct_size = sizeof(info),
        .id = 102,
        .kind = ZST_MEDIA_VIDEO,
        .name = "Caps Change Stream"
    };
    zst_element_add_dynamic_pad(src_el, src_pad, &info);
    assert(zst_pad_link(src_pad, sink_pad) == ZST_OK);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);

    zst_caps_t* old_caps = zst_caps_new_simple("video/x-raw");
    zst_caps_t* new_caps = zst_caps_new_simple("video/x-h264");
    zst_pad_set_caps(src_pad, old_caps);

    // Push new caps & trigger bus events
    zst_pad_set_caps(src_pad, new_caps);
    zst_pad_event_t* ce = zst_pad_event_new_caps(new_caps);
    assert(zst_pad_push_event(src_pad, ce) == ZST_OK);
    zst_pad_event_unref(ce);

    zst_event_t* caps_ev = zst_event_new_caps_changed(src_el, src_pad, old_caps, new_caps);
    zst_bus_post(bus, caps_ev);

    int caps_changed_seen = 0;
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_CAPS_CHANGED) {
            assert(ev->as.caps_changed.pad == src_pad);
            assert(strcmp(ev->as.caps_changed.new_caps->structs->media_type, "video/x-h264") == 0);
            caps_changed_seen++;
        }
        zst_event_destroy(ev);
    }
    assert(caps_changed_seen == 1);
    assert(g_sink_event_count == 1);

    zst_caps_destroy(old_caps);
    zst_caps_destroy(new_caps);
    zst_element_remove_dynamic_pad(src_el, src_pad);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_pipeline_destroy(pipe);
    printf("test_caps_changed_playing passed!\n");
}

static void test_signal_lost_present(void)
{
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    zst_element_t* src_el = zst_element_create(&g_test_ops, NULL);
    zst_pipeline_add(pipe, src_el);

    zst_event_t* lost_ev = zst_event_new_signal_lost(src_el);
    zst_event_t* present_ev = zst_event_new_signal_present(src_el);

    assert(lost_ev != NULL);
    assert(present_ev != NULL);

    assert(zst_bus_post(bus, lost_ev) == ZST_OK);
    assert(zst_bus_post(bus, present_ev) == ZST_OK);

    int lost_seen = 0;
    int present_seen = 0;
    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_SIGNAL_LOST) {
            assert(ev->src == src_el);
            lost_seen++;
        } else if (ev->type == ZST_EVENT_SIGNAL_PRESENT) {
            assert(ev->src == src_el);
            present_seen++;
        }
        zst_event_destroy(ev);
    }
    assert(lost_seen == 1);
    assert(present_seen == 1);

    zst_pipeline_destroy(pipe);
    printf("test_signal_lost_present passed!\n");
}

static void test_unlinked_policies(void)
{
    zst_pad_t* pad = zst_pad_create("src_0", ZST_PAD_SRC);
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    buf->payload = malloc(4);
    buf->destroy = mock_buf_destroy;


    // 1. ZST_PAD_UNLINKED_ERROR (default)
    assert(zst_pad_push(pad, buf) == ZST_ERROR);

    // 2. ZST_PAD_UNLINKED_DROP
    assert(zst_pad_set_unlinked_policy(pad, ZST_PAD_UNLINKED_DROP, 0) == ZST_OK);
    assert(zst_pad_push(pad, buf) == ZST_OK);

    // 3. ZST_PAD_UNLINKED_QUEUE
    assert(zst_pad_set_unlinked_policy(pad, ZST_PAD_UNLINKED_QUEUE, 2) == ZST_OK);
    assert(zst_pad_push(pad, buf) == ZST_OK);
    assert(pad->queued_count == 1);
    assert(zst_pad_push(pad, buf) == ZST_OK);
    assert(pad->queued_count == 2);
    assert(zst_pad_push(pad, buf) == ZST_ERROR); // full

    // 4. ZST_PAD_UNLINKED_BLOCK (requires peer, otherwise times out)
    assert(zst_pad_set_unlinked_policy(pad, ZST_PAD_UNLINKED_BLOCK, 0) == ZST_OK);
    assert(zst_pad_push(pad, buf) == ZST_TIMEOUT);

    zst_buffer_unref(buf);
    zst_pad_unref(pad);
    printf("test_unlinked_policies passed!\n");
}

static void write_mpegts_file(const char* filepath, int include_audio)
{
    unlink(filepath);
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);

    zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
    zst_element_set_property_int(vsrc, "width", 64);
    zst_element_set_property_int(vsrc, "height", 48);
    zst_element_set_property_int(vsrc, "fps", 30);
    zst_element_set_property_int(vsrc, "num-buffers", 50); // 1.6s
    zst_element_set_property_bool(vsrc, "use-clock", false);

    zst_element_t* venc = zst_element_factory_make("x264enc");
    zst_element_t* mux = zst_element_factory_make("tsmux");
    zst_element_t* sink = zst_element_factory_make("filesink");
    zst_element_set_property_string(sink, "location", filepath);

    zst_pipeline_add(pipe, vsrc);
    zst_pipeline_add(pipe, venc);
    zst_pipeline_add(pipe, mux);
    zst_pipeline_add(pipe, sink);

    zst_pad_t* vsrc_pad = zst_element_get_pad(vsrc, "src");
    zst_pad_t* venc_sink = zst_element_get_pad(venc, "sink");
    zst_pad_t* venc_src = zst_element_get_pad(venc, "src");
    zst_pad_t* mux_video = zst_element_get_pad(mux, "video");

    assert(zst_pad_link(vsrc_pad, venc_sink) == ZST_OK);
    assert(zst_pad_link(venc_src, mux_video) == ZST_OK);

    zst_element_t* asrc = NULL;
    zst_element_t* aenc = NULL;

    if (include_audio) {
        asrc = zst_element_factory_make("audiotestsrc");
        zst_element_set_property_int(asrc, "num-buffers", 50);
        zst_element_set_property_bool(asrc, "use-clock", false);

        aenc = zst_element_factory_make("aacenc");
        zst_pipeline_add(pipe, asrc);
        zst_pipeline_add(pipe, aenc);

        zst_pad_t* mux_audio = zst_element_get_pad(mux, "audio");
        assert(mux_audio != NULL);

        assert(zst_pad_link(zst_element_get_pad(asrc, "src"), zst_element_get_pad(aenc, "sink")) == ZST_OK);
        assert(zst_pad_link(zst_element_get_pad(aenc, "src"), mux_audio) == ZST_OK);
    }

    zst_pad_t* mux_src = zst_element_get_pad(mux, "src");
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");
    assert(zst_pad_link(mux_src, sink_pad) == ZST_OK);

    assert(zst_scheduler_attach(sched, pipe) == ZST_OK);
    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(sched) == ZST_OK);

    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 5000);
        if (r == ZST_TIMEOUT || r != ZST_OK || !ev) break;
        if (ev->type == ZST_EVENT_ERROR || ev->type == ZST_EVENT_EOS) {
            zst_event_destroy(ev);
            break;
        }
        zst_event_destroy(ev);
    }

    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
}

static void concatenate_files(const char* dst_path, const char* src1, const char* src2)
{
    FILE* dst = fopen(dst_path, "wb");
    assert(dst != NULL);

    FILE* s1 = fopen(src1, "rb");
    assert(s1 != NULL);
    char buf[4096];
    size_t n;
    // Copy src1 100 times to exceed 5MB prober limit
    for (int i = 0; i < 100; i++) {
        fseek(s1, 0, SEEK_SET);
        while ((n = fread(buf, 1, sizeof(buf), s1)) > 0) {
            fwrite(buf, 1, n, dst);
        }
    }
    fclose(s1);

    FILE* s2 = fopen(src2, "rb");
    assert(s2 != NULL);
    while ((n = fread(buf, 1, sizeof(buf), s2)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(s2);

    fclose(dst);
}

static void test_mpegts_pmt_changes(void)
{
    const char* file_v = "/tmp/video_only.ts";
    const char* file_va = "/tmp/video_audio.ts";
    const char* file_merged = "/tmp/test_pmt_changes.ts";

    zst_plugin_registry_init();
    if (zst_register_builtin_elements() != ZST_OK) abort();

    write_mpegts_file(file_v, 0);
    write_mpegts_file(file_va, 1);

    FILE* f1 = fopen(file_v, "rb");
    FILE* f2 = fopen(file_va, "rb");
    if (f1) { fseek(f1, 0, SEEK_END); printf("file_v size: %ld bytes\n", ftell(f1)); fclose(f1); }
    else { printf("Failed to open file_v\n"); }
    if (f2) { fseek(f2, 0, SEEK_END); printf("file_va size: %ld bytes\n", ftell(f2)); fclose(f2); }
    else { printf("Failed to open file_va\n"); }

    concatenate_files(file_merged, file_v, file_va);

    FILE* fm = fopen(file_merged, "rb");
    if (fm) { fseek(fm, 0, SEEK_END); printf("file_merged size: %ld bytes\n", ftell(fm)); fclose(fm); }
    else { printf("Failed to open file_merged\n"); }

    unlink(file_v);
    unlink(file_va);

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    zst_element_t* src = zst_element_factory_make("filesrc");
    assert(src != NULL);
    assert(zst_element_set_property_string(src, "path", file_merged) == ZST_OK);

    zst_element_t* demux = zst_element_factory_make("tsdemux");
    assert(demux != NULL);

    zst_element_t* sink = zst_element_factory_make("fakesink");
    assert(sink != NULL);

    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, demux);
    zst_pipeline_add(pipe, sink);

    assert(zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(demux, "sink")) == ZST_OK);

    /* Set state to PLAYING */
    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);

    /* Run the scheduler to process the file stream */
    assert(zst_scheduler_run(sched) == ZST_OK);

    /* Process events until EOS */
    int video_added_seen = 0;
    int audio_added_seen = 0;
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");

    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 5000);
        if (r == ZST_TIMEOUT) {
            printf("Bus event loop TIMEOUT\n");
            break;
        }
        if (r != ZST_OK || !ev) {
            printf("Bus event loop error or NULL event, r=%d\n", r);
            break;
        }

        printf("Bus event type: %d\n", ev->type);

        if (ev->type == ZST_EVENT_PAD_ADDED) {
            zst_pad_t* pad = ev->as.pad_added.pad;
            printf("Bus event: Pad added: %s\n", pad->name);
            if (strcmp(pad->name, "video_0") == 0) {
                video_added_seen = 1;
                /* Verify that audio_0 is not discovered yet */
                assert(zst_element_get_pad(demux, "audio_0") == NULL);

                /* Link video_0 to the fakesink sink pad dynamically */
                zst_pipeline_reconfigure_begin(pipe);
                assert(zst_pipeline_link_pads_dynamic(pipe, pad, sink_pad) == ZST_OK);
                zst_pipeline_reconfigure_end(pipe);
            } else if (strcmp(pad->name, "audio_0") == 0) {
                assert(video_added_seen == 1);
                audio_added_seen = 1;
            }
        }
        if (ev->type == ZST_EVENT_ERROR) {
            printf("Bus event: ERROR: src=%s result=%d message=%s\n", 
                   (ev->src && ev->src->ops) ? ev->src->ops->name : "NULL",
                   ev->as.error.result, ev->as.error.message);
            zst_event_destroy(ev);
            break;
        }
        if (ev->type == ZST_EVENT_EOS) {
            printf("Bus event: EOS\n");
            zst_event_destroy(ev);
            break;
        }
        zst_event_destroy(ev);
    }

    /* Verify that audio_0 was discovered dynamically during playback! */
    assert(video_added_seen == 1);
    assert(audio_added_seen == 1);
    zst_pad_t* audio_pad = zst_element_get_pad(demux, "audio_0");
    assert(audio_pad != NULL);

    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    unlink(file_merged);
    zst_plugin_registry_deinit();

    printf("test_mpegts_pmt_changes passed!\n");
}

static void test_mp4_dynamic_tracks(void)
{
    const char* tmp_file = "/tmp/test_dynamic_mp4.mp4";
    unlink(tmp_file);

    zst_plugin_registry_init();
    if (zst_register_builtin_elements() != ZST_OK) abort();

    // 1. Write the MP4 file
    {
        zst_pipeline_t* pipe = zst_pipeline_create();
        zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
        zst_scheduler_t* sched = zst_scheduler_create(&cfg);

        zst_element_t* src = zst_element_factory_make("videotestsrc");
        zst_element_set_property_int(src, "width", 64);
        zst_element_set_property_int(src, "height", 48);
        zst_element_set_property_int(src, "fps", 30);
        zst_element_set_property_int(src, "num-buffers", 5);
        zst_element_set_property_bool(src, "use-clock", false);

        zst_element_t* enc = zst_element_factory_make("x264enc");
        zst_element_t* mux = zst_element_factory_make("mp4mux");
        zst_element_set_property_int(mux, "width", 64);
        zst_element_set_property_int(mux, "height", 48);
        zst_element_set_property_int(mux, "fps", 30);
        zst_element_set_property_string(mux, "location", tmp_file);

        zst_pipeline_add(pipe, src);
        zst_pipeline_add(pipe, enc);
        zst_pipeline_add(pipe, mux);

        assert(zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(enc, "sink")) == ZST_OK);
        assert(zst_pad_link(zst_element_get_pad(enc, "src"), zst_element_get_pad(mux, "video")) == ZST_OK);

        assert(zst_scheduler_attach(sched, pipe) == ZST_OK);
        assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
        assert(zst_scheduler_run(sched) == ZST_OK);

        for (;;) {
            zst_event_t* ev = NULL;
            zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 5000);
            if (r == ZST_TIMEOUT || r != ZST_OK || !ev) break;
            if (ev->type == ZST_EVENT_ERROR || ev->type == ZST_EVENT_EOS) {
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }

        zst_scheduler_stop(sched);
        zst_pipeline_set_state(pipe, ZST_STATE_NULL);
        zst_scheduler_destroy(sched);
        zst_pipeline_destroy(pipe);
    }

    // 2. Read the MP4 file and verify dynamic tracks
    {
        zst_element_t* demux = zst_element_factory_make("mp4demux");
        assert(demux != NULL);
        assert(zst_element_set_property_string(demux, "location", tmp_file) == ZST_OK);

        assert(zst_element_set_state(demux, ZST_STATE_READY) == ZST_OK);
        assert(zst_element_set_state(demux, ZST_STATE_PLAYING) == ZST_OK);

        zst_pad_t* video_pad = zst_element_get_pad(demux, "video_0");
        assert(video_pad != NULL);

        uint32_t stream_count = zst_element_get_stream_count(demux);
        assert(stream_count >= 1);

        zst_stream_info_t info;
        assert(zst_element_get_stream_info(demux, 0, &info) == ZST_OK);
        assert(info.kind == ZST_MEDIA_VIDEO);

        assert(zst_element_get_stream_pad(demux, info.id) != NULL);
        zst_stream_info_clear(&info);

        assert(zst_element_set_state(demux, ZST_STATE_NULL) == ZST_OK);
        zst_element_destroy(demux);
    }

    unlink(tmp_file);
    zst_plugin_registry_deinit();
    printf("test_mp4_dynamic_tracks passed!\n");
}

static zst_result_t mt_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)in; (void)el;
    usleep(100);
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    buf->payload = malloc(4);
    *(int*)buf->payload = 42;
    buf->destroy = mock_buf_destroy;
    *out = buf;
    return ZST_OK;
}

static zst_result_t mt_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    (void)el; (void)in; (void)out;
    return ZST_OK;
}

static zst_element_ops_t mt_src_ops = {.name = "mt_src", .process = mt_source_process};
static zst_element_ops_t mt_sink_ops = {.name = "mt_sink", .process = mt_sink_process};

static void test_multithread_dynamic_pad_removal(void)
{
    printf("Starting test_multithread_dynamic_pad_removal...\n");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    zst_element_t* src_el = zst_element_create(&mt_src_ops, NULL);
    zst_element_t* sink_el = zst_element_create(&mt_sink_ops, NULL);
    zst_pipeline_add(pipe, src_el);
    zst_pipeline_add(pipe, sink_el);

    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink_el, sink_pad);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(sched) == ZST_OK);

    for (int i = 0; i < 20; i++) {
        zst_pad_t* src_pad = zst_pad_create("src_dyn", ZST_PAD_SRC);
        zst_stream_info_t info = {
            .struct_size = sizeof(info),
            .id = 200 + i,
            .kind = ZST_MEDIA_VIDEO,
            .name = "Dyn"
        };

        zst_pipeline_reconfigure_begin(pipe);
        zst_element_add_dynamic_pad(src_el, src_pad, &info);
        zst_pipeline_link_pads_dynamic(pipe, src_pad, sink_pad);
        zst_pipeline_reconfigure_end(pipe);

        usleep(500);

        zst_pipeline_reconfigure_begin(pipe);
        zst_pipeline_unlink_pads_dynamic(pipe, src_pad, sink_pad);
        zst_element_remove_dynamic_pad(src_el, src_pad);
        zst_pipeline_reconfigure_end(pipe);
    }

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    printf("test_multithread_dynamic_pad_removal passed!\n");
}

static void test_sc6f0_source_dynamic(void)
{
    printf("Starting test_sc6f0_source_dynamic...\n");

    zst_plugin_registry_init();
    if (zst_register_builtin_elements() != ZST_OK) abort();

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_SINGLE_THREAD, .worker_threads = 1 };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    zst_element_t* src = zst_element_factory_make("sc6f0src");
    assert(src != NULL);
    assert(zst_element_set_property_bool(src, "mock-mode", true) == ZST_OK);
    assert(zst_element_set_property_string(src, "trigger-signal", "none") == ZST_OK);

    zst_pipeline_add(pipe, src);

    assert(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING) == ZST_OK);
    assert(zst_scheduler_run(sched) == ZST_OK);

    usleep(50000);

    assert(zst_element_get_stream_count(src) == 0);

    assert(zst_element_set_property_string(src, "trigger-signal", "1080p") == ZST_OK);
    usleep(250000); 

    int signal_present_seen = 0;
    int pad_added_seen = 0;
    int stream_added_seen = 0;

    zst_event_t* ev = NULL;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_SIGNAL_PRESENT) {
            signal_present_seen++;
        } else if (ev->type == ZST_EVENT_PAD_ADDED) {
            pad_added_seen++;
            printf("Mock test: Pad added: %s\n", ev->as.pad_added.pad->name);
        } else if (ev->type == ZST_EVENT_STREAM_ADDED) {
            stream_added_seen++;
        }
        zst_event_destroy(ev);
    }

    assert(signal_present_seen == 1);
    assert(pad_added_seen == 2); 
    assert(stream_added_seen == 2);

    uint32_t count = zst_element_get_stream_count(src);
    assert(count == 2); 

    zst_stream_info_t info;
    assert(zst_element_get_stream_info(src, 0, &info) == ZST_OK);
    assert(info.kind == ZST_MEDIA_VIDEO);
    assert(strcmp(info.name, "video_0") == 0);
    zst_stream_info_clear(&info);

    assert(zst_element_get_stream_info(src, 1, &info) == ZST_OK);
    assert(info.kind == ZST_MEDIA_AUDIO);
    assert(strcmp(info.name, "audio_0") == 0);
    zst_stream_info_clear(&info);

    zst_pad_t* vpad = zst_element_get_stream_pad(src, 1);
    assert(vpad != NULL);
    assert(strcmp(vpad->name, "video_0") == 0);

    assert(zst_element_set_property_string(src, "trigger-signal", "720p") == ZST_OK);
    usleep(250000);

    int caps_changed_seen = 0;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_CAPS_CHANGED) {
            caps_changed_seen++;
            printf("Mock test: Caps changed on pad %s\n", ev->as.caps_changed.pad->name);
        }
        zst_event_destroy(ev);
    }
    assert(caps_changed_seen >= 1);

    assert(zst_element_set_property_string(src, "trigger-signal", "none") == ZST_OK);
    usleep(250000);

    int signal_lost_seen = 0;
    int pad_removed_seen = 0;
    while (zst_bus_pop(bus, &ev, 1) == ZST_OK) {
        if (ev->type == ZST_EVENT_SIGNAL_LOST) {
            signal_lost_seen++;
        } else if (ev->type == ZST_EVENT_PAD_REMOVED) {
            pad_removed_seen++;
        }
        zst_event_destroy(ev);
    }
    assert(signal_lost_seen == 1);
    assert(pad_removed_seen == 2);

    assert(zst_element_get_stream_count(src) == 0);

    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
    zst_plugin_registry_deinit();

    printf("test_sc6f0_source_dynamic passed!\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    zst_log_set_level(ZST_LOG_LEVEL_DEBUG);
    printf("\n[test_dynamic]\n");
    test_dynamic_basics();
    test_pad_removed_playing();
    test_caps_changed_playing();
    test_signal_lost_present();
    test_unlinked_policies();
    test_mpegts_pmt_changes();
    test_mp4_dynamic_tracks();
    test_multithread_dynamic_pad_removal();
    test_sc6f0_source_dynamic();
    printf("ALL Phase G dynamic tests passed!\n\n");
    return 0;
}
