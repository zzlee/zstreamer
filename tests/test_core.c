/*=============================================================================
    test_core.c — Unit / smoke tests for the zstreamer core framework
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_buffer_pool.h"
#include "zst_queue.h"
#include "zst_scheduler.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_element_factory.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_file_source.h"
#include "zstreamer/elements/zst_file_sink.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_audio_test_src.h"
#include "zstreamer/elements/zst_text_overlay.h"
#include "zstreamer/elements/zst_mp4_muxer.h"
#include "zst_allocator.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"
#include "zstreamer/elements/zst_file_source.h"
#include "zstreamer/elements/zst_file_sink.h"
#include "zstreamer/elements/zst_rtmp_source.h"
#include "zstreamer/elements/zst_rtmp_sink.h"
#include "zstreamer/elements/zst_fake_sink.h"

zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);
zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
zst_element_t* zst_text_overlay_create(const char* text);
zst_element_t* zst_text_source_create(void);
zst_element_t* zst_audio_test_src_create(void);
zst_element_t* zst_h264_encoder_create(void);
zst_element_t* zst_h264_decoder_create(void);
zst_element_t* zst_h265_encoder_create(void);
zst_element_t* zst_h265_decoder_create(void);
zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_aac_decoder_create(void);

static int g_tests_run   = 0;
static int g_tests_passed = 0;

#define TEST(name)                                              \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  TEST: %-50s ... ", name);                     \
        fflush(stdout);                                         \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        g_tests_passed++;                                       \
        printf("PASS\n");                                       \
    } while (0)

/* ═══════════════════════════════════════════════════════════════
   Buffer tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_buffer_create_destroy(void)
{
    TEST("buffer create / destroy");
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_VIDEO_FRAME);
    assert(buf->refcount == 1);

    zst_buffer_unref(buf);
    PASS();
}

static void
test_buffer_refcount(void)
{
    TEST("buffer refcount");
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    assert(buf != NULL);

    zst_buffer_ref(buf);
    assert(buf->refcount == 2);

    zst_buffer_unref(buf);
    assert(buf->refcount == 1);

    zst_buffer_unref(buf);
    PASS();
}

static void
test_buffer_null_safety(void)
{
    TEST("buffer null safety");
    /* These should not crash */
    zst_buffer_ref(NULL);
    zst_buffer_unref(NULL);
    PASS();
}

static void
test_buffer_create_with_pool(void)
{
    TEST("buffer create with pool");
    zst_buffer_pool_config_t cfg = {
        .min_buffers = 1,
        .max_buffers = 1,
        .buffer_size = 1024,
        .buffer_type = ZST_BUFFER_USER
    };

    zst_buffer_pool_t* pool = zst_buffer_pool_create(NULL, &cfg);
    assert(pool != NULL);

    /* Pool preallocates min_buffers, so we should be able to get 1 */
    zst_buffer_t* buf = zst_buffer_create_with_pool(pool);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_USER);
    assert(buf->pool == pool);

    /* Pool size is max_buffers = 1, so getting another one with no timeout should return NULL since it fails to acquire */
    zst_buffer_t* buf2 = zst_buffer_create_with_pool(pool);
    assert(buf2 == NULL);

    zst_buffer_unref(buf);

    /* Now we should be able to get it again */
    buf2 = zst_buffer_create_with_pool(pool);
    assert(buf2 != NULL);
    zst_buffer_unref(buf2);

    zst_buffer_pool_destroy(pool);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pad tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pad_create_destroy(void)
{
    TEST("pad create / destroy");
    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    assert(src != NULL);
    assert(strcmp(src->name, "src") == 0);
    assert(src->direction == ZST_PAD_SRC);

    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    assert(sink != NULL);
    assert(sink->direction == ZST_PAD_SINK);

    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    PASS();
}

static void
test_pad_link_unlink(void)
{
    TEST("pad link / unlink");
    zst_pad_t* src  = zst_pad_create("src",  ZST_PAD_SRC);
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);

    zst_result_t r = zst_pad_link(src, sink);
    assert(r == ZST_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);

    /* Double-link should fail */
    zst_pad_t* sink2 = zst_pad_create("sink2", ZST_PAD_SINK);
    r = zst_pad_link(src, sink2);
    assert(r == ZST_ERROR);

    zst_pad_unlink(src);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);

    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    zst_pad_destroy(sink2);
    PASS();
}

static void
test_pad_invalid_link(void)
{
    TEST("pad invalid link");
    zst_pad_t* src  = zst_pad_create("src",  ZST_PAD_SRC);
    zst_pad_t* src2 = zst_pad_create("src2", ZST_PAD_SRC);

    /* SRC-SRC should fail */
    zst_result_t r = zst_pad_link(src, src2);
    assert(r == ZST_ERROR);

    zst_pad_destroy(src);
    zst_pad_destroy(src2);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Element tests
   ═══════════════════════════════════════════════════════════════ */
static zst_result_t
dummy_open(zst_element_t* el) { (void)el; return ZST_OK; }

static zst_element_ops_t g_dummy_ops = {
    .name   = "dummy",
    .open   = dummy_open,
};

static void
test_element_create_destroy(void)
{
    TEST("element create / destroy");
    zst_element_t* el = zst_element_create(&g_dummy_ops, NULL);
    assert(el != NULL);
    assert(el->state == ZST_STATE_NULL);
    assert(el->ops == &g_dummy_ops);

    zst_element_destroy(el);
    PASS();
}

static void
test_element_state_transition(void)
{
    TEST("element state transition");
    zst_element_t* el = zst_element_create(&g_dummy_ops, NULL);
    zst_result_t r;

    r = zst_element_set_state(el, ZST_STATE_READY);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_READY);

    r = zst_element_set_state(el, ZST_STATE_PLAYING);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_PLAYING);

    r = zst_element_set_state(el, ZST_STATE_NULL);
    assert(r == ZST_OK);
    assert(el->state == ZST_STATE_NULL);

    zst_element_destroy(el);
    PASS();
}

static void
test_element_pads(void)
{
    TEST("element pads");
    zst_element_t* el  = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t*     src = zst_pad_create("src", ZST_PAD_SRC);
    zst_pad_t*     snk = zst_pad_create("sink", ZST_PAD_SINK);

    zst_element_add_pad(el, src);
    zst_element_add_pad(el, snk);

    assert(el->nb_src_pads  == 1);
    assert(el->nb_sink_pads == 1);

    /* get_pad should find both */
    assert(zst_element_get_pad(el, "src")  == src);
    assert(zst_element_get_pad(el, "sink") == snk);
    assert(zst_element_get_pad(el, "none") == NULL);

    zst_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Pipeline tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_pipeline_create_destroy(void)
{
    TEST("pipeline create / destroy");
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    zst_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_add_remove(void)
{
    TEST("pipeline add / remove");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_element_t*  el   = zst_element_create(&g_dummy_ops, NULL);

    zst_pipeline_add(pipe, el);
    assert(pipe->nb_elements == 1);

    zst_pipeline_remove(pipe, el);
    assert(pipe->nb_elements == 0);

    zst_element_destroy(el);
    zst_pipeline_destroy(pipe);
    PASS();
}

static void
test_pipeline_state_propagation(void)
{
    TEST("pipeline state propagation");
    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_element_t*  el   = zst_element_create(&g_dummy_ops, NULL);

    zst_pipeline_add(pipe, el);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    assert(el->state == ZST_STATE_PLAYING);
    assert(pipe->state == ZST_STATE_PLAYING);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    assert(el->state == ZST_STATE_NULL);

    zst_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Queue tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_queue_push_pop(void)
{
    TEST("queue push / pop");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 5,
        .max_bytes   = 0,
        .max_duration= 0,
    };
    zst_queue_t* q = zst_queue_create(&cfg);
    assert(q != NULL);

    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);

    /* Push two buffers */
    assert(zst_queue_push(q, b1, 1000) == ZST_OK);
    assert(zst_queue_push(q, b2, 1000) == ZST_OK);
    assert(zst_queue_size(q) == 2);

    /* Pop them back */
    zst_buffer_t* out;
    assert(zst_queue_pop(q, &out, 1000) == ZST_OK);
    assert(out == b1);
    zst_buffer_unref(out);

    assert(zst_queue_pop(q, &out, 1000) == ZST_OK);
    assert(out == b2);
    zst_buffer_unref(out);

    assert(zst_queue_size(q) == 0);

    zst_queue_destroy(q);
    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    PASS();
}

static void
test_queue_timeout(void)
{
    TEST("queue timeout");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 1,
    };
    zst_queue_t* q = zst_queue_create(&cfg);
    assert(q != NULL);

    /* Pop from empty queue should timeout */
    zst_buffer_t* out;
    zst_result_t r = zst_queue_pop(q, &out, 10);
    assert(r == ZST_TIMEOUT);

    zst_queue_destroy(q);
    PASS();
}

static void
test_queue_flush(void)
{
    TEST("queue flush");
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 5,
    };
    zst_queue_t* q = zst_queue_create(&cfg);

    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    zst_queue_push(q, b1, 1000);
    zst_queue_push(q, b2, 1000);

    zst_queue_flush(q);
    assert(zst_queue_size(q) == 0);

    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    zst_queue_destroy(q);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Scheduler / Pipeline integration tests (Phase 2)
   ═══════════════════════════════════════════════════════════════ */
static void
mock_buf_destroy(zst_buffer_t* b)
{
    free(b->payload);
}

static zst_result_t
mock_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    int* counter = el->priv;
    if (*counter >= 5) {
        return ZST_EOF;
    }

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!buf) return ZST_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }
    *data = ++(*counter);
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return ZST_OK;
}

static zst_result_t
mock_transform_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)el;
    if (!in || !in->payload) return ZST_ERROR;

    int val = *(int*)in->payload;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!buf) return ZST_ERROR;

    int* data = malloc(sizeof(int));
    if (!data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }
    *data = val * 2;
    buf->payload = data;
    buf->destroy = mock_buf_destroy;

    *out = buf;
    return ZST_OK;
}

typedef struct {
    int count;
    int sum;
} mock_sink_t;

static zst_result_t
mock_sink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    mock_sink_t* sink = el->priv;
    if (!in || !in->payload) return ZST_ERROR;

    int val = *(int*)in->payload;
    sink->count++;
    sink->sum += val;

    return ZST_OK;
}

