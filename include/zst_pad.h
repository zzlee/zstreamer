/*=============================================================================
    zst_pad.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_PAD_SRC,
    ZST_PAD_SINK
} zst_pad_direction_t;

typedef enum {
    ZST_PAD_PROBE_TYPE_PRE_BUFFER   = 1 << 0,
    ZST_PAD_PROBE_TYPE_POST_BUFFER  = 1 << 1,
    ZST_PAD_PROBE_TYPE_PRE_EVENT    = 1 << 2,
    ZST_PAD_PROBE_TYPE_POST_EVENT   = 1 << 3,
    ZST_PAD_PROBE_TYPE_BLOCK        = 1 << 4
} zst_pad_probe_type_t;

typedef enum {
    ZST_PAD_PROBE_OK,
    ZST_PAD_PROBE_DROP,
    ZST_PAD_PROBE_BLOCK,
    ZST_PAD_PROBE_REBLOCK
} zst_pad_probe_return_t;

typedef struct {
    zst_pad_t* pad;
    uint32_t type; /* Note: one of zst_pad_probe_type_t flags */
    zst_buffer_t* buffer;
} zst_pad_probe_info_t;

typedef zst_pad_probe_return_t (*zst_pad_probe_cb)(
    zst_pad_t* pad,
    zst_pad_probe_info_t* info,
    void* user_data);

typedef struct zst_pad_probe zst_pad_probe_t;

typedef zst_result_t (*zst_pad_push_fn)(
    zst_pad_t* pad,
    zst_buffer_t* buf);

typedef zst_result_t (*zst_pad_pull_fn)(
    zst_pad_t* pad,
    zst_buffer_t** out);

struct zst_pad {

    const char* name;

    zst_pad_direction_t direction;

    zst_element_t* parent;

    zst_caps_t* caps;          /* Negotiated caps */
    zst_caps_t* template_caps; /* Supported template caps */

    zst_pad_push_fn push;
    zst_pad_pull_fn pull;

    zst_pad_t* peer;

    void* priv;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    zst_pad_probe_t* probes;
    uint32_t next_probe_id;
    bool is_blocked;
    uint32_t block_waiters;
};

uint32_t zst_pad_add_probe(
    zst_pad_t* pad,
    uint32_t mask,
    zst_pad_probe_cb callback,
    void* user_data,
    void (*destroy_data)(void*));

void zst_pad_remove_probe(
    zst_pad_t* pad,
    uint32_t id);

void zst_pad_block(
    zst_pad_t* pad);

void zst_pad_unblock(
    zst_pad_t* pad);

zst_pad_t* zst_pad_create(
    const char* name,
    zst_pad_direction_t direction);

void zst_pad_destroy(
    zst_pad_t* pad);

zst_result_t zst_pad_link(
    zst_pad_t* src,
    zst_pad_t* sink);

void zst_pad_unlink(
    zst_pad_t* pad);

zst_result_t zst_pad_push(
    zst_pad_t* pad,
    zst_buffer_t* buf);

zst_result_t zst_pad_pull(
    zst_pad_t* pad,
    zst_buffer_t** out);

void zst_pad_reset_callbacks(
    zst_pad_t* pad);

zst_result_t zst_pad_set_caps(
    zst_pad_t* pad,
    const zst_caps_t* caps);

zst_caps_t* zst_pad_get_caps(
    zst_pad_t* pad);

zst_result_t zst_pad_set_template_caps(
    zst_pad_t* pad,
    const zst_caps_t* caps);

zst_result_t zst_pad_negotiate(
    zst_pad_t* src,
    zst_pad_t* sink);

#ifdef __cplusplus
}
#endif