/*=============================================================================
    zst_buffer_pool.c
=============================================================================*/

#include "zst_buffer_pool.h"
#include "zst_allocator.h"
#include "zst_log.h"
#include "zst_caps.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

static int get_bytes_per_pixel_num(const char* pixel_format) {
    if (strcmp(pixel_format, "YUV420P") == 0 || strcmp(pixel_format, "NV12") == 0 || strcmp(pixel_format, "NV21") == 0) return 3;
    if (strcmp(pixel_format, "YUYV422") == 0 || strcmp(pixel_format, "UYVY") == 0 || strcmp(pixel_format, "YUYV") == 0) return 4;
    if (strcmp(pixel_format, "RGB24") == 0 || strcmp(pixel_format, "BGR24") == 0) return 6;
    if (strcmp(pixel_format, "RGBA") == 0 || strcmp(pixel_format, "BGRA") == 0 || strcmp(pixel_format, "ARGB") == 0) return 8;
    return 3;
}

static int get_bytes_per_pixel_den(const char* pixel_format) {
    if (strcmp(pixel_format, "YUV420P") == 0 || strcmp(pixel_format, "NV12") == 0 || strcmp(pixel_format, "NV21") == 0) return 2;
    if (strcmp(pixel_format, "YUYV422") == 0 || strcmp(pixel_format, "UYVY") == 0 || strcmp(pixel_format, "YUYV") == 0) return 2;
    if (strcmp(pixel_format, "RGB24") == 0 || strcmp(pixel_format, "BGR24") == 0) return 2;
    if (strcmp(pixel_format, "RGBA") == 0 || strcmp(pixel_format, "BGRA") == 0 || strcmp(pixel_format, "ARGB") == 0) return 2;
    return 2;
}

static int get_audio_bytes_per_sample(const char* format) {
    if (strcmp(format, "S16LE") == 0 || strcmp(format, "S16BE") == 0) return 2;
    if (strcmp(format, "S32LE") == 0 || strcmp(format, "S32BE") == 0 || strcmp(format, "F32LE") == 0 || strcmp(format, "F32BE") == 0) return 4;
    if (strcmp(format, "F64LE") == 0 || strcmp(format, "F64BE") == 0) return 8;
    if (strcmp(format, "U8") == 0) return 1;
    return 2;
}

struct zst_buffer_pool {
    zst_allocator_t* allocator;
    zst_buffer_pool_config_t config;

    zst_buffer_t** buffers;
    uint32_t count;
    uint32_t total_allocated;

    pthread_mutex_t lock;
    pthread_cond_t  cond;

    _Atomic(uint64_t) generation;
    int active;
};

zst_buffer_pool_t*
zst_buffer_pool_create(zst_allocator_t* allocator, zst_buffer_pool_config_t* config)
{
    if (!config) return NULL;

    zst_buffer_pool_t* pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->allocator = allocator ? zst_allocator_ref(allocator) : zst_allocator_cpu_create();
    pool->config = *config;

    if (pool->config.max_buffers == 0) {
        pool->config.max_buffers = 32; // Default limit
    }

    if (pool->config.min_buffers > pool->config.max_buffers) {
        pool->config.min_buffers = pool->config.max_buffers;
    }

    pool->buffers = calloc(pool->config.max_buffers, sizeof(zst_buffer_t*));
    if (!pool->buffers) {
        zst_allocator_unref(pool->allocator);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    atomic_init(&pool->generation, 1);
    pool->active = 1;
    pool->count = 0;
    pool->total_allocated = 0;

    // Pre-allocate up to min_buffers
    for (uint32_t i = 0; i < pool->config.min_buffers; i++) {
        zst_buffer_t* buf = zst_buffer_create_with_allocator(
            pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
        if (buf) {
            buf->pool = pool;
            pool->buffers[pool->count++] = buf;
            pool->total_allocated++;
        }
    }

    return pool;
}

zst_result_t
zst_buffer_pool_acquire(zst_buffer_pool_t* pool, zst_buffer_t** out_buf, int timeout_ms, uint32_t flags)
{
    if (!pool || !out_buf) return ZST_ERROR;

    *out_buf = NULL;

    struct timespec ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&pool->lock);

    while (pool->active && pool->count == 0) {
        if (pool->total_allocated < pool->config.max_buffers) {
            // Allocate a new buffer
            zst_buffer_t* buf = zst_buffer_create_with_allocator(
                pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
            if (buf) {
                buf->pool = pool;
                pool->total_allocated++;
                *out_buf = buf;
                pthread_mutex_unlock(&pool->lock);
                return ZST_OK;
            }
        }

        if (flags & ZST_POOL_ACQUIRE_NONBLOCK) {
            pthread_mutex_unlock(&pool->lock);
            return ZST_TIMEOUT;
        }

        if (timeout_ms < 0) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        } else if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->lock);
            return ZST_TIMEOUT;
        } else {
            int err = pthread_cond_timedwait(&pool->cond, &pool->lock, &ts);
            if (err != 0) {
                pthread_mutex_unlock(&pool->lock);
                return ZST_TIMEOUT;
            }
        }
    }

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return ZST_ERROR;
    }

    // Pop a buffer
    zst_buffer_t* buf = pool->buffers[--pool->count];
    buf->memory.size = pool->config.buffer_size;
    buf->refcount = 1; // It's going to be used
    *out_buf = buf;

    int trigger_low = 0;
    if (pool->config.watermark_cb && pool->count == pool->config.low_watermark) {
        trigger_low = 1;
    }

    pthread_mutex_unlock(&pool->lock);

    if (trigger_low) {
        pool->config.watermark_cb(pool, ZST_POOL_WATERMARK_LOW, pool->config.watermark_user_data);
    }

    return ZST_OK;
}