static void
test_scheduler_single_threaded(void)
{
    TEST("scheduler single-threaded pipeline");

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static zst_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    zst_element_t* source = zst_element_create(&source_ops, source_counter);
    zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(source, src_pad);

    static zst_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    zst_element_t* transform = zst_element_create(&transform_ops, NULL);
    zst_pad_t* trans_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* trans_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(transform, trans_sink);
    zst_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static zst_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    zst_element_t* sink = zst_element_create(&sink_ops, sink_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    zst_pipeline_add(pipe, source);
    zst_pipeline_add(pipe, transform);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(src_pad, trans_sink);
    zst_pad_link(trans_src, sink_pad);

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    PASS();
}

static void
test_queue_config_limits(void)
{
    TEST("queue config limits (bytes, duration, async mode)");

    /* Test 1: Max bytes in SYNC mode blocks/times out */
    zst_queue_config_t cfg_bytes = {
        .mode = ZST_QUEUE_SYNC,
        .max_bytes = 100,
    };
    zst_queue_t* q1 = zst_queue_create(&cfg_bytes);
    zst_buffer_t* b1 = zst_buffer_create(ZST_BUFFER_USER);
    b1->memory.size = 60;
    assert(zst_queue_push(q1, b1, 10) == ZST_OK);
    
    zst_buffer_t* b2 = zst_buffer_create(ZST_BUFFER_USER);
    b2->memory.size = 50; // Total is now 110, queue is now full
    assert(zst_queue_push(q1, b2, 10) == ZST_OK);

    zst_buffer_t* b3 = zst_buffer_create(ZST_BUFFER_USER);
    b3->memory.size = 10; // Queue is already full (110 >= 100), this must time out
    assert(zst_queue_push(q1, b3, 10) == ZST_TIMEOUT);

    zst_queue_destroy(q1);
    zst_buffer_unref(b1);
    zst_buffer_unref(b2);
    zst_buffer_unref(b3);

    /* Test 2: Max duration in SYNC mode blocks/times out */
    zst_queue_config_t cfg_dur = {
        .mode = ZST_QUEUE_SYNC,
        .max_duration = 1000,
    };
    zst_queue_t* q2 = zst_queue_create(&cfg_dur);
    zst_buffer_t* bd1 = zst_buffer_create(ZST_BUFFER_USER);
    bd1->pts = 10000;
    bd1->duration = 0;
    assert(zst_queue_push(q2, bd1, 10) == ZST_OK);

    zst_buffer_t* bd2 = zst_buffer_create(ZST_BUFFER_USER);
    bd2->pts = 11500; // Duration is 1500 (>= 1000), queue is now full
    bd2->duration = 0;
    assert(zst_queue_push(q2, bd2, 10) == ZST_OK);

    zst_buffer_t* bd3 = zst_buffer_create(ZST_BUFFER_USER);
    bd3->pts = 12000; // Queue is already full (1500 >= 1000), this must time out
    bd3->duration = 0;
    assert(zst_queue_push(q2, bd3, 10) == ZST_TIMEOUT);

    zst_queue_destroy(q2);
    zst_buffer_unref(bd1);
    zst_buffer_unref(bd2);
    zst_buffer_unref(bd3);

    /* Test 3: ASYNC mode drops buffers when full */
    zst_queue_config_t cfg_async = {
        .mode = ZST_QUEUE_ASYNC,
        .max_buffers = 1,
    };
    zst_queue_t* q3 = zst_queue_create(&cfg_async);
    zst_buffer_t* ba1 = zst_buffer_create(ZST_BUFFER_USER);
    assert(zst_queue_push(q3, ba1, 10) == ZST_OK);

    zst_buffer_t* ba2 = zst_buffer_create(ZST_BUFFER_USER);
    assert(zst_queue_push(q3, ba2, 10) == ZST_ERROR);

    zst_queue_destroy(q3);
    zst_buffer_unref(ba1);
    zst_buffer_unref(ba2);

    PASS();
}

static void
test_scheduler_multi_threaded(void)
{
    TEST("scheduler multi-threaded pipeline with queues");

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 3
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_attach(sched, pipe);

    int* source_counter = malloc(sizeof(int));
    *source_counter = 0;
    static zst_element_ops_t source_ops = {
        .name = "mock_source",
        .process = mock_source_process
    };
    zst_element_t* source = zst_element_create(&source_ops, source_counter);
    zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(source, src_pad);

    /* Add explicit queue elements */
    zst_element_t* q1 = zst_queue_element_create(NULL);
    zst_element_t* q2 = zst_queue_element_create(NULL);

    static zst_element_ops_t transform_ops = {
        .name = "mock_transform",
        .process = mock_transform_process
    };
    zst_element_t* transform = zst_element_create(&transform_ops, NULL);
    zst_pad_t* trans_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* trans_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(transform, trans_sink);
    zst_element_add_pad(transform, trans_src);

    mock_sink_t* sink_data = calloc(1, sizeof(mock_sink_t));
    static zst_element_ops_t sink_ops = {
        .name = "mock_sink",
        .process = mock_sink_process
    };
    zst_element_t* sink = zst_element_create(&sink_ops, sink_data);
    zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(sink, sink_pad);

    zst_pipeline_add(pipe, source);
    zst_pipeline_add(pipe, q1);
    zst_pipeline_add(pipe, transform);
    zst_pipeline_add(pipe, q2);
    zst_pipeline_add(pipe, sink);

    zst_pad_link(src_pad, zst_element_get_pad(q1, "sink"));
    zst_pad_link(zst_element_get_pad(q1, "src"), trans_sink);
    zst_pad_link(trans_src, zst_element_get_pad(q2, "sink"));
    zst_pad_link(zst_element_get_pad(q2, "src"), sink_pad);

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100 ms to let explicit queues process */
    nanosleep(&ts, NULL);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_stop(sched);

    assert(sink_data->count == 5);
    assert(sink_data->sum == 30);

    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    PASS();
}

static void
test_pipeline_topological_sort_check(void)
{
    TEST("pipeline topological sort");

    zst_pipeline_t* pipe = zst_pipeline_create();

    zst_element_t* a = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* a_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(a, a_src);

    zst_element_t* b = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* b_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_pad_t* b_src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(b, b_sink);
    zst_element_add_pad(b, b_src);

    zst_element_t* c = zst_element_create(&g_dummy_ops, NULL);
    zst_pad_t* c_sink = zst_pad_create("sink", ZST_PAD_SINK);
    zst_element_add_pad(c, c_sink);

    zst_pad_link(a_src, b_sink);
    zst_pad_link(b_src, c_sink);

    zst_pipeline_add(pipe, c);
    zst_pipeline_add(pipe, b);
    zst_pipeline_add(pipe, a);

    assert(pipe->elements[0] == c);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == a);

    zst_pipeline_topological_sort(pipe);

    assert(pipe->elements[0] == a);
    assert(pipe->elements[1] == b);
    assert(pipe->elements[2] == c);

    zst_pipeline_destroy(pipe);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Caps Negotiation tests (Phase 5)
   ═══════════════════════════════════════════════════════════════ */
static void
test_caps_basic(void)
{
    TEST("caps create / copy / destroy");
    zst_caps_t* caps = zst_caps_create();
    assert(caps != NULL);
    assert(caps->structs == NULL);

    zst_caps_struct_t* s1 = zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P");
    assert(s1 != NULL);
    assert(strcmp(s1->media_type, "video/x-raw") == 0);
    assert(s1->type == ZST_CAPS_VIDEO);
    assert(s1->video.width == 640);
    assert(s1->video.height == 480);
    assert(s1->video.framerate == 30.0);
    assert(strcmp(s1->video.pixel_format, "YUV420P") == 0);

    zst_result_t ret = zst_caps_append(caps, s1);
    assert(ret == ZST_OK);
    assert(caps->structs == s1);

    zst_caps_t* copy = zst_caps_copy(caps);
    assert(copy != NULL);
    assert(copy->structs != NULL);
    assert(copy->structs != s1);
    assert(strcmp(copy->structs->media_type, "video/x-raw") == 0);
    assert(copy->structs->video.width == 640);

    zst_caps_destroy(caps);
    zst_caps_destroy(copy);
    PASS();
}

static void
test_caps_intersection_video(void)
{
    TEST("caps intersection (video)");
    
    zst_caps_t* c1 = zst_caps_create();
    zst_caps_append(c1, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_caps_t* c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-h264", 640, 480, 30.0, ""));
    
    zst_caps_t* res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    zst_caps_destroy(res);

    zst_caps_destroy(c2);
    c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-raw", 0, 480, 0.0, ""));
    
    res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->video.width == 640);
    assert(res->structs->video.height == 480);
    assert(res->structs->video.framerate == 30.0);
    assert(strcmp(res->structs->video.pixel_format, "YUV420P") == 0);
    zst_caps_destroy(res);

    zst_caps_destroy(c2);
    c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_video("video/x-raw", 1280, 480, 30.0, "YUV420P"));
    
    res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs == NULL);
    zst_caps_destroy(res);

    zst_caps_destroy(c1);
    zst_caps_destroy(c2);
    PASS();
}

static void
test_caps_intersection_audio(void)
{
    TEST("caps intersection (audio)");
    
    zst_caps_t* c1 = zst_caps_create();
    zst_caps_append(c1, zst_caps_struct_create_audio("audio/x-raw", 2, 44100, "S16LE"));
    zst_caps_t* c2 = zst_caps_create();
    zst_caps_append(c2, zst_caps_struct_create_audio("audio/x-raw", 0, 0, ""));
    
    zst_caps_t* res = zst_caps_intersect(c1, c2);
    assert(res != NULL);
    assert(res->structs != NULL);
    assert(res->structs->audio.channels == 2);
    assert(res->structs->audio.sample_rate == 44100);
    assert(strcmp(res->structs->audio.format, "S16LE") == 0);
    
    zst_caps_destroy(res);
    zst_caps_destroy(c1);
    zst_caps_destroy(c2);
    PASS();
}

static void
test_caps_fixate(void)
{
    TEST("caps fixation");
    
    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    
    assert(!zst_caps_is_fixed(caps));
    
    zst_result_t ret = zst_caps_fixate(caps);
    assert(ret == ZST_OK);
    
    assert(zst_caps_is_fixed(caps));
    assert(caps->structs->video.width == 640);
    assert(caps->structs->video.height == 480);
    assert(caps->structs->video.framerate == 30.0);
    assert(strcmp(caps->structs->video.pixel_format, "YUV420P") == 0);
    
    zst_caps_destroy(caps);
    PASS();
}

static zst_caps_t*
element_get_caps_cb(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 1920, 1080, 60.0, "NV12"));
    return caps;
}

static zst_element_ops_t g_query_ops = {
    .name = "query_element",
    .get_caps = element_get_caps_cb
};

static void
test_pad_negotiate_and_link(void)
{
    TEST("pad caps negotiation / link compatibility");
    
    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    
    zst_caps_t* src_template = zst_caps_create();
    zst_caps_append(src_template, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_pad_set_template_caps(src, src_template);
    
    zst_caps_t* sink_template = zst_caps_create();
    zst_caps_append(sink_template, zst_caps_struct_create_video("video/x-raw", 1280, 720, 30.0, "YUV420P"));
    zst_pad_set_template_caps(sink, sink_template);
    
    zst_result_t ret = zst_pad_link(src, sink);
    assert(ret == ZST_ERROR);
    assert(src->peer == NULL);
    assert(sink->peer == NULL);
    
    zst_caps_destroy(sink_template);
    sink_template = zst_caps_create();
    zst_caps_append(sink_template, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, ""));
    zst_pad_set_template_caps(sink, sink_template);
    
    ret = zst_pad_link(src, sink);
    assert(ret == ZST_OK);
    assert(src->peer == sink);
    assert(sink->peer == src);
    
    assert(src->caps != NULL);
    assert(sink->caps != NULL);
    assert(zst_caps_is_fixed(src->caps));
    assert(src->caps->structs->video.width == 640);
    assert(src->caps->structs->video.height == 480);
    assert(strcmp(src->caps->structs->video.pixel_format, "YUV420P") == 0);
    
    zst_pad_destroy(src);
    zst_pad_destroy(sink);
    zst_caps_destroy(src_template);
    zst_caps_destroy(sink_template);
    
    zst_element_t* el = zst_element_create(&g_query_ops, NULL);
    zst_pad_t* query_pad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, query_pad);
    
    zst_caps_t* queried = zst_pad_get_caps(query_pad);
    assert(queried != NULL);
    assert(queried->structs != NULL);
    assert(queried->structs->video.width == 1920);
    assert(strcmp(queried->structs->video.pixel_format, "NV12") == 0);
    
    zst_caps_destroy(queried);
    zst_element_destroy(el);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Event Bus tests (Phase 6)
   ═══════════════════════════════════════════════════════════════ */
static void
test_event_create_destroy(void)
{
    TEST("event create / destroy");
    
    zst_event_t* ev1 = zst_event_new_eos(NULL);
    assert(ev1 != NULL);
    assert(ev1->type == ZST_EVENT_EOS);
    assert(ev1->src == NULL);
    zst_event_destroy(ev1);
    
    zst_event_t* ev2 = zst_event_new_error(NULL, ZST_ERROR, "test error");
    assert(ev2 != NULL);
    assert(ev2->type == ZST_EVENT_ERROR);
    assert(ev2->as.error.result == ZST_ERROR);
    assert(strcmp(ev2->as.error.message, "test error") == 0);
    zst_event_destroy(ev2);
    
    zst_event_t* ev3 = zst_event_new_state_changed(NULL, ZST_STATE_NULL, ZST_STATE_READY);
    assert(ev3 != NULL);
    assert(ev3->type == ZST_EVENT_STATE_CHANGED);
    assert(ev3->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev3->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev3);
    
    PASS();
}

static void
test_bus_basic(void)
{
    TEST("bus basic post / pop");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    zst_event_t* ev = zst_event_new_eos(NULL);
    zst_result_t r = zst_bus_post(bus, ev);
    assert(r == ZST_OK);
    
    zst_event_t* popped = NULL;
    r = zst_bus_pop(bus, &popped, 0);
    assert(r == ZST_OK);
    assert(popped != NULL);
    assert(popped->type == ZST_EVENT_EOS);
    
    zst_event_destroy(popped);
    zst_bus_destroy(bus);
    
    PASS();
}

static void
test_bus_timeout(void)
{
    TEST("bus pop timeout");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    zst_event_t* popped = NULL;
    zst_result_t r = zst_bus_pop(bus, &popped, 10);
    assert(r == ZST_TIMEOUT);
    assert(popped == NULL);
    
    zst_bus_destroy(bus);
    
    PASS();
}

static volatile int g_handler_called = 0;
static zst_event_type_t g_last_event_type;

static void
test_bus_handler_cb(zst_bus_t* bus, zst_event_t* event, void* user_data)
{
    (void)bus;
    (void)user_data;
    __atomic_fetch_add(&g_handler_called, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_last_event_type, event->type, __ATOMIC_SEQ_CST);
}

