/*=============================================================================
    test_queue.c — Unit / smoke tests for the zstreamer queue
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_pad.h"
#include "zst_element.h"
#include "zst_pipeline.h"
#include "zst_queue.h"
#include "zst_scheduler.h"
#include "zst_buffer_pool.h"

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

static void
mock_buf_destroy(zst_buffer_t* b)
{
    free(b->payload);
}

static void
mock_buf_memory_destroy(zst_buffer_t* b)
{
    free(b->memory.data);
}

typedef struct {
    zst_pad_t* dummy_src;
    int producer_id;
    int count;
} prod_ctx_t;

static void*
producer_thread(void* arg)
{
    prod_ctx_t* ctx = arg;
    assert(ctx->dummy_src != NULL);

    for (int i = 0; i < ctx->count; i++) {
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
        assert(buf != NULL);

        uint64_t* val = malloc(sizeof(uint64_t));
        assert(val != NULL);
        *val = (uint64_t)(ctx->producer_id * 1000000 + i);
        buf->payload = val;
        buf->destroy = mock_buf_destroy;

        zst_result_t r = zst_pad_push(ctx->dummy_src, buf);
        assert(r == ZST_OK);

        zst_buffer_unref(buf);
    }
    return NULL;
}

static void*
pool_producer_thread(void* arg)
{
    prod_ctx_t* ctx = arg;
    assert(ctx->dummy_src != NULL);

    for (int i = 0; i < ctx->count; i++) {
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
        assert(buf != NULL);

        buf->memory.data = malloc(128);
        assert(buf->memory.data != NULL);
        buf->memory.size = 128;
        memset(buf->memory.data, 0xAA, 128);
        buf->destroy = mock_buf_memory_destroy;

        zst_result_t r = zst_pad_push(ctx->dummy_src, buf);
        assert(r == ZST_OK);

        zst_buffer_unref(buf);
    }
    return NULL;
}

typedef struct {
    pthread_mutex_t lock;
    int count;
    uint64_t sum;
} test_sink_state_t;

static zst_result_t
test_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    (void)pad;
    test_sink_state_t* state = pad->parent ? pad->parent->priv : NULL;
    if (!state) return ZST_ERROR;

    if (buf && (buf->flags & ZST_BUFFER_FLAG_EOS)) {
        return ZST_OK;
    }
    if (buf && buf->payload) {
        uint64_t val = *(uint64_t*)buf->payload;
        pthread_mutex_lock(&state->lock);
        state->count++;
        state->sum += val;
        pthread_mutex_unlock(&state->lock);
    }
    return ZST_OK;
}

typedef struct {
    pthread_mutex_t lock;
    int count;
    zst_buffer_pool_t* pool;
} test_pool_sink_state_t;

static zst_result_t
test_pool_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    (void)pad;
    test_pool_sink_state_t* state = pad->parent ? pad->parent->priv : NULL;
    if (!state) return ZST_ERROR;

    if (buf && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        assert(buf->pool == state->pool);
        pthread_mutex_lock(&state->lock);
        state->count++;
        pthread_mutex_unlock(&state->lock);
    }
    return ZST_OK;
}

typedef struct {
    zst_pad_t* dummy_src;
    volatile int running;
} state_stress_ctx_t;

static void*
state_stress_producer(void* arg)
{
    state_stress_ctx_t* ctx = arg;
    assert(ctx->dummy_src != NULL);

    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
        if (!buf) continue;

        zst_pad_push(ctx->dummy_src, buf);
        zst_buffer_unref(buf);

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 }; /* 0.1ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static zst_result_t
dummy_open(zst_element_t* el) { (void)el; return ZST_OK; }

static zst_element_ops_t g_dummy_ops = {
    .name   = "dummy",
    .open   = dummy_open,
};

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
test_queue_fast_path(void)
{
    TEST("queue fast path — has_extra_limits false");

    /* power-of-2 max_buffers == capacity, no byte/duration limits */
    zst_queue_config_t cfg = {
        .mode        = ZST_QUEUE_SYNC,
        .max_buffers = 8,
        .max_bytes   = 0,
        .max_duration= 0,
    };
    zst_queue_t* q = zst_queue_create(&cfg);
    assert(q != NULL);

    zst_buffer_t* bufs[10];
    for (int i = 0; i < 8; i++) {
        bufs[i] = zst_buffer_create(ZST_BUFFER_USER);
        assert(zst_queue_push(q, bufs[i], 100) == ZST_OK);
    }

    /* 9th push should time out via sequence array backpressure */
    zst_buffer_t* overflow = zst_buffer_create(ZST_BUFFER_USER);
    assert(zst_queue_push(q, overflow, 10) == ZST_TIMEOUT);
    zst_buffer_unref(overflow);

    /* Pop back and verify all 8 */
    for (int i = 0; i < 8; i++) {
        zst_buffer_t* out;
        assert(zst_queue_pop(q, &out, 100) == ZST_OK);
        zst_buffer_unref(out);
        zst_buffer_unref(bufs[i]);
    }

    /* Verify queue is now empty */
    assert(zst_queue_size(q) == 0);

    zst_queue_destroy(q);
    PASS();
}