void
zst_buffer_pool_release(zst_buffer_pool_t* pool, zst_buffer_t* buf)
{
    if (!pool || !buf) return;

    // Reset buffer metadata
    buf->pts = 0;
    buf->dts = 0;
    buf->duration = 0;
    buf->flags = 0;

    pthread_mutex_lock(&pool->lock);

    if (pool->active && pool->count < pool->config.max_buffers) {
        pool->buffers[pool->count++] = buf;
        pthread_cond_signal(&pool->cond);

        int trigger_high = 0;
        if (pool->config.watermark_cb && pool->count == pool->config.high_watermark) {
            trigger_high = 1;
        }

        pthread_mutex_unlock(&pool->lock);

        if (trigger_high) {
            pool->config.watermark_cb(pool, ZST_POOL_WATERMARK_HIGH, pool->config.watermark_user_data);
        }
    } else {
        pthread_mutex_unlock(&pool->lock);

        // Pool is inactive or full, destroy the buffer properly
        buf->pool = NULL;

        // Use normal unref to trigger memory release
        // (but refcount is already 0, so we just do the inner part of unref)
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);

        pthread_mutex_lock(&pool->lock);
        pool->total_allocated--;

        int should_free = (!pool->active && pool->total_allocated == 0);
        pthread_mutex_unlock(&pool->lock);

        if (should_free) {
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->cond);
            free(pool->buffers);
            zst_allocator_unref(pool->allocator);
            free(pool);
        }
    }
}

void
zst_buffer_pool_destroy(zst_buffer_pool_t* pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->active = 0;
    pthread_cond_broadcast(&pool->cond);

    for (uint32_t i = 0; i < pool->count; i++) {
        zst_buffer_t* buf = pool->buffers[i];
        buf->pool = NULL;
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);
        pool->total_allocated--;
    }
    pool->count = 0;
    int should_free = (pool->total_allocated == 0);
    pthread_mutex_unlock(&pool->lock);

    if (should_free) {
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        free(pool->buffers);
        zst_allocator_unref(pool->allocator);
        free(pool);
    }
}

zst_buffer_pool_config_t zst_buffer_pool_get_config(zst_buffer_pool_t* pool) {
    zst_buffer_pool_config_t config = {0};
    if (!pool) return config;

    pthread_mutex_lock(&pool->lock);
    config = pool->config;
    pthread_mutex_unlock(&pool->lock);
    return config;
}

uint64_t zst_buffer_pool_get_generation(zst_buffer_pool_t* pool) {
    if (!pool) return 0;
    return atomic_load_explicit(&pool->generation, memory_order_acquire);
}