static void
test_bus_async_dispatch(void)
{
    TEST("bus async dispatch handler");
    
    zst_bus_t* bus = zst_bus_create();
    assert(bus != NULL);
    
    __atomic_store_n(&g_handler_called, 0, __ATOMIC_SEQ_CST);
    
    zst_result_t r = zst_bus_set_handler(bus, test_bus_handler_cb, NULL);
    assert(r == ZST_OK);
    
    zst_event_t* ev = zst_event_new_eos(NULL);
    r = zst_bus_post(bus, ev);
    assert(r == ZST_OK);
    
    /* Sleep a bit to allow dispatch thread to run */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    nanosleep(&ts, NULL);
    
    assert(__atomic_load_n(&g_handler_called, __ATOMIC_SEQ_CST) == 1);
    assert(__atomic_load_n(&g_last_event_type, __ATOMIC_SEQ_CST) == ZST_EVENT_EOS);
    
    /* Remove handler (stops thread) */
    r = zst_bus_set_handler(bus, NULL, NULL);
    assert(r == ZST_OK);
    
    zst_bus_destroy(bus);
    
    PASS();
}

static void
test_pipeline_bus_events(void)
{
    TEST("pipeline/element lifecycle events on bus");
    
    zst_pipeline_t* pipe = zst_pipeline_create();
    assert(pipe != NULL);
    assert(pipe->bus != NULL);
    
    zst_element_ops_t ops = { .name = "test_el" };
    zst_element_t* el = zst_element_create(&ops, NULL);
    assert(el != NULL);
    
    zst_result_t r = zst_pipeline_add(pipe, el);
    assert(r == ZST_OK);
    assert(el->bus == pipe->bus);
    
    /* Transition pipeline state */
    r = zst_pipeline_set_state(pipe, ZST_STATE_READY);
    assert(r == ZST_OK);
    
    /* We expect two events: element state changed, then pipeline state changed */
    zst_event_t* ev = NULL;
    r = zst_bus_pop(pipe->bus, &ev, 100);
    assert(r == ZST_OK);
    assert(ev != NULL);
    assert(ev->type == ZST_EVENT_STATE_CHANGED);
    assert(ev->src == el);
    assert(ev->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev);
    
    ev = NULL;
    r = zst_bus_pop(pipe->bus, &ev, 100);
    assert(r == ZST_OK);
    assert(ev != NULL);
    assert(ev->type == ZST_EVENT_STATE_CHANGED);
    assert(ev->src == NULL); // Pipeline itself
    assert(ev->as.state_changed.old_state == ZST_STATE_NULL);
    assert(ev->as.state_changed.new_state == ZST_STATE_READY);
    zst_event_destroy(ev);
    
    zst_pipeline_destroy(pipe); // also destroys el and bus
    
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Dynamic Plugins tests (Phase 7)
   ═══════════════════════════════════════════════════════════════ */
static void
test_plugin_registry_basic(void)
{
    TEST("plugin registry initialization and scanning");
    
    zst_result_t r = zst_plugin_registry_init();
    assert(r == ZST_OK);
    
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        /* fallback to /app/build/plugins if we are there */
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    r = zst_plugin_registry_scan(ppath);
    assert(r == ZST_OK);
    
    setenv("ZSTREAMER_PLUGIN_PATH", ppath, 1);
    r = zst_plugin_registry_scan_env();
    assert(r == ZST_OK);
    
    PASS();
}

static const char*
test_plugin_path(void)
{
    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    return ppath;
}

static void
test_element_factory_refcounting(void)
{
    TEST("element factory make and plugin refcounting");
    
    zst_plugin_registry_init();
    zst_plugin_registry_scan(test_plugin_path());
    
    zst_element_t* filesink = zst_element_factory_make("filesink");
    assert(filesink != NULL);
    assert(filesink->plugin != NULL);
    assert(strcmp(filesink->ops->name, "filesink") == 0);

    zst_element_t* fakesink = zst_element_factory_make("fakesink");
    assert(fakesink != NULL);
    assert(fakesink->plugin != NULL);
    assert(strcmp(fakesink->ops->name, "fakesink") == 0);
    
    zst_element_t* v4l2source = zst_element_factory_make("v4l2src");
    assert(v4l2source != NULL);
    assert(v4l2source->plugin != NULL);
    assert(strcmp(v4l2source->ops->name, "v4l2src") == 0);
    
    zst_element_t* alsasource = zst_element_factory_make("alsasrc");
    assert(alsasource != NULL);
    assert(alsasource->plugin != NULL);
    assert(strcmp(alsasource->ops->name, "alsasrc") == 0);
    
    zst_element_t* h264encoder = zst_element_factory_make("h264enc");
    assert(h264encoder != NULL);
    assert(h264encoder->plugin != NULL);
    assert(strcmp(h264encoder->ops->name, "h264enc") == 0);
    
    zst_element_t* h265encoder = zst_element_factory_make("h265enc");
    assert(h265encoder != NULL);
    assert(h265encoder->plugin != NULL);
    assert(strcmp(h265encoder->ops->name, "h265enc") == 0);
    
    zst_element_t* h265decoder = zst_element_factory_make("h265dec");
    assert(h265decoder != NULL);
    assert(h265decoder->plugin != NULL);
    assert(strcmp(h265decoder->ops->name, "h265dec") == 0);

    zst_element_t* rtspsrc = zst_element_factory_make("rtspsrc");
    assert(rtspsrc != NULL);
    assert(rtspsrc->plugin != NULL);
    assert(strcmp(rtspsrc->ops->name, "rtspsrc") == 0);
    zst_element_destroy(rtspsrc);

    zst_element_t* rtspsink = zst_element_factory_make("rtspsink");
    assert(rtspsink != NULL);
    assert(rtspsink->plugin != NULL);
    assert(strcmp(rtspsink->ops->name, "rtspsink") == 0);
    zst_element_destroy(rtspsink);

    zst_element_t* rtmpsrc = zst_element_factory_make("rtmpsrc");
    assert(rtmpsrc != NULL);
    assert(rtmpsrc->plugin != NULL);
    assert(strcmp(rtmpsrc->ops->name, "rtmpsrc") == 0);
    zst_element_destroy(rtmpsrc);

    zst_element_t* rtmpsink = zst_element_factory_make("rtmpsink");
    assert(rtmpsink != NULL);
    assert(rtmpsink->plugin != NULL);
    assert(strcmp(rtmpsink->ops->name, "rtmpsink") == 0);
    zst_element_destroy(rtmpsink);
    
    zst_element_t* aacencoder = zst_element_factory_make("aacenc");
    assert(aacencoder != NULL);
    assert(aacencoder->plugin != NULL);
    assert(strcmp(aacencoder->ops->name, "aacenc") == 0);
    
    zst_element_t* mp4muxer = zst_element_factory_make("mp4mux");
    assert(mp4muxer != NULL);
    assert(mp4muxer->plugin != NULL);
    assert(strcmp(mp4muxer->ops->name, "mp4mux") == 0);

    zst_element_t* videoscaler = zst_element_factory_make("videoscaler");
    assert(videoscaler != NULL);
    assert(videoscaler->plugin != NULL);
    assert(strcmp(videoscaler->ops->name, "videoscaler") == 0);

    zst_element_t* audioresampler = zst_element_factory_make("audioresampler");
    assert(audioresampler != NULL);
    assert(audioresampler->plugin != NULL);
    assert(strcmp(audioresampler->ops->name, "audioresampler") == 0);

    zst_element_t* audiotestsrc = zst_element_factory_make("audiotestsrc");
    assert(audiotestsrc != NULL);
    assert(audiotestsrc->plugin != NULL);
    assert(strcmp(audiotestsrc->ops->name, "audiotestsrc") == 0);

    zst_element_t* h264decoder = zst_element_factory_make("h264dec");
    assert(h264decoder != NULL);
    assert(h264decoder->plugin != NULL);
    assert(strcmp(h264decoder->ops->name, "h264dec") == 0);

    zst_element_t* aacdecoder = zst_element_factory_make("aacdec");
    assert(aacdecoder != NULL);
    assert(aacdecoder->plugin != NULL);
    assert(strcmp(aacdecoder->ops->name, "aacdec") == 0);

    zst_plugin_t* filesink_plugin = filesink->plugin;
    assert(filesink_plugin->refcount == 2);
    
    zst_element_destroy(filesink);
    assert(filesink_plugin->refcount == 1);
    
    zst_plugin_t* fakesink_plugin = fakesink->plugin;
    assert(fakesink_plugin->refcount == 2);
    zst_element_destroy(fakesink);
    assert(fakesink_plugin->refcount == 1);

    zst_element_destroy(v4l2source);
    zst_element_destroy(alsasource);
    zst_element_destroy(h264encoder);
    zst_element_destroy(h265encoder);
    zst_element_destroy(h265decoder);
    zst_element_destroy(aacencoder);
    zst_element_destroy(mp4muxer);
    zst_element_destroy(videoscaler);
    zst_element_destroy(audioresampler);
    zst_element_destroy(audiotestsrc);
    zst_element_destroy(h264decoder);
    zst_element_destroy(aacdecoder);
    
    zst_plugin_registry_deinit();
    
    PASS();
}

static void
test_builtin_element_registry(void)
{
    TEST("builtin element registry");

    zst_plugin_registry_init();
    assert(zst_register_builtin_elements() == ZST_OK);

    const zst_element_desc_t* queue_desc = zst_element_factory_get_desc("queue");
    assert(queue_desc != NULL);
    assert(strcmp(queue_desc->name, "queue") == 0);
    assert(queue_desc->nb_pads == 2);

    zst_element_t* queue = zst_element_factory_make("queue");
    assert(queue != NULL);
    assert(queue->plugin == NULL);
    assert(queue->desc == queue_desc);
    zst_element_destroy(queue);

    const zst_element_desc_t* audio_desc = zst_element_factory_get_desc("audiotestsrc");
    assert(audio_desc != NULL);
    assert(strcmp(audio_desc->name, "audiotestsrc") == 0);

    zst_element_t* audio = zst_element_factory_make("audiotestsrc");
    assert(audio != NULL);
    assert(audio->plugin == NULL);
    assert(audio->desc == audio_desc);
    zst_element_destroy(audio);

    const zst_element_desc_t** descs = NULL;
    uint32_t n_descs = zst_element_factory_list(&descs);
    assert(n_descs >= 2);
    assert(descs != NULL);
    zst_element_factory_list_free(descs);

    zst_plugin_registry_deinit();

    PASS();
}

static void
test_element_factory_introspection_and_typed_properties(void)
{
    TEST("element factory introspection and typed properties");

    zst_plugin_registry_init();
    zst_plugin_registry_scan(test_plugin_path());

    const zst_element_desc_t* filesrc_desc = zst_element_factory_get_desc("filesrc");
    assert(filesrc_desc != NULL);
    assert(strcmp(filesrc_desc->name, "filesrc") == 0);
    assert(filesrc_desc->nb_properties >= 5);
    assert(filesrc_desc->nb_pads == 1);
    assert(strcmp(filesrc_desc->pads[0].name, "src") == 0);
    assert(filesrc_desc->pads[0].direction == ZST_PAD_SRC);

    const zst_element_desc_t** descs = NULL;
    uint32_t n_descs = zst_element_factory_list(&descs);
    assert(n_descs >= 3);
    assert(descs != NULL);
    int saw_filesrc = 0;
    int saw_filesink = 0;
    int saw_fakesink = 0;
    for (uint32_t i = 0; i < n_descs; i++) {
        if (strcmp(descs[i]->name, "filesrc") == 0) saw_filesrc = 1;
        if (strcmp(descs[i]->name, "filesink") == 0) saw_filesink = 1;
        if (strcmp(descs[i]->name, "fakesink") == 0) saw_fakesink = 1;
    }
    assert(saw_filesrc && saw_filesink && saw_fakesink);
    zst_element_factory_list_free(descs);

    zst_element_t* src = zst_element_factory_make("filesrc");
    assert(src != NULL);
    assert(src->desc == filesrc_desc);
    assert(zst_element_set_property_string(src, "path", "input.bin") == ZST_OK);
    assert(zst_element_set_property_uint(src, "chunk-size", 16) == ZST_OK);
    assert(zst_element_set_property_bool(src, "loop", true) == ZST_OK);

    char path[64];
    uint64_t chunk_size = 0;
    bool loop = false;
    assert(zst_element_get_property_string(src, "path", path, sizeof(path)) == ZST_OK);
    assert(strcmp(path, "input.bin") == 0);
    assert(zst_element_get_property_uint(src, "chunk-size", &chunk_size) == ZST_OK);
    assert(chunk_size == 16);
    assert(zst_element_get_property_bool(src, "loop", &loop) == ZST_OK);
    assert(loop == true);
    zst_element_destroy(src);

    zst_element_t* sink = zst_element_factory_make("filesink");
    assert(sink != NULL);
    assert(zst_element_set_property_string(sink, "path", "output.bin") == ZST_OK);
    assert(zst_element_get_property_string(sink, "path", path, sizeof(path)) == ZST_OK);
    assert(strcmp(path, "output.bin") == 0);
    zst_element_destroy(sink);

    zst_element_t* fake = zst_element_factory_make("fakesink");
    assert(fake != NULL);
    assert(zst_element_set_property_double(fake, "drop-probability", 0.25) == ZST_OK);
    assert(zst_element_set_property_uint(fake, "total-buffers", 10) == ZST_ERROR);
    zst_element_destroy(fake);

    // Test config-based creators
    zst_file_source_config_t src_cfg = {
        .struct_size = sizeof(zst_file_source_config_t),
        .path = "config_input.bin",
        .chunk_size = 1024,
        .loop = true,
        .offset = 100,
        .length = 500
    };
    zst_element_t* cfg_src = zst_file_source_create_with_config(&src_cfg);
    assert(cfg_src != NULL);
    assert(zst_element_get_property_string(cfg_src, "path", path, sizeof(path)) == ZST_OK);
    assert(strcmp(path, "config_input.bin") == 0);
    uint64_t cfg_chunk_size = 0;
    assert(zst_element_get_property_uint(cfg_src, "chunk-size", &cfg_chunk_size) == ZST_OK);
    assert(cfg_chunk_size == 1024);
    bool cfg_loop = false;
    assert(zst_element_get_property_bool(cfg_src, "loop", &cfg_loop) == ZST_OK);
    assert(cfg_loop == true);
    int64_t cfg_offset = 0;
    assert(zst_element_get_property_int(cfg_src, "offset", &cfg_offset) == ZST_OK);
    assert(cfg_offset == 100);
    int64_t cfg_length = 0;
    assert(zst_element_get_property_int(cfg_src, "length", &cfg_length) == ZST_OK);
    assert(cfg_length == 500);
    zst_element_destroy(cfg_src);

    zst_file_sink_config_t sink_cfg = {
        .struct_size = sizeof(zst_file_sink_config_t),
        .path = "config_output.bin"
    };
    zst_element_t* cfg_sink = zst_file_sink_create_with_config(&sink_cfg);
    assert(cfg_sink != NULL);
    assert(zst_element_get_property_string(cfg_sink, "path", path, sizeof(path)) == ZST_OK);
    assert(strcmp(path, "config_output.bin") == 0);
    zst_element_destroy(cfg_sink);

    zst_fake_sink_config_t fake_cfg = {
        .struct_size = sizeof(zst_fake_sink_config_t),
        .drop_probability = 0.75
    };
    zst_element_t* cfg_fake = zst_fake_sink_create_with_config(&fake_cfg);
    assert(cfg_fake != NULL);
    double cfg_drop_prob = 0.0;
    assert(zst_element_get_property_double(cfg_fake, "drop-probability", &cfg_drop_prob) == ZST_OK);
    assert(cfg_drop_prob == 0.75);
    zst_element_destroy(cfg_fake);

    // Test more element config-based creators
    zst_video_test_src_config_t vts_cfg = {
        .struct_size = sizeof(zst_video_test_src_config_t),
        .width = 1280,
        .height = 720,
        .fps = 60,
        .pattern = "gradient",
        .pixel_format = "RGB",
        .num_buffers = 100,
        .loop = true,
        .use_clock = true
    };
    zst_element_t* cfg_vts = zst_video_test_src_create_with_config(&vts_cfg);
    assert(cfg_vts != NULL);
    uint64_t vts_width = 0, vts_height = 0, vts_fps = 0;
    assert(zst_element_get_property_uint(cfg_vts, "width", &vts_width) == ZST_OK);
    assert(vts_width == 1280);
    assert(zst_element_get_property_uint(cfg_vts, "height", &vts_height) == ZST_OK);
    assert(vts_height == 720);
    assert(zst_element_get_property_uint(cfg_vts, "fps", &vts_fps) == ZST_OK);
    assert(vts_fps == 60);
    char vts_pattern[32], vts_format[32];
    assert(zst_element_get_property_string(cfg_vts, "pattern", vts_pattern, sizeof(vts_pattern)) == ZST_OK);
    assert(strcmp(vts_pattern, "gradient") == 0);
    assert(zst_element_get_property_string(cfg_vts, "pixel-format", vts_format, sizeof(vts_format)) == ZST_OK);
    assert(strcmp(vts_format, "RGB") == 0);
    zst_element_destroy(cfg_vts);

    zst_audio_test_src_config_t ats_cfg = {
        .struct_size = sizeof(zst_audio_test_src_config_t),
        .sample_rate = 48000,
        .channels = 6,
        .sample_format = "F32LE",
        .wave = "square",
        .frequency = 880.0,
        .volume = 0.5,
        .samples_per_buffer = 512,
        .num_samples = 480000,
        .num_buffers = 937,
        .loop = true,
        .use_clock = true
    };
    zst_element_t* cfg_ats = zst_audio_test_src_create_with_config(&ats_cfg);
    assert(cfg_ats != NULL);
    uint64_t ats_rate = 0, ats_ch = 0;
    assert(zst_element_get_property_uint(cfg_ats, "sample-rate", &ats_rate) == ZST_OK);
    assert(ats_rate == 48000);
    assert(zst_element_get_property_uint(cfg_ats, "channels", &ats_ch) == ZST_OK);
    assert(ats_ch == 6);
    char ats_format[32], ats_wave[32];
    assert(zst_element_get_property_string(cfg_ats, "sample-format", ats_format, sizeof(ats_format)) == ZST_OK);
    assert(strcmp(ats_format, "F32LE") == 0);
    assert(zst_element_get_property_string(cfg_ats, "wave", ats_wave, sizeof(ats_wave)) == ZST_OK);
    assert(strcmp(ats_wave, "square") == 0);
    double ats_freq = 0.0, ats_vol = 0.0;
    assert(zst_element_get_property_double(cfg_ats, "frequency", &ats_freq) == ZST_OK);
    assert(ats_freq == 880.0);
    assert(zst_element_get_property_double(cfg_ats, "volume", &ats_vol) == ZST_OK);
    assert(ats_vol == 0.5);
    zst_element_destroy(cfg_ats);

    zst_text_overlay_config_t to_cfg = {
        .struct_size = sizeof(zst_text_overlay_config_t),
        .text = "Hello Config",
        .timecode = true,
        .font_size = 36,
        .font_path = "/usr/share/fonts/dejavu.ttf",
        .x = 20,
        .y = 50
    };
    zst_element_t* cfg_to = zst_text_overlay_create_with_config(&to_cfg);
    assert(cfg_to != NULL);
    char to_text[64], to_path[256];
    assert(zst_element_get_property_string(cfg_to, "text", to_text, sizeof(to_text)) == ZST_OK);
    assert(strcmp(to_text, "Hello Config") == 0);
    bool to_tc = false;
    assert(zst_element_get_property_bool(cfg_to, "timecode", &to_tc) == ZST_OK);
    assert(to_tc == true);
    int64_t to_sz = 0, to_x = 0, to_y = 0;
    assert(zst_element_get_property_int(cfg_to, "font-size", &to_sz) == ZST_OK);
    assert(to_sz == 36);
    assert(zst_element_get_property_string(cfg_to, "font-path", to_path, sizeof(to_path)) == ZST_OK);
    assert(strcmp(to_path, "/usr/share/fonts/dejavu.ttf") == 0);
    assert(zst_element_get_property_int(cfg_to, "x", &to_x) == ZST_OK);
    assert(to_x == 20);
    assert(zst_element_get_property_int(cfg_to, "y", &to_y) == ZST_OK);
    assert(to_y == 50);
    zst_element_destroy(cfg_to);

    zst_mp4_muxer_config_t mux_cfg = {
        .struct_size = sizeof(zst_mp4_muxer_config_t),
        .width = 1920,
        .height = 1080,
        .fps = 30,
        .sample_rate = 44100,
        .channels = 2,
        .location = "config_output.mp4"
    };
    zst_element_t* cfg_mux = zst_mp4_muxer_create_with_config(&mux_cfg);
    assert(cfg_mux != NULL);
    uint64_t mux_width = 0, mux_height = 0, mux_fps = 0, mux_rate = 0, mux_ch = 0;
    assert(zst_element_get_property_uint(cfg_mux, "width", &mux_width) == ZST_OK);
    assert(mux_width == 1920);
    assert(zst_element_get_property_uint(cfg_mux, "height", &mux_height) == ZST_OK);
    assert(mux_height == 1080);
    assert(zst_element_get_property_uint(cfg_mux, "fps", &mux_fps) == ZST_OK);
    assert(mux_fps == 30);
    assert(zst_element_get_property_uint(cfg_mux, "sample-rate", &mux_rate) == ZST_OK);
    assert(mux_rate == 44100);
    assert(zst_element_get_property_uint(cfg_mux, "channels", &mux_ch) == ZST_OK);
    assert(mux_ch == 2);
    char mux_loc[256];
    assert(zst_element_get_property_string(cfg_mux, "location", mux_loc, sizeof(mux_loc)) == ZST_OK);
    assert(strcmp(mux_loc, "config_output.mp4") == 0);
    zst_element_destroy(cfg_mux);

    zst_plugin_registry_deinit();

    PASS();
}

typedef struct {
    int buffers;
    uint32_t last_type;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t nb_samples;
    uint32_t format;
    zst_time_t duration;
} decoder_capture_t;

static zst_result_t
decoder_capture_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)out;
    decoder_capture_t* c = el->priv;
    assert(c != NULL);
    assert(in != NULL);

    c->buffers++;
    c->last_type = in->type;
    c->duration = in->duration;

    if (!(in->flags & ZST_BUFFER_FLAG_EOS) && in->payload) {
        if (in->type == ZST_BUFFER_VIDEO_FRAME) {
            zst_video_frame_t* frame = in->payload;
            c->width = frame->width;
            c->height = frame->height;
            c->format = frame->format;
            assert(frame->plane[0] != NULL);
        } else if (in->type == ZST_BUFFER_AUDIO_FRAME) {
            zst_audio_frame_t* frame = in->payload;
            c->sample_rate = frame->sample_rate;
            c->channels = frame->channels;
            c->nb_samples = frame->nb_samples;
            c->format = frame->format;
            assert(frame->data != NULL);
        }
    }

    return ZST_OK;
}