static void
test_queue_element_stress(void)
{
    TEST("queue element stress (concurrency, lifecycle, buffer pool)");

    const int NUM_PRODUCERS = 4;
    const int BUFFERS_PER_PRODUCER = 500;
    const int TOTAL_BUFFERS = NUM_PRODUCERS * BUFFERS_PER_PRODUCER;

    /* ── Part 1: Sync Mode Concurrency & Order Verification ── */
    {
        zst_queue_config_t q_cfg = {
            .mode = ZST_QUEUE_SYNC,
            .max_buffers = 20,
            .max_bytes = 0,
            .max_duration = 0,
        };

        zst_element_t* q_el = zst_queue_element_create(&q_cfg);
        assert(q_el != NULL);

        test_sink_state_t* sink_state = calloc(1, sizeof(test_sink_state_t));
        assert(sink_state != NULL);
        pthread_mutex_init(&sink_state->lock, NULL);
        sink_state->count = 0;
        sink_state->sum = 0;

        // Create a dummy element to parent our sink pad
        zst_element_t* dummy_sink = zst_element_create(&g_dummy_ops, sink_state);
        zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
        sink_pad->push = test_sink_push;
        zst_element_add_pad(dummy_sink, sink_pad);

        zst_pad_t* q_src = zst_element_get_pad(q_el, "src");
        assert(q_src != NULL);
        assert(zst_pad_link(q_src, sink_pad) == ZST_OK);

        // Create dummy src pad to push into queue element's sink
        zst_pad_t* dummy_src = zst_pad_create("src", ZST_PAD_SRC);
        zst_pad_t* q_sink = zst_element_get_pad(q_el, "sink");
        assert(dummy_src != NULL && q_sink != NULL);
        assert(zst_pad_link(dummy_src, q_sink) == ZST_OK);

        assert(zst_element_set_state(q_el, ZST_STATE_READY) == ZST_OK);
        assert(zst_element_set_state(q_el, ZST_STATE_PLAYING) == ZST_OK);

        pthread_t producers[NUM_PRODUCERS];
        prod_ctx_t prod_contexts[NUM_PRODUCERS];

        for (int i = 0; i < NUM_PRODUCERS; i++) {
            prod_contexts[i].dummy_src = dummy_src;
            prod_contexts[i].producer_id = i;
            prod_contexts[i].count = BUFFERS_PER_PRODUCER;
            assert(pthread_create(&producers[i], NULL, producer_thread, &prod_contexts[i]) == 0);
        }

        for (int i = 0; i < NUM_PRODUCERS; i++) {
            pthread_join(producers[i], NULL);
        }

        int wait_limit = 2000;
        while (wait_limit > 0) {
            pthread_mutex_lock(&sink_state->lock);
            int count = sink_state->count;
            pthread_mutex_unlock(&sink_state->lock);
            if (count >= TOTAL_BUFFERS) {
                break;
            }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
            wait_limit--;
        }

        assert(zst_element_set_state(q_el, ZST_STATE_NULL) == ZST_OK);

        pthread_mutex_lock(&sink_state->lock);
        assert(sink_state->count == TOTAL_BUFFERS);
        uint64_t expected_sum = 0;
        for (int p = 0; p < NUM_PRODUCERS; p++) {
            for (int i = 0; i < BUFFERS_PER_PRODUCER; i++) {
                expected_sum += (uint64_t)(p * 1000000 + i);
            }
        }
        assert(sink_state->sum == expected_sum);
        pthread_mutex_unlock(&sink_state->lock);

        pthread_mutex_destroy(&sink_state->lock);
        zst_pad_unlink(q_src);
        zst_pad_unlink(dummy_src);
        zst_pad_destroy(dummy_src);
        zst_element_destroy(q_el);
        zst_element_destroy(dummy_sink); // this frees sink_state
    }

    /* ── Part 2: Concurrent State Transitions & Flush Stress ── */
    {
        zst_queue_config_t q_cfg = {
            .mode = ZST_QUEUE_SYNC,
            .max_buffers = 5,
            .max_bytes = 0,
            .max_duration = 0,
        };

        zst_element_t* q_el = zst_queue_element_create(&q_cfg);
        assert(q_el != NULL);

        zst_pad_t* dummy_src = zst_pad_create("src", ZST_PAD_SRC);
        zst_pad_t* q_sink = zst_element_get_pad(q_el, "sink");
        assert(dummy_src != NULL && q_sink != NULL);
        assert(zst_pad_link(dummy_src, q_sink) == ZST_OK);

        // Keep it in READY so the queue exists, then start producer
        assert(zst_element_set_state(q_el, ZST_STATE_READY) == ZST_OK);

        state_stress_ctx_t stress_ctx;
        stress_ctx.dummy_src = dummy_src;
        __atomic_store_n(&stress_ctx.running, 1, __ATOMIC_RELEASE);

        pthread_t prod_thread;
        assert(pthread_create(&prod_thread, NULL, state_stress_producer, &stress_ctx) == 0);

        // Rapid state transitions between PLAYING and READY
        for (int i = 0; i < 50; i++) {
            assert(zst_element_set_state(q_el, ZST_STATE_PLAYING) == ZST_OK);
            struct timespec ts1 = { .tv_sec = 0, .tv_nsec = 500000 }; /* 0.5ms */
            nanosleep(&ts1, NULL);
            assert(zst_element_set_state(q_el, ZST_STATE_READY) == ZST_OK);
            struct timespec ts2 = { .tv_sec = 0, .tv_nsec = 500000 }; /* 0.5ms */
            nanosleep(&ts2, NULL);
        }

        // Stop the producer
        __atomic_store_n(&stress_ctx.running, 0, __ATOMIC_RELEASE);
        pthread_join(prod_thread, NULL);

        // Finally clean up state
        assert(zst_element_set_state(q_el, ZST_STATE_NULL) == ZST_OK);
        zst_pad_unlink(dummy_src);
        zst_pad_destroy(dummy_src);
        zst_element_destroy(q_el);
    }

    /* ── Part 3: Buffer Pool Integration Stress ── */
    {
        zst_queue_config_t q_cfg = {
            .mode = ZST_QUEUE_SYNC,
            .max_buffers = 50,
            .max_bytes = 0,
            .max_duration = 0,
        };
        zst_element_t* q_el = zst_queue_element_create(&q_cfg);
        assert(q_el != NULL);

        zst_buffer_pool_config_t pool_cfg = {
            .min_buffers = 10,
            .max_buffers = 30,
            .buffer_size = 512,
            .buffer_type = ZST_BUFFER_USER
        };
        zst_buffer_pool_t* pool = zst_buffer_pool_create(NULL, &pool_cfg);
        assert(pool != NULL);
        zst_buffer_pool_prefill(pool);

        assert(zst_queue_element_set_pool(q_el, pool) == ZST_OK);

        test_pool_sink_state_t* pool_sink_state = calloc(1, sizeof(test_pool_sink_state_t));
        assert(pool_sink_state != NULL);
        pthread_mutex_init(&pool_sink_state->lock, NULL);
        pool_sink_state->count = 0;
        pool_sink_state->pool = pool;

        zst_element_t* dummy_sink = zst_element_create(&g_dummy_ops, pool_sink_state);
        zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
        sink_pad->push = test_pool_sink_push;
        zst_element_add_pad(dummy_sink, sink_pad);

        zst_pad_t* q_src = zst_element_get_pad(q_el, "src");
        assert(q_src != NULL);
        assert(zst_pad_link(q_src, sink_pad) == ZST_OK);

        zst_pad_t* dummy_src = zst_pad_create("src", ZST_PAD_SRC);
        zst_pad_t* q_sink = zst_element_get_pad(q_el, "sink");
        assert(dummy_src != NULL && q_sink != NULL);
        assert(zst_pad_link(dummy_src, q_sink) == ZST_OK);

        assert(zst_element_set_state(q_el, ZST_STATE_READY) == ZST_OK);
        assert(zst_element_set_state(q_el, ZST_STATE_PLAYING) == ZST_OK);

        pthread_t producers[NUM_PRODUCERS];
        prod_ctx_t prod_contexts[NUM_PRODUCERS];

        for (int i = 0; i < NUM_PRODUCERS; i++) {
            prod_contexts[i].dummy_src = dummy_src;
            prod_contexts[i].producer_id = i;
            prod_contexts[i].count = BUFFERS_PER_PRODUCER;
            assert(pthread_create(&producers[i], NULL, pool_producer_thread, &prod_contexts[i]) == 0);
        }

        for (int i = 0; i < NUM_PRODUCERS; i++) {
            pthread_join(producers[i], NULL);
        }

        int wait_limit = 2000;
        while (wait_limit > 0) {
            pthread_mutex_lock(&pool_sink_state->lock);
            int count = pool_sink_state->count;
            pthread_mutex_unlock(&pool_sink_state->lock);
            if (count >= TOTAL_BUFFERS) {
                break;
            }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
            wait_limit--;
        }

        assert(zst_element_set_state(q_el, ZST_STATE_NULL) == ZST_OK);

        pthread_mutex_lock(&pool_sink_state->lock);
        assert(pool_sink_state->count == TOTAL_BUFFERS);
        pthread_mutex_unlock(&pool_sink_state->lock);

        pthread_mutex_destroy(&pool_sink_state->lock);
        zst_pad_unlink(q_src);
        zst_pad_unlink(dummy_src);
        zst_pad_destroy(dummy_src);
        zst_element_destroy(q_el);
        zst_element_destroy(dummy_sink); // this frees pool_sink_state
        zst_buffer_pool_destroy(pool);
    }

    PASS();
}


int main()
{
    printf("Starting ZStreamer Queue Core Tests...\n");
    printf("──────────────────────────────────────────────────\n");

    test_queue_push_pop();
    test_queue_timeout();
    test_queue_flush();
    test_queue_config_limits();
    test_queue_fast_path();
    test_queue_element_stress();

    printf("──────────────────────────────────────────────────\n");
    printf("  %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("──────────────────────────────────────────────────\n\n");

#ifdef __aarch64__
    fflush(stdout);
    fflush(stderr);
    _exit((g_tests_passed == g_tests_run) ? 0 : 1);
#else
    return (g_tests_passed == g_tests_run) ? 0 : 1;
#endif
}