zst_result_t zst_buffer_pool_set_config(zst_buffer_pool_t* pool, const zst_buffer_pool_config_t* config) {
    if (!pool || !config) return ZST_ERROR;

    zst_buffer_pool_config_t new_config = *config;
    if (new_config.max_buffers == 0) {
        new_config.max_buffers = 32;
    }
    if (new_config.min_buffers > new_config.max_buffers) {
        new_config.min_buffers = new_config.max_buffers;
    }

    pthread_mutex_lock(&pool->lock);

    /* Free idle buffers first if the new cap is smaller than the current idle
     * count. Checked-out buffers may still make total_allocated temporarily
     * exceed max_buffers; excess returned buffers will be destroyed in
     * zst_buffer_pool_release() instead of being stored. */
    while (pool->count > new_config.max_buffers) {
        zst_buffer_t* buf = pool->buffers[--pool->count];
        buf->pool = NULL;
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);
        pool->total_allocated--;
    }

    if (new_config.max_buffers != pool->config.max_buffers) {
        zst_buffer_t** buffers = realloc(pool->buffers,
                                         new_config.max_buffers * sizeof(zst_buffer_t*));
        if (!buffers) {
            pthread_mutex_unlock(&pool->lock);
            return ZST_ERROR;
        }
        pool->buffers = buffers;
    }

    pool->config = new_config;
    atomic_fetch_add_explicit(&pool->generation, 1, memory_order_release);

    /* Honor a raised min_buffers immediately for idle/open pools. */
    while (pool->active && pool->total_allocated < pool->config.min_buffers) {
        zst_buffer_t* buf = zst_buffer_create_with_allocator(
            pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
        if (!buf) {
            pthread_mutex_unlock(&pool->lock);
            return ZST_ERROR;
        }
        buf->pool = pool;
        pool->buffers[pool->count++] = buf;
        pool->total_allocated++;
    }

    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    return ZST_OK;
}

zst_buffer_pool_config_t zst_buffer_pool_config_from_caps(const zst_caps_t* caps) {
    zst_buffer_pool_config_t config = {0};

    if (!caps || !zst_caps_is_fixed(caps)) {
        return config;
    }

    zst_caps_struct_t* s = caps->structs;
    if (!s) return config;

    config.min_buffers = 2;
    config.max_buffers = 8;

    if (s->type == ZST_CAPS_VIDEO) {
        config.buffer_type = ZST_BUFFER_VIDEO_FRAME;
        int num = get_bytes_per_pixel_num(s->video.pixel_format);
        int den = get_bytes_per_pixel_den(s->video.pixel_format);
        config.buffer_size = s->video.width * s->video.height * num / den;
    } else if (s->type == ZST_CAPS_AUDIO) {
        config.buffer_type = ZST_BUFFER_AUDIO_FRAME;
        int bps = get_audio_bytes_per_sample(s->audio.format);
        config.buffer_size = 1024 * s->audio.channels * bps;
    }

    return config;
}

void zst_buffer_pool_prefill(zst_buffer_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    while (pool->total_allocated < pool->config.max_buffers) {
        zst_buffer_t* buf = zst_buffer_create_with_allocator(
            pool->config.buffer_type, pool->allocator, pool->config.buffer_size);
        if (buf) {
            buf->pool = pool;
            pool->buffers[pool->count++] = buf;
            pool->total_allocated++;
        } else {
            break;
        }
    }

    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

void zst_buffer_pool_drain(zst_buffer_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    if (!pool->active) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    uint32_t target_allocated = pool->config.min_buffers;

    while (pool->count > 0 && pool->total_allocated > target_allocated) {
        zst_buffer_t* buf = pool->buffers[--pool->count];
        buf->pool = NULL;
        if (buf->memory.release && buf->memory.priv)
            buf->memory.release(buf->memory.priv);
        if (buf->destroy)
            buf->destroy(buf);
        free(buf);
        pool->total_allocated--;
    }

    pthread_mutex_unlock(&pool->lock);
}

#include "zst_pipeline.h"

void
zst_pool_config_default_size(zst_buffer_pool_config_t* config, zst_pipeline_t* pipeline)
{
    if (!config || !pipeline) return;

    int n_queues = zst_pipeline_count_elements_of_type(pipeline, "queue");
    if (n_queues > 0 && config->min_buffers < (uint32_t)(n_queues + 2)) {
        config->min_buffers = n_queues + 2;
        if (config->max_buffers < config->min_buffers) {
            config->max_buffers = config->min_buffers * 2;
        }
    }
}