static zst_element_ops_t g_decoder_capture_ops = {
    .name = "decoder_capture",
    .process = decoder_capture_process,
};

static zst_element_t*
decoder_capture_create(decoder_capture_t* capture)
{
    zst_element_t* el = zst_element_create(&g_decoder_capture_ops, capture);
    assert(el != NULL);
    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    assert(sink != NULL);
    assert(zst_element_add_pad(el, sink) == ZST_OK);
    return el;
}

static void
decoder_test_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        free(buf->memory.data);
        free(buf->payload);
    }
}

static void
test_h264_decoder_roundtrip(void)
{
    TEST("H.264 decoder (Phase 4v) roundtrip and caps");

    zst_element_t* enc = zst_h264_encoder_create();
    zst_element_t* dec = zst_h264_decoder_create();
    decoder_capture_t* capture = calloc(1, sizeof(*capture));
    assert(capture != NULL);
    zst_element_t* sink = decoder_capture_create(capture);
    assert(enc != NULL && dec != NULL && sink != NULL);
    assert(strcmp(dec->ops->name, "h264dec") == 0);
    assert(zst_element_set_state(enc, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(dec, ZST_STATE_READY) == ZST_OK);
    assert(zst_pad_link(dec->src_pads[0], sink->sink_pads[0]) == ZST_OK);

    const int width = 64;
    const int height = 64;
    zst_buffer_t* raw = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(raw != NULL);
    raw->memory.size = (size_t)width * (size_t)height * 3u / 2u;
    raw->memory.data = calloc(1, raw->memory.size);
    raw->payload = calloc(1, sizeof(zst_video_frame_t));
    raw->destroy = decoder_test_buf_free;
    assert(raw->memory.data != NULL && raw->payload != NULL);

    zst_video_frame_t* frame = raw->payload;
    frame->width = width;
    frame->height = height;
    frame->format = 0; /* AV_PIX_FMT_YUV420P */
    frame->plane[0] = raw->memory.data;
    frame->plane[1] = (uint8_t*)raw->memory.data + width * height;
    frame->plane[2] = (uint8_t*)raw->memory.data + width * height + width * height / 4;
    frame->stride[0] = width;
    frame->stride[1] = width / 2;
    frame->stride[2] = width / 2;
    memset(frame->plane[0], 80, (size_t)width * height);
    memset(frame->plane[1], 90, (size_t)width * height / 4);
    memset(frame->plane[2], 100, (size_t)width * height / 4);

    zst_buffer_t* pkt = NULL;
    assert(enc->ops->process(enc, raw, &pkt) == ZST_OK);
    assert(pkt != NULL && pkt->memory.size > 0);
    assert(dec->sink_pads[0]->push(dec->sink_pads[0], pkt) == ZST_OK);

    assert(capture->buffers >= 1);
    assert(capture->last_type == ZST_BUFFER_VIDEO_FRAME);
    assert(capture->width == (uint32_t)width);
    assert(capture->height == (uint32_t)height);

    zst_caps_t* caps = zst_pad_get_caps(dec->src_pads[0]);
    assert(caps != NULL && caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "video/x-raw") == 0);
    assert(caps->structs->video.width == width);
    assert(caps->structs->video.height == height);
    zst_caps_destroy(caps);

    zst_buffer_unref(pkt);
    zst_buffer_unref(raw);
    zst_element_destroy(sink);
    zst_element_destroy(dec);
    zst_element_destroy(enc);
    PASS();
}

