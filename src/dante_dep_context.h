#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zst_types.h"

typedef struct dep_context dep_context_t;

typedef enum {
    DEP_ENDPOINT_RX,
    DEP_ENDPOINT_TX
} dep_endpoint_direction_t;

typedef struct dep_endpoint {
    dep_endpoint_direction_t direction;
    dep_context_t* context;
    struct dep_endpoint* next;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint32_t* channel_map;
    uint32_t channel_count;
    uint32_t queue_periods;
    uint32_t block_samples;
    uint32_t tx_lead_us;
    uint32_t reconnect_ms;
    int32_t* samples;
    uint64_t* timestamps;
    size_t capacity_frames;
    size_t read_frame;
    size_t used_frames;
    uint32_t configured_period_samples;
    uint32_t sample_rate;
    atomic_bool enabled;
    bool active;
    uint64_t periods;
    uint64_t resets;
    uint64_t overruns;
    uint64_t underflows;
} dep_endpoint_t;

zst_result_t dep_endpoint_init(dep_endpoint_t* endpoint,
                               dep_endpoint_direction_t direction,
                               const char* channels,
                               uint32_t queue_periods,
                               uint32_t block_samples,
                               uint32_t tx_lead_us,
                               uint32_t reconnect_ms);
void dep_endpoint_deinit(dep_endpoint_t* endpoint);

dep_context_t* dep_context_acquire(const char* shm_name);
void dep_context_release(dep_context_t* context);
zst_result_t dep_context_attach(dep_context_t* context, dep_endpoint_t* endpoint);
void dep_context_detach(dep_endpoint_t* endpoint);
zst_result_t dep_endpoint_start(dep_endpoint_t* endpoint);
void dep_endpoint_stop(dep_endpoint_t* endpoint);

zst_result_t dep_endpoint_read(dep_endpoint_t* endpoint, int32_t** samples_out,
                               uint32_t* frames_out, uint64_t* pts_out,
                               uint32_t* rate_out);
zst_result_t dep_endpoint_write(dep_endpoint_t* endpoint, const int32_t* samples,
                                uint32_t frames);

bool dep_endpoint_is_active(dep_endpoint_t* endpoint);
uint32_t dep_endpoint_sample_rate(dep_endpoint_t* endpoint);
uint64_t dep_endpoint_stat(dep_endpoint_t* endpoint, const char* name);