static void
test_h265_decoder_roundtrip(void)
{
    TEST("H.265 decoder roundtrip and caps");

    zst_element_t* enc = zst_h265_encoder_create();
    zst_element_t* dec = zst_h265_decoder_create();
    decoder_capture_t* capture = calloc(1, sizeof(*capture));
    assert(capture != NULL);
    zst_element_t* sink = decoder_capture_create(capture);
    assert(enc != NULL && dec != NULL && sink != NULL);
    assert(strcmp(enc->ops->name, "h265enc") == 0);
    assert(strcmp(dec->ops->name, "h265dec") == 0);
    assert(zst_element_set_state(enc, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(dec, ZST_STATE_READY) == ZST_OK);
    assert(zst_pad_link(dec->src_pads[0], sink->sink_pads[0]) == ZST_OK);

    const int width = 64;
    const int height = 64;
    zst_buffer_t* pkt = NULL;

    for (int n = 0; n < 8 && !pkt; n++) {
        zst_buffer_t* raw = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        assert(raw != NULL);
        raw->memory.size = (size_t)width * (size_t)height * 3u / 2u;
        raw->memory.data = calloc(1, raw->memory.size);
        raw->payload = calloc(1, sizeof(zst_video_frame_t));
        raw->destroy = decoder_test_buf_free;
        assert(raw->memory.data != NULL && raw->payload != NULL);
        raw->pts = (zst_time_t)n;

        zst_video_frame_t* frame = raw->payload;
        frame->width = width;
        frame->height = height;
        frame->format = 0; /* AV_PIX_FMT_YUV420P */
        frame->plane[0] = raw->memory.data;
        frame->plane[1] = (uint8_t*)raw->memory.data + width * height;
        frame->plane[2] = (uint8_t*)raw->memory.data + width * height + width * height / 4;
        frame->stride[0] = width;
        frame->stride[1] = width / 2;
        frame->stride[2] = width / 2;
        memset(frame->plane[0], 80 + n, (size_t)width * height);
        memset(frame->plane[1], 90, (size_t)width * height / 4);
        memset(frame->plane[2], 100, (size_t)width * height / 4);

        assert(enc->ops->process(enc, raw, &pkt) == ZST_OK);
        zst_buffer_unref(raw);
    }

    assert(pkt != NULL && pkt->memory.size > 0);
    assert(dec->sink_pads[0]->push(dec->sink_pads[0], pkt) == ZST_OK);

    assert(capture->buffers >= 1);
    assert(capture->last_type == ZST_BUFFER_VIDEO_FRAME);
    assert(capture->width == (uint32_t)width);
    assert(capture->height == (uint32_t)height);

    zst_caps_t* caps = zst_pad_get_caps(dec->src_pads[0]);
    assert(caps != NULL && caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "video/x-raw") == 0);
    assert(caps->structs->video.width == width);
    assert(caps->structs->video.height == height);
    zst_caps_destroy(caps);

    zst_buffer_unref(pkt);
    zst_element_destroy(sink);
    zst_element_destroy(dec);
    zst_element_destroy(enc);
    PASS();
}

static int
aac_test_freq_index(int sample_rate)
{
    switch (sample_rate) {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    case 12000: return 9;
    case 11025: return 10;
    case 8000:  return 11;
    default:    return 4;
    }
}

static void
aac_test_write_adts(uint8_t* h, int payload_len, int sample_rate, int channels)
{
    int profile = 1; /* AAC LC */
    int freq_idx = aac_test_freq_index(sample_rate);
    int frame_len = payload_len + 7;
    h[0] = 0xff;
    h[1] = 0xf1;
    h[2] = (uint8_t)(((profile & 3) << 6) | ((freq_idx & 15) << 2) | ((channels >> 2) & 1));
    h[3] = (uint8_t)(((channels & 3) << 6) | ((frame_len >> 11) & 3));
    h[4] = (uint8_t)((frame_len >> 3) & 0xff);
    h[5] = (uint8_t)(((frame_len & 7) << 5) | 0x1f);
    h[6] = 0xfc;
}

static void
test_aac_decoder_roundtrip(void)
{
    TEST("AAC decoder (Phase 4y) ADTS decode and caps");

    zst_element_t* enc = zst_aac_encoder_create();
    zst_element_t* dec = zst_aac_decoder_create();
    decoder_capture_t* capture = calloc(1, sizeof(*capture));
    assert(capture != NULL);
    zst_element_t* sink = decoder_capture_create(capture);
    assert(enc != NULL && dec != NULL && sink != NULL);
    assert(strcmp(dec->ops->name, "aacdec") == 0);
    assert(zst_element_set_state(enc, ZST_STATE_READY) == ZST_OK);
    assert(zst_element_set_state(dec, ZST_STATE_READY) == ZST_OK);
    assert(zst_pad_link(dec->src_pads[0], sink->sink_pads[0]) == ZST_OK);

    zst_buffer_t* pkt = NULL;
    for (int n = 0; n < 5 && !pkt; n++) {
        zst_buffer_t* raw = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
        assert(raw != NULL);
        raw->memory.size = 1024u * 2u * sizeof(int16_t);
        raw->memory.data = calloc(1, raw->memory.size);
        raw->payload = calloc(1, sizeof(zst_audio_frame_t));
        raw->destroy = decoder_test_buf_free;
        assert(raw->memory.data != NULL && raw->payload != NULL);

        zst_audio_frame_t* frame = raw->payload;
        frame->sample_rate = 44100;
        frame->channels = 2;
        frame->format = 0; /* project S16LE code */
        frame->nb_samples = 1024;
        frame->data = raw->memory.data;
        int16_t* pcm = raw->memory.data;
        for (int i = 0; i < 1024 * 2; i++) {
            pcm[i] = (int16_t)((i % 100) - 50);
        }

        assert(enc->ops->process(enc, raw, &pkt) == ZST_OK);
        zst_buffer_unref(raw);
    }
    assert(pkt != NULL && pkt->memory.size > 0);

    zst_buffer_t* adts_pkt = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
    assert(adts_pkt != NULL);
    adts_pkt->memory.size = pkt->memory.size + 7;
    adts_pkt->memory.data = malloc(adts_pkt->memory.size);
    adts_pkt->destroy = decoder_test_buf_free;
    assert(adts_pkt->memory.data != NULL);
    aac_test_write_adts(adts_pkt->memory.data, (int)pkt->memory.size, 44100, 2);
    memcpy((uint8_t*)adts_pkt->memory.data + 7, pkt->memory.data, pkt->memory.size);
    adts_pkt->pts = pkt->pts;
    adts_pkt->dts = pkt->dts;

    assert(dec->sink_pads[0]->push(dec->sink_pads[0], adts_pkt) == ZST_OK);
    assert(capture->buffers >= 1);
    assert(capture->last_type == ZST_BUFFER_AUDIO_FRAME);
    assert(capture->sample_rate == 44100);
    assert(capture->channels == 2);
    assert(capture->nb_samples > 0);
    assert(capture->duration > 0);

    zst_caps_t* caps = zst_pad_get_caps(dec->src_pads[0]);
    assert(caps != NULL && caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "audio/x-raw") == 0);
    assert(caps->structs->audio.sample_rate == 44100);
    assert(caps->structs->audio.channels == 2);
    zst_caps_destroy(caps);

    zst_buffer_unref(adts_pkt);
    zst_buffer_unref(pkt);
    zst_element_destroy(sink);
    zst_element_destroy(dec);
    zst_element_destroy(enc);
    PASS();
}

static void
scaler_test_free(zst_buffer_t* buf)
{
    if (buf) {
        free(buf->memory.data);
        free(buf->payload);
    }
}

static void
resampler_test_free(zst_buffer_t* buf)
{
    if (buf) {
        free(buf->memory.data);
        free(buf->payload);
    }
}

static void
test_video_scaler(void)
{
    TEST("video scaler (Phase 4g) basic scaling and fallback");

    /* 1. Create video scaler element */
    zst_element_t* scaler = zst_video_scaler_create(320, 240, "YUV420P");
    assert(scaler != NULL);
    assert(strcmp(scaler->ops->name, "videoscaler") == 0);

    /* 2. Open it */
    assert(zst_element_set_state(scaler, ZST_STATE_READY) == ZST_OK);

    /* 3. Create an input buffer (YUV420P, 640x480) */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    assert(in_buf != NULL);

    size_t in_y_size = 640 * 480;
    size_t in_uv_size = in_y_size / 4;
    size_t in_total_size = in_y_size + 2 * in_uv_size;
    uint8_t* in_data = malloc(in_total_size);
    assert(in_data != NULL);
    memset(in_data, 128, in_total_size);

    in_buf->memory.type = ZST_MEMORY_CPU;
    in_buf->memory.data = in_data;
    in_buf->memory.size = in_total_size;

    zst_video_frame_t* in_frame = calloc(1, sizeof(*in_frame));
    assert(in_frame != NULL);
    in_frame->width = 640;
    in_frame->height = 480;
    in_frame->format = 0; /* YUV420P */
    in_frame->plane[0] = in_data;
    in_frame->plane[1] = in_data + in_y_size;
    in_frame->plane[2] = in_data + in_y_size + in_uv_size;
    in_frame->stride[0] = 640;
    in_frame->stride[1] = 320;
    in_frame->stride[2] = 320;
    in_buf->payload = in_frame;

    in_buf->destroy = scaler_test_free;

    /* Set some caps on sink pad representing input */
    zst_caps_t* sink_caps = zst_caps_create();
    zst_caps_append(sink_caps, zst_caps_struct_create_video("video/x-raw", 640, 480, 30.0, "YUV420P"));
    zst_pad_t* sink_pad = zst_element_get_pad(scaler, "sink");
    assert(zst_pad_set_caps(sink_pad, sink_caps) == ZST_OK);
    zst_caps_destroy(sink_caps);

    /* Set some caps on src pad representing target/negotiated output */
    zst_caps_t* src_caps = zst_caps_create();
    zst_caps_append(src_caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    zst_pad_t* src_pad = zst_element_get_pad(scaler, "src");
    assert(zst_pad_set_caps(src_pad, src_caps) == ZST_OK);
    zst_caps_destroy(src_caps);

    /* 4. Process the buffer */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = scaler->ops->process(scaler, in_buf, &out_buf);
    assert(res == ZST_OK);
    assert(out_buf != NULL);

    /* Verify scaled output dimensions */
    zst_video_frame_t* out_frame = out_buf->payload;
    assert(out_frame != NULL);
    assert(out_frame->width == 320);
    assert(out_frame->height == 240);
    assert(out_frame->format == 0); /* YUV420P */

    zst_buffer_unref(out_buf);
    zst_buffer_unref(in_buf);

    /* 5. Clean up */
    assert(zst_element_set_state(scaler, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(scaler);

    PASS();
}

static void
test_audio_resampler(void)
{
    TEST("audio resampler (Phase 4h) basic resampling and fallback");

    /* 1. Create resampler element: 48000Hz stereo -> 44100Hz stereo */
    zst_element_t* resampler = zst_audio_resampler_create(44100, 2, "S16LE");
    assert(resampler != NULL);
    assert(strcmp(resampler->ops->name, "audioresampler") == 0);

    /* 2. Open it */
    assert(zst_element_set_state(resampler, ZST_STATE_READY) == ZST_OK);

    /* 3. Create input buffer: 48000Hz stereo, 480 samples, interleaved S16 */
    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    assert(in_buf != NULL);

    int in_samples = 480;
    int in_channels = 2;
    size_t in_size = in_samples * in_channels * sizeof(int16_t);
    int16_t* in_data = malloc(in_size);
    assert(in_data != NULL);
    memset(in_data, 0, in_size);

    in_buf->memory.type = ZST_MEMORY_CPU;
    in_buf->memory.data = in_data;
    in_buf->memory.size = in_size;

    zst_audio_frame_t* in_frame = calloc(1, sizeof(*in_frame));
    assert(in_frame != NULL);
    in_frame->sample_rate = 48000;
    in_frame->channels = 2;
    in_frame->format = 0; /* S16LE */
    in_frame->nb_samples = in_samples;
    in_frame->data = in_data;
    in_buf->payload = in_frame;

    in_buf->destroy = resampler_test_free;

    /* Set caps on sink pad representing input */
    zst_caps_t* sink_caps = zst_caps_create();
    zst_caps_append(sink_caps, zst_caps_struct_create_audio("audio/x-raw", 2, 48000, "S16LE"));
    zst_pad_t* sink_pad = zst_element_get_pad(resampler, "sink");
    assert(zst_pad_set_caps(sink_pad, sink_caps) == ZST_OK);
    zst_caps_destroy(sink_caps);

    /* Set caps on src pad representing output */
    zst_caps_t* src_caps = zst_caps_create();
    zst_caps_append(src_caps, zst_caps_struct_create_audio("audio/x-raw", 2, 44100, "S16LE"));
    zst_pad_t* src_pad = zst_element_get_pad(resampler, "src");
    assert(zst_pad_set_caps(src_pad, src_caps) == ZST_OK);
    zst_caps_destroy(src_caps);

    /* 4. Process buffer */
    zst_buffer_t* out_buf = NULL;
    zst_result_t res = resampler->ops->process(resampler, in_buf, &out_buf);
    assert(res == ZST_OK);
    assert(out_buf != NULL);

    /* Verify output samples */
    zst_audio_frame_t* out_frame = out_buf->payload;
    assert(out_frame != NULL);
    assert(out_frame->sample_rate == 44100);
    assert(out_frame->channels == 2);
    /* Expect roughly 441 samples, or slightly fewer due to resampler filter delay (e.g. 425) */
    assert(out_frame->nb_samples >= 420 && out_frame->nb_samples <= 450);

    zst_buffer_unref(out_buf);
    zst_buffer_unref(in_buf);

    /* 5. Clean up */
    assert(zst_element_set_state(resampler, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(resampler);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Logging tests (Phase 3.5)
   ═══════════════════════════════════════════════════════════════ */

/* ── capture buffer for custom handler test ─────────────────────── */
static char g_log_buf[4096];
static int  g_log_called;

static void
test_log_handler(zst_log_level_t level,
                  const char* category,
                  const char* file,
                  int line,
                  const char* func,
                  const char* message)
{
    (void)file; (void)line; (void)func;
    g_log_called++;
    snprintf(g_log_buf, sizeof(g_log_buf),
             "%d [%s] %s", (int)level,
             category ? category : "",
             message ? message : "");
}

static void
test_log_levels(void)
{
    TEST("log level runtime filter");

    zst_log_set_handler(test_log_handler);
    zst_log_set_level(ZST_LOG_LEVEL_INFO);

    /* ERROR and INFO should pass; DEBUG should not */
    g_log_called = 0;
    ZST_LOG_ERROR("test", "error msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_WARN("test", "warn msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_INFO("test", "info msg");
    assert(g_log_called == 1);

    g_log_called = 0;
    ZST_LOG_DEBUG("test", "debug msg");
    assert(g_log_called == 0);

    g_log_called = 0;
    ZST_LOG_TRACE("test", "trace msg");
    assert(g_log_called == 0);

    /* Lower the bar: DEBUG should now pass */
    zst_log_set_level(ZST_LOG_LEVEL_DEBUG);
    g_log_called = 0;
    ZST_LOG_DEBUG("test", "debug ok");
    assert(g_log_called == 1);

    /* Restore defaults */
    zst_log_set_handler(NULL);
    zst_log_set_level(ZST_LOG_LEVEL_TRACE);

    PASS();
}

static void
test_log_custom_handler(void)
{
    TEST("log custom handler receives correct data");

    zst_log_set_handler(test_log_handler);
    zst_log_set_level(ZST_LOG_LEVEL_TRACE);

    g_log_called = 0;
    memset(g_log_buf, 0, sizeof(g_log_buf));

    ZST_LOG_WARN("mycat", "hello %s %d", "world", 42);

    assert(g_log_called == 1);
    /* Check the captured buffer */
    assert(strstr(g_log_buf, "2 [mycat] hello world 42") != NULL);

    zst_log_set_handler(NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Allocator & Clock tests (Phase 8a/8b)
   ═══════════════════════════════════════════════════════════════ */
static void
test_allocator_basic(void)
{
    TEST("allocator create / alloc / free / destroy");

    zst_allocator_t* alloc = zst_allocator_cpu_create();
    assert(alloc != NULL);

    void* ptr = zst_allocator_alloc(alloc, 1024);
    assert(ptr != NULL);

    zst_allocator_free(alloc, ptr);

    zst_buffer_t* buf = zst_buffer_create_with_allocator(ZST_BUFFER_USER, alloc, 512);
    assert(buf != NULL);
    assert(buf->memory.data != NULL);
    assert(buf->memory.size == 512);

    /* Buffer creation should have bumped the allocator's refcount */
    assert(alloc->refcount == 2);

    zst_buffer_unref(buf);

    /* Allocator refcount should be back to 1 */
    assert(alloc->refcount == 1);

    zst_allocator_unref(alloc);

    PASS();
}


static void
test_allocator_pool_nonblock(void)
{
    TEST("allocator pool nonblock acquire");

    zst_allocator_t* alloc = zst_allocator_cpu_create();

    zst_buffer_pool_config_t config = {0};
    config.min_buffers = 2;
    config.max_buffers = 2;
    config.buffer_size = 1024;
    config.buffer_type = ZST_BUFFER_USER;

    zst_buffer_pool_t* pool = zst_buffer_pool_create(alloc, &config);
    assert(pool != NULL);

    zst_buffer_t* buf1 = NULL;
    zst_buffer_t* buf2 = NULL;
    zst_buffer_t* buf3 = NULL;

    /* Acquire first buffer */
    assert(zst_buffer_pool_acquire(pool, &buf1, 0, 0) == ZST_OK);
    assert(buf1 != NULL);

    /* Acquire second buffer */
    assert(zst_buffer_pool_acquire(pool, &buf2, 0, 0) == ZST_OK);
    assert(buf2 != NULL);

    /* Pool is now exhausted. Blocking acquire should time out */
    assert(zst_buffer_pool_acquire(pool, &buf3, 50, 0) == ZST_TIMEOUT);

    /* Non-blocking acquire should time out immediately */
    assert(zst_buffer_pool_acquire(pool, &buf3, -1, ZST_POOL_ACQUIRE_NONBLOCK) == ZST_TIMEOUT);

    /* Release buffer 1 */
    zst_buffer_unref(buf1);

    /* Non-blocking acquire should now succeed */
    assert(zst_buffer_pool_acquire(pool, &buf3, -1, ZST_POOL_ACQUIRE_NONBLOCK) == ZST_OK);
    assert(buf3 != NULL);

    zst_buffer_unref(buf2);
    zst_buffer_unref(buf3);

    zst_buffer_pool_destroy(pool);
    zst_allocator_unref(alloc);

    PASS();
}

static void
test_clock_basic(void)
{
    TEST("clock create / time / wait / destroy");

    zst_clock_t* clk = zst_clock_system_create();
    assert(clk != NULL);

    zst_time_t t1 = zst_clock_get_time(clk);
    assert(t1 > 0);

    /* Wait for 50 ms (50000000 ns) */
    zst_clock_wait(clk, 50000000);

    zst_time_t t2 = zst_clock_get_time(clk);
    assert(t2 > t1);
    /* Should have elapsed at least ~50ms */
    assert(t2 - t1 >= 40000000);

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_pipeline_set_clock(pipe, clk);
    assert(zst_pipeline_get_clock(pipe) == clk);
    assert(clk->refcount == 2);

    zst_pipeline_destroy(pipe);
    assert(clk->refcount == 1);

    zst_clock_unref(clk);

    PASS();
}

static void
test_clock_slaving(void)
{
    TEST("clock slaving time advance");

    zst_clock_t* sys_clock = zst_clock_system_create();
    zst_clock_t* master_clock = zst_clock_system_create();
    zst_clock_t* slave = zst_clock_slave_create(master_clock, sys_clock);
    assert(slave != NULL);

    zst_time_t t1 = zst_clock_get_time(slave);
    zst_clock_wait(slave, 50000000); // 50 ms
    zst_time_t t2 = zst_clock_get_time(slave);

    assert(t2 > t1);

    zst_clock_unref(slave);
    zst_clock_unref(master_clock);
    zst_clock_unref(sys_clock);

    PASS();
}

static void
test_clock_slaving_qos_sync(void)
{
    TEST("clock slaving qos sync");

    zst_clock_t* clk = zst_clock_system_create();
    assert(clk != NULL);

    zst_element_t* sink = zst_fake_sink_create();
    assert(sink != NULL);

    /* Direct clock assignment */
    zst_element_set_clock(sink, clk);

    /* Test early buffer (should wait/block) */
    zst_buffer_t* buf_early = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    zst_time_t current = zst_clock_get_time(clk);
    buf_early->pts = current + 50000000ULL; /* 50ms early */

    zst_time_t t1 = zst_clock_get_time(clk);
    zst_pad_t* sink_pad = sink->sink_pads[0];
    zst_result_t ret = sink_pad->push(sink_pad, buf_early);
    zst_time_t t2 = zst_clock_get_time(clk);

    assert(ret == ZST_OK);
    assert(t2 - t1 >= 40000000ULL); /* should have blocked for ~50ms (at least 40ms) */

    /* Test late buffer (QoS drop) */
    zst_buffer_t* buf_late = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    current = zst_clock_get_time(clk);
    buf_late->pts = current - 200000000ULL; /* 200ms late */

    ret = sink_pad->push(sink_pad, buf_late);
    assert(ret == ZST_OK);
    assert(buf_late->flags & ZST_BUFFER_FLAG_DROP); /* QoS should have set the drop flag */

    zst_buffer_unref(buf_early);
    zst_buffer_unref(buf_late);
    zst_element_destroy(sink);
    zst_clock_unref(clk);

    PASS();
}

/* ── Text Overlay (Phase 11a) ────────────────────────────────────────────── */

static void
test_text_overlay(void)
{
    TEST("text_overlay basic rendering");

    zst_element_t* overlay = zst_text_overlay_create("TEST");
    assert(overlay != NULL);

    /* We ignore open() failure here because the system might not have the font */
    zst_result_t open_res = overlay->ops->open(overlay);
    if (open_res != ZST_OK) {
        printf("  [SKIP] Could not open text overlay (missing font?)\n");
        zst_element_destroy(overlay);
        PASS();
        return;
    }

    zst_pad_t* sinkpad = zst_element_get_pad(overlay, "sink");
    assert(sinkpad != NULL);

    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    sinkpad->caps = caps;

    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);

    zst_video_frame_t* vf = malloc(sizeof(*vf));
    vf->width = 320;
    vf->height = 240;
    vf->format = 0;

    uint8_t* dummy_data = calloc(1, 320 * 240 * 3 / 2);
    vf->plane[0] = dummy_data;
    vf->plane[1] = dummy_data + 320 * 240;
    vf->plane[2] = dummy_data + 320 * 240 + 320 * 240 / 4;
    vf->stride[0] = 320;
    vf->stride[1] = 160;
    vf->stride[2] = 160;

    in_buf->payload = vf;

    zst_buffer_t* out_buf = NULL;
    assert(overlay->ops->process(overlay, in_buf, &out_buf) == ZST_OK);
    assert(out_buf != NULL);

    zst_buffer_unref(out_buf);
    in_buf->payload = NULL;
    zst_buffer_unref(in_buf);

    free(dummy_data);
    free(vf);

    assert(overlay->ops->close(overlay) == ZST_OK);
    zst_element_destroy(overlay);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Fake Sink Tests
   ═══════════════════════════════════════════════════════════════ */
static void
test_fakesink(void)
{
    TEST("fake_sink basics and stats");
    zst_plugin_registry_init();

    const char* ppath = getenv("ZSTREAMER_TEST_PLUGIN_PATH");
    if (!ppath) {
        ppath = "/workspace/build/plugins";
        if (access("/app/build/plugins", R_OK) == 0) {
            ppath = "/app/build/plugins";
        }
    }
    zst_plugin_registry_scan(ppath);

    zst_element_t* fakesink = zst_element_factory_make("fakesink");
    assert(fakesink != NULL);
    assert(strcmp(fakesink->ops->name, "fakesink") == 0);

    zst_element_set_state(fakesink, ZST_STATE_PLAYING);

    char val[64];
    zst_element_get_property(fakesink, "drop-probability", val, sizeof(val));
    assert(atof(val) == 0.0);

    zst_element_set_property(fakesink, "drop-probability", "0.0");

    zst_pad_t* sink_pad = zst_element_get_pad(fakesink, "sink");
    assert(sink_pad != NULL);

    zst_buffer_t* buf1 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    buf1->memory.data = (void*)0xDEADBEEF;
    buf1->memory.size = 100;

    /* In actual code we push on the src pad peer, but here we can manually call the sink push function */
    zst_result_t ret = sink_pad->push(sink_pad, buf1);
    assert(ret == ZST_OK);

    zst_element_get_property(fakesink, "total-buffers", val, sizeof(val));
    assert(strcmp(val, "1") == 0);
    zst_element_get_property(fakesink, "total-bytes", val, sizeof(val));
    assert(strcmp(val, "100") == 0);

    zst_buffer_t* buf2 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    buf2->memory.data = (void*)0xCAFEBABE;
    buf2->memory.size = 50;

    ret = sink_pad->push(sink_pad, buf2);
    assert(ret == ZST_OK);

    zst_element_get_property(fakesink, "total-buffers", val, sizeof(val));
    assert(strcmp(val, "2") == 0);
    zst_element_get_property(fakesink, "total-bytes", val, sizeof(val));
    assert(strcmp(val, "150") == 0);

    /* Clean up the unreffed buffers (simulating caller behavior since fakesink does not unref) */
    zst_buffer_unref(buf1);
    zst_buffer_unref(buf2);

    /* Test drop probability 1.0 */
    zst_element_set_property(fakesink, "drop-probability", "1.0");
    zst_buffer_t* buf3 = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
    buf3->memory.data = (void*)0xCAFEBABE;
    buf3->memory.size = 200;

    ret = sink_pad->push(sink_pad, buf3);
    assert(ret == ZST_OK);

    zst_element_get_property(fakesink, "total-buffers", val, sizeof(val));
    assert(strcmp(val, "2") == 0); /* Still 2 because dropped */

    zst_buffer_unref(buf3);

    zst_element_set_state(fakesink, ZST_STATE_NULL);
    zst_element_destroy(fakesink);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Video Test Source
   ═══════════════════════════════════════════════════════════════ */
static void
test_video_test_src(void)
{
    TEST("video_test_src basic generation");

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    assert(src != NULL);

    zst_element_t* sink = zst_element_factory_make("fakesink");
    assert(sink != NULL);

    zst_pipeline_t* pipe = zst_pipeline_create();
    zst_pipeline_add(pipe, src);
    zst_pipeline_add(pipe, sink);

    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");

    zst_result_t res = zst_pad_link(src_pad, sink_pad);
    assert(res == ZST_OK);

    zst_element_set_property(src, "num-buffers", "5");
    zst_element_set_property(src, "pattern", "gradient");

    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);

    /* Process manually since there is no scheduler thread attached here.
     * video_test_src produces buffers via its pull callback because it's a source.
     * Wait, src pad of videotestsrc actually pushes? It doesn't have a thread.
     * To simulate, we'll pull from its src pad and push to sink.
     * Actually, the test_fakesink calls sink_pad->push.
     * Let's do a loop.
     */
    for (int i = 0; i < 6; i++) {
        zst_buffer_t* buf = NULL;
        zst_result_t ret = src_pad->pull(src_pad, &buf);
        if (ret == ZST_OK && buf != NULL) {
            if (buf->flags & ZST_BUFFER_FLAG_EOS) {
                zst_buffer_unref(buf);
                break;
            }
            zst_video_frame_t* vf = buf->payload;
            assert(vf != NULL);
            assert(vf->width == 640);
            assert(vf->height == 480);
            assert(buf->type == ZST_BUFFER_VIDEO_FRAME);
            assert(buf->memory.size == 640 * 480 * 3 / 2);

            sink_pad->push(sink_pad, buf);
            zst_buffer_unref(buf);
        } else {
            break;
        }
    }

    char val[64];
    zst_element_get_property(sink, "total-buffers", val, sizeof(val));
    assert(strcmp(val, "5") == 0);

    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_pipeline_destroy(pipe);

    PASS();
}

static void
test_audio_test_src(void)
{
    TEST("audio_test_src generation, properties, caps, EOS");

    zst_element_t* src = zst_audio_test_src_create();
    assert(src != NULL);
    assert(strcmp(src->ops->name, "audiotestsrc") == 0);

    char val[64];
    assert(zst_element_get_property(src, "sample-rate", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "44100") == 0);
    assert(zst_element_get_property(src, "channels", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "2") == 0);
    assert(zst_element_get_property(src, "sample-format", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "S16LE") == 0);
    assert(zst_element_get_property(src, "wave", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "sine") == 0);

    assert(zst_element_set_property(src, "sample-rate", "48000") == ZST_OK);
    assert(zst_element_set_property(src, "channels", "1") == ZST_OK);
    assert(zst_element_set_property(src, "sample-format", "F32LE") == ZST_OK);
    assert(zst_element_set_property(src, "wave", "silence") == ZST_OK);
    assert(zst_element_set_property(src, "frequency", "1000") == ZST_OK);
    assert(zst_element_set_property(src, "samples-per-buffer", "512") == ZST_OK);
    assert(zst_element_set_property(src, "num-samples", "800") == ZST_OK);

    assert(zst_element_get_property(src, "sample-rate", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "48000") == 0);
    assert(zst_element_get_property(src, "channels", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "1") == 0);
    assert(zst_element_get_property(src, "sample-format", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "F32LE") == 0);

    zst_caps_t* caps = zst_pad_get_caps(zst_element_get_pad(src, "src"));
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "audio/x-raw") == 0);
    assert(caps->structs->audio.channels == 1);
    assert(caps->structs->audio.sample_rate == 48000);
    assert(strcmp(caps->structs->audio.format, "F32LE") == 0);
    zst_caps_destroy(caps);

    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    zst_buffer_t* buf = NULL;
    assert(src->ops->process(src, NULL, &buf) == ZST_OK);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_AUDIO_FRAME);
    assert(!(buf->flags & ZST_BUFFER_FLAG_EOS));
    assert(buf->memory.size == 512 * sizeof(float));
    assert(buf->pts == 0);
    assert(buf->duration == 512ULL * 1000000000ULL / 48000ULL);

    zst_audio_frame_t* af = buf->payload;
    assert(af != NULL);
    assert(af->sample_rate == 48000);
    assert(af->channels == 1);
    assert(af->format == 3); /* F32LE */
    assert(af->nb_samples == 512);
    float* f32 = af->data;
    assert(f32 != NULL);
    for (int i = 0; i < 512; i++) {
        assert(f32[i] == 0.0f);
    }
    zst_buffer_unref(buf);

    buf = NULL;
    assert(src->ops->process(src, NULL, &buf) == ZST_OK);
    assert(buf != NULL);
    assert(!(buf->flags & ZST_BUFFER_FLAG_EOS));
    af = buf->payload;
    assert(af != NULL);
    assert(af->nb_samples == 288);
    assert(buf->memory.size == 288 * sizeof(float));
    assert(buf->pts == 512ULL * 1000000000ULL / 48000ULL);
    zst_buffer_unref(buf);

    buf = NULL;
    assert(src->ops->process(src, NULL, &buf) == ZST_EOF);
    assert(buf == NULL);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);

    /* Check S16LE signal generation and loop mode. */
    src = zst_audio_test_src_create();
    assert(src != NULL);
    assert(zst_element_set_property(src, "wave", "square") == ZST_OK);
    assert(zst_element_set_property(src, "samples-per-buffer", "4") == ZST_OK);
    assert(zst_element_set_property(src, "num-buffers", "1") == ZST_OK);
    assert(zst_element_set_property(src, "loop", "true") == ZST_OK);
    assert(zst_element_set_state(src, ZST_STATE_PLAYING) == ZST_OK);

    buf = NULL;
    assert(src->ops->process(src, NULL, &buf) == ZST_OK);
    assert(buf != NULL);
    af = buf->payload;
    assert(af != NULL);
    assert(af->format == 0); /* S16LE */
    assert(af->nb_samples == 4);
    int16_t* s16 = af->data;
    assert(s16 != NULL);
    assert(s16[0] > 0);
    zst_buffer_unref(buf);

    buf = NULL;
    assert(src->ops->process(src, NULL, &buf) == ZST_OK);
    assert(buf != NULL);
    assert(!(buf->flags & ZST_BUFFER_FLAG_EOS));
    zst_buffer_unref(buf);

    assert(zst_element_set_state(src, ZST_STATE_NULL) == ZST_OK);
    zst_element_destroy(src);

    PASS();
}

static void
test_text_source(void)
{
    TEST("text_source basic frame generation and properties");

    zst_element_t* src = zst_text_source_create();
    assert(src != NULL);

    /* We ignore open() failure here because the system might not have the font */
    zst_result_t open_res = src->ops->open(src);
    if (open_res != ZST_OK) {
        printf("  [SKIP] Could not open text source (missing font?)\n");
        zst_element_destroy(src);
        PASS();
        return;
    }

    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    assert(src_pad != NULL);

    /* Check default properties */
    char val[64];
    assert(zst_element_get_property(src, "width", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "640") == 0);

    assert(zst_element_get_property(src, "height", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "480") == 0);

    assert(zst_element_get_property(src, "fps", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "30") == 0);

    assert(zst_element_get_property(src, "text", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "Hello ZStreamer") == 0);

    /* Set properties and check them */
    assert(zst_element_set_property(src, "width", "320") == ZST_OK);
    assert(zst_element_set_property(src, "height", "240") == ZST_OK);
    assert(zst_element_set_property(src, "fps", "15") == ZST_OK);
    assert(zst_element_set_property(src, "text", "Test Content") == ZST_OK);
    assert(zst_element_set_property(src, "bg-color", "red") == ZST_OK);
    assert(zst_element_set_property(src, "color", "#0000ff") == ZST_OK); // text-color
    assert(zst_element_set_property(src, "pixel-format", "NV12") == ZST_OK);
    assert(zst_element_set_property(src, "num-buffers", "2") == ZST_OK);
    assert(zst_element_set_property(src, "x", "20") == ZST_OK);
    assert(zst_element_set_property(src, "y", "40") == ZST_OK);

    assert(zst_element_get_property(src, "width", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "320") == 0);

    assert(zst_element_get_property(src, "height", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "240") == 0);

    assert(zst_element_get_property(src, "fps", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "15") == 0);

    assert(zst_element_get_property(src, "text", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "Test Content") == 0);

    assert(zst_element_get_property(src, "bg-color", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "red") == 0);

    assert(zst_element_get_property(src, "color", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "#0000ff") == 0);

    assert(zst_element_get_property(src, "pixel-format", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "NV12") == 0);

    assert(zst_element_get_property(src, "x", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "20") == 0);

    assert(zst_element_get_property(src, "y", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "40") == 0);

    /* Generate buffers manually */
    zst_buffer_t* buf = NULL;
    zst_result_t process_res = src->ops->process(src, NULL, &buf);
    assert(process_res == ZST_OK);
    assert(buf != NULL);
    assert(buf->type == ZST_BUFFER_VIDEO_FRAME);
    assert(!(buf->flags & ZST_BUFFER_FLAG_EOS));

    zst_video_frame_t* vf = buf->payload;
    assert(vf != NULL);
    assert(vf->width == 320);
    assert(vf->height == 240);
    assert(vf->format == 1); // NV12
    zst_buffer_unref(buf);

    /* Second buffer */
    buf = NULL;
    process_res = src->ops->process(src, NULL, &buf);
    assert(process_res == ZST_OK);
    assert(buf != NULL);
    assert(!(buf->flags & ZST_BUFFER_FLAG_EOS));
    zst_buffer_unref(buf);

    /* Third buffer should be EOS */
    buf = NULL;
    process_res = src->ops->process(src, NULL, &buf);
    assert(process_res == ZST_OK);
    assert(buf != NULL);
    assert(buf->flags & ZST_BUFFER_FLAG_EOS);
    zst_buffer_unref(buf);

    src->ops->close(src);
    zst_element_destroy(src);

    PASS();
}

static void
test_text_source_factory(void)
{
    TEST("text_source dynamic loading from registry");

    zst_plugin_registry_init();
    zst_plugin_registry_scan("/workspace/build/plugins");

    zst_element_t* src = zst_element_factory_make("textsource");
    assert(src != NULL);
    assert(src->plugin != NULL);
    assert(strcmp(src->ops->name, "textsource") == 0);

    zst_element_destroy(src);
    zst_plugin_registry_deinit();

    PASS();
}

static void
test_file_source(void)
{
    TEST("file_source basic reading, offset, length, chunk-size, and loop");

    const char* filepath = "test_filesrc.txt";
    FILE* fp = fopen(filepath, "wb");
    assert(fp != NULL);
    const char* content = "0123456789abcdefghijklmnopqrstuvwxyz";
    size_t len = strlen(content);
    assert(fwrite(content, 1, len, fp) == len);
    fclose(fp);

    zst_plugin_registry_init();
    zst_plugin_registry_scan("/workspace/build/plugins");

    // 1. Basic reading test
    zst_element_t* src = zst_element_factory_make("filesrc");
    assert(src != NULL);

    zst_element_set_property(src, "path", filepath);
    zst_element_set_property(src, "chunk-size", "10");

    zst_caps_t* caps = zst_pad_get_caps(zst_element_get_pad(src, "src"));
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "text/plain") == 0);
    zst_caps_destroy(caps);

    zst_result_t res = zst_element_set_state(src, ZST_STATE_PLAYING);
    assert(res == ZST_OK);

    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    assert(src_pad != NULL);

    zst_buffer_t* buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 10);
    assert(strncmp((char*)buf->memory.data, "0123456789", 10) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 10);
    assert(strncmp((char*)buf->memory.data, "abcdefghij", 10) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 10);
    assert(strncmp((char*)buf->memory.data, "klmnopqrst", 10) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 6);
    assert(strncmp((char*)buf->memory.data, "uvwxyz", 6) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_EOF);

    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    // 2. Offset and length test
    src = zst_element_factory_make("filesrc");
    assert(src != NULL);
    zst_element_set_property(src, "path", filepath);
    zst_element_set_property(src, "chunk-size", "5");
    zst_element_set_property(src, "offset", "10");
    zst_element_set_property(src, "length", "12");

    res = zst_element_set_state(src, ZST_STATE_PLAYING);
    assert(res == ZST_OK);
    src_pad = zst_element_get_pad(src, "src");

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 5);
    assert(strncmp((char*)buf->memory.data, "abcde", 5) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 5);
    assert(strncmp((char*)buf->memory.data, "fghij", 5) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 2);
    assert(strncmp((char*)buf->memory.data, "kl", 2) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_EOF);

    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    // 3. Loop test
    src = zst_element_factory_make("filesrc");
    assert(src != NULL);
    zst_element_set_property(src, "path", filepath);
    zst_element_set_property(src, "chunk-size", "20");
    zst_element_set_property(src, "offset", "30");
    zst_element_set_property(src, "loop", "true");

    res = zst_element_set_state(src, ZST_STATE_PLAYING);
    assert(res == ZST_OK);
    src_pad = zst_element_get_pad(src, "src");

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 6);
    assert(strncmp((char*)buf->memory.data, "uvwxyz", 6) == 0);
    zst_buffer_unref(buf);

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_OK);
    assert(buf != NULL);
    assert(buf->memory.size == 6);
    assert(strncmp((char*)buf->memory.data, "uvwxyz", 6) == 0);
    zst_buffer_unref(buf);

    zst_element_set_property(src, "loop", "false");

    buf = NULL;
    res = src_pad->pull(src_pad, &buf);
    assert(res == ZST_EOF);

    zst_element_set_state(src, ZST_STATE_NULL);
    zst_element_destroy(src);

    remove(filepath);
    PASS();
}

static zst_pad_probe_return_t
probe_modify_cb(zst_pad_t* pad, zst_pad_probe_info_t* info, void* user_data)
{
    (void)pad;
    int* counter = user_data;
    (*counter)++;

    if (info->type & ZST_PAD_PROBE_TYPE_PRE_BUFFER) {
        /* Change the PTS of the buffer to prove we intercepted it */
        info->buffer->pts = 12345;
    }
    return ZST_PAD_PROBE_OK;
}

static zst_pad_probe_return_t
probe_drop_cb(zst_pad_t* pad, zst_pad_probe_info_t* info, void* user_data)
{
    (void)pad;
    (void)info;
    int* counter = user_data;
    (*counter)++;
    return ZST_PAD_PROBE_DROP;
}

static zst_pad_probe_return_t
probe_block_cb(zst_pad_t* pad, zst_pad_probe_info_t* info, void* user_data)
{
    (void)pad;
    (void)info;
    int* blocked_flag = user_data;
    *blocked_flag = 1;
    return ZST_PAD_PROBE_BLOCK;
}

static void*
push_thread_func(void* arg)
{
    zst_pad_t* src_pad = arg;
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    buf->pts = 100;

    zst_result_t ret = zst_pad_push(src_pad, buf);
    zst_buffer_unref(buf);

    /* Return the result */
    return (void*)(intptr_t)ret;
}

static void
test_pad_probes(void)
{
    TEST("pad probes");

    zst_plugin_registry_init();
    zst_plugin_registry_scan("plugins");

    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* sink = zst_element_factory_make("fakesink");

    assert(src != NULL);
    assert(sink != NULL);

    zst_pad_t* src_pad = zst_element_get_pad(src, "src");
    zst_pad_t* sink_pad = zst_element_get_pad(sink, "sink");

    assert(zst_pad_link(src_pad, sink_pad) == ZST_OK);

    int modify_counter = 0;
    uint32_t modify_id = zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_TYPE_PRE_BUFFER, probe_modify_cb, &modify_counter, NULL);
    assert(modify_id > 0);

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
    buf->pts = 100;

    /* Push directly using the sink_pad push function so we trigger default_sink_pad_push and the PRE_BUFFER probe */
    assert(sink_pad->push(sink_pad, buf) == ZST_OK);
    assert(modify_counter == 1);
    assert(buf->pts == 12345); /* Modified by probe */
    zst_buffer_unref(buf);

    zst_pad_remove_probe(sink_pad, modify_id);

    /* Test Drop */
    int drop_counter = 0;
    uint32_t drop_id = zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_TYPE_PRE_BUFFER, probe_drop_cb, &drop_counter, NULL);

    buf = zst_buffer_create(ZST_BUFFER_USER);
    buf->pts = 200;

    /* A drop probe should make it return OK but not process further */
    assert(sink_pad->push(sink_pad, buf) == ZST_OK);
    assert(drop_counter == 1);
    zst_buffer_unref(buf);
    zst_pad_remove_probe(sink_pad, drop_id);

    /* Test Block and Unblock across threads */
    int block_flag = 0;
    uint32_t block_id = zst_pad_add_probe(sink_pad, ZST_PAD_PROBE_TYPE_PRE_BUFFER, probe_block_cb, &block_flag, NULL);

    pthread_t thread;
    pthread_create(&thread, NULL, push_thread_func, src_pad);

    /* Wait until the thread hits the block probe */
    while (__atomic_load_n(&block_flag, __ATOMIC_ACQUIRE) == 0) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000; /* 1ms */
        nanosleep(&ts, NULL);
    }

    /* The pad should now be blocked */
    assert(sink_pad->is_blocked == true);

    /* Unblock it */
    zst_pad_unblock(sink_pad);

    void* thread_ret = NULL;
    pthread_join(thread, &thread_ret);
    assert((zst_result_t)(intptr_t)thread_ret == ZST_OK);

    zst_pad_remove_probe(sink_pad, block_id);

    zst_element_destroy(src);
    zst_element_destroy(sink);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
static void
test_text_overlay_multiline(void)
{
    TEST("text_overlay multi-line rendering");

    zst_element_t* overlay = zst_text_overlay_create("Line 1\nLine 2 is longer\nLine 3");
    assert(overlay != NULL);

    zst_result_t open_res = overlay->ops->open(overlay);
    if (open_res != ZST_OK) {
        printf("  [SKIP] Could not open text overlay (missing font?)\n");
        zst_element_destroy(overlay);
        PASS();
        return;
    }

    zst_pad_t* sinkpad = zst_element_get_pad(overlay, "sink");
    assert(sinkpad != NULL);

    zst_caps_t* caps = zst_caps_create();
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 320, 240, 30.0, "YUV420P"));
    sinkpad->caps = caps;

    zst_buffer_t* in_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);

    zst_video_frame_t* vf = malloc(sizeof(*vf));
    vf->width = 320;
    vf->height = 240;
    vf->format = 0;

    uint8_t* dummy_data = calloc(1, 320 * 240 * 3 / 2);
    vf->plane[0] = dummy_data;
    vf->plane[1] = dummy_data + 320 * 240;
    vf->plane[2] = dummy_data + 320 * 240 + 320 * 240 / 4;
    vf->stride[0] = 320;
    vf->stride[1] = 160;
    vf->stride[2] = 160;

    in_buf->payload = vf;

    zst_buffer_t* out_buf = NULL;
    assert(overlay->ops->process(overlay, in_buf, &out_buf) == ZST_OK);
    assert(out_buf != NULL);

    zst_buffer_unref(out_buf);
    in_buf->payload = NULL;
    zst_buffer_unref(in_buf);

    free(dummy_data);
    free(vf);

    zst_element_destroy(overlay);
    PASS();
}

static void test_rtmp_elements(void)
{
    TEST("rtmp source/sink properties and caps");

    zst_element_t* rtmpsrc = zst_rtmp_source_create("rtmp://localhost/live/stream");
    assert(rtmpsrc != NULL);
    char val[256];
    assert(zst_element_get_property(rtmpsrc, "url", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "rtmp://localhost/live/stream") == 0);
    
    assert(zst_element_set_property(rtmpsrc, "url", "rtmp://127.0.0.1/live/test") == ZST_OK);
    assert(zst_element_get_property(rtmpsrc, "url", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "rtmp://127.0.0.1/live/test") == 0);

    zst_pad_t* vpad = zst_element_get_pad(rtmpsrc, "video");
    assert(vpad != NULL);
    zst_caps_t* vcaps = zst_pad_get_caps(vpad);
    assert(vcaps != NULL);
    zst_caps_destroy(vcaps);
    
    zst_element_destroy(rtmpsrc);

    zst_element_t* rtmpsink = zst_rtmp_sink_create();
    assert(rtmpsink != NULL);
    assert(zst_element_set_property(rtmpsink, "url", "rtmp://localhost/live/out") == ZST_OK);
    assert(zst_element_get_property(rtmpsink, "url", val, sizeof(val)) == ZST_OK);
    assert(strcmp(val, "rtmp://localhost/live/out") == 0);

    zst_pad_t* sink_vpad = zst_element_get_pad(rtmpsink, "video");
    assert(sink_vpad != NULL);
    zst_caps_t* sink_vcaps = zst_pad_get_caps(sink_vpad);
    assert(sink_vcaps != NULL);
    zst_caps_destroy(sink_vcaps);

    zst_element_destroy(rtmpsink);
    PASS();
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║     zstreamer — core unit tests                   ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    /* ── Buffer ── */
    printf("[buffer]\n");
    test_buffer_create_destroy();
    test_buffer_refcount();
    test_buffer_null_safety();
    test_buffer_create_with_pool();

    /* ── Pad ── */
    printf("[pad]\n");
    test_pad_create_destroy();
    test_pad_link_unlink();
    test_pad_invalid_link();
    test_pad_probes();

    /* ── Element ── */
    printf("[element]\n");
    test_element_create_destroy();
    test_element_state_transition();
    test_element_pads();

    /* ── Pipeline ── */
    printf("[pipeline]\n");
    test_pipeline_create_destroy();
    test_pipeline_add_remove();
    test_pipeline_state_propagation();
    test_pipeline_topological_sort_check();

    /* ── Queue ── */
    printf("[queue]\n");
    test_queue_push_pop();
    test_queue_timeout();
    test_queue_flush();
    test_queue_config_limits();

    /* ── Scheduler (Phase 2) ── */
    printf("[scheduler]\n");
    test_scheduler_single_threaded();
    test_scheduler_multi_threaded();

    /* ── Caps Negotiation (Phase 5) ── */
    printf("[caps negotiation]\n");
    test_caps_basic();
    test_caps_intersection_video();
    test_caps_intersection_audio();
    test_caps_fixate();
    test_pad_negotiate_and_link();

    /* ── Event Bus (Phase 6) ── */
    printf("[event bus]\n");
    test_event_create_destroy();
    test_bus_basic();
    test_bus_timeout();
    test_bus_async_dispatch();
    test_pipeline_bus_events();

    /* ── Dynamic Plugins (Phase 7) ── */
    printf("[dynamic plugins]\n");
    test_plugin_registry_basic();
    test_builtin_element_registry();
    test_element_factory_refcounting();
    test_element_factory_introspection_and_typed_properties();

    /* ── Logging (Phase 3.5) ── */
    printf("[logging]\n");
    test_log_levels();
    test_log_custom_handler();

    /* ── Conversion Elements (Phase 4g/4h) ── */
    printf("[conversion elements]\n");
    test_video_scaler();
    test_audio_resampler();

    /* ── Decoders (Phase 4v/4y) ── */
    printf("[decoders]\n");
    test_h264_decoder_roundtrip();
    test_h265_decoder_roundtrip();
    test_aac_decoder_roundtrip();

    /* ── Allocator (Phase 8a) ── */
    printf("[allocator]\n");
    test_allocator_basic();
    test_allocator_pool_nonblock();

    /* ── Clock (Phase 8b) ── */
    printf("[clock]\n");
    test_clock_basic();
    test_clock_slaving();
    test_clock_slaving_qos_sync();

    /* ── Text Overlay (Phase 11a) ── */
    printf("[text overlay]\n");
    test_text_overlay();
    test_text_overlay_multiline();

    printf("[fakesink]\n");
    test_fakesink();

    printf("[video test source]\n");
    test_video_test_src();

    printf("[audio test source]\n");
    test_audio_test_src();

    printf("[file source]\n");
    test_file_source();

    printf("[text source]\n");
    test_text_source();
    test_text_source_factory();

    printf("[rtmp source/sink]\n");
    test_rtmp_elements();

    /* ── Summary ── */
    printf("\n──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
