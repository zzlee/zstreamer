/*=============================================================================
    st2110_redundancy_demux.c — SMPTE ST 2022-7 Redundancy Demuxer
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_log.h"
#include "zst_pad.h"
#include "zst_bus.h"
#include "zstreamer/elements/zst_st2110_redundancy.h"

#include <pthread.h>

typedef struct {
    int failover_detection_ms;
    int recovery_detection_ms;

    zst_pad_t* sink_primary_pad;
    zst_pad_t* sink_backup_pad;
    zst_pad_t* src_pad;

    pthread_mutex_t lock;
    bool initialized;
    uint16_t highest_seq;
    bool seen_history[4096];
} st2110_redundancy_demux_t;

static zst_result_t
st2110_redundancy_demux_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_redundancy_demux_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    if (buf->memory.size < 12) {
        if (buf->flags & ZST_BUFFER_FLAG_EOS) {
            if (s->src_pad && s->src_pad->peer) {
                zst_buffer_t* out = zst_buffer_ref(buf);
                if (out) {
                    zst_pad_push(s->src_pad, out);
                    zst_buffer_unref(out);
                }
            }
        }
        return ZST_OK;
    }

    uint8_t* data = buf->memory.data;
    uint16_t seq = (data[2] << 8) | data[3];

    pthread_mutex_lock(&s->lock);

    if (!s->initialized) {
        s->initialized = true;
        s->highest_seq = seq;
        memset(s->seen_history, 0, sizeof(s->seen_history));
        s->seen_history[seq % 4096] = true;
    } else {
        int16_t diff = (int16_t)(seq - s->highest_seq);

        if (diff > 0) {
            if (diff >= 4096) {
                memset(s->seen_history, 0, sizeof(s->seen_history));
            } else {
                for (uint16_t i = s->highest_seq + 1; i != seq; i++) {
                    s->seen_history[i % 4096] = false;
                }
            }
            s->highest_seq = seq;
            s->seen_history[seq % 4096] = true;
        } else if (diff > -2048) {
            if (s->seen_history[seq % 4096]) {
                pthread_mutex_unlock(&s->lock);
                return ZST_OK; /* Duplicate, drop */
            }
            s->seen_history[seq % 4096] = true;
        } else {
            /* Really old packet, ignore */
            pthread_mutex_unlock(&s->lock);
            return ZST_OK;
        }
    }

    pthread_mutex_unlock(&s->lock);

    if (s->src_pad && s->src_pad->peer) {
        zst_buffer_t* out = zst_buffer_ref(buf);
        if (out) {
            zst_pad_push(s->src_pad, out);
            zst_buffer_unref(out);
        }
    }

    return ZST_OK;
}

static zst_result_t
st2110_redundancy_demux_open(zst_element_t* el)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);
    s->initialized = false;
    s->highest_seq = 0;
    memset(s->seen_history, 0, sizeof(s->seen_history));
    pthread_mutex_unlock(&s->lock);

    return ZST_OK;
}

static zst_result_t
st2110_redundancy_demux_close(zst_element_t* el)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (s) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        el->priv = NULL;
    }
    return ZST_OK;
}

static zst_result_t
st2110_redundancy_demux_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "failover-detection-ms") == 0) {
        s->failover_detection_ms = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "recovery-detection-ms") == 0) {
        s->recovery_detection_ms = atoi(value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
st2110_redundancy_demux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "failover-detection-ms") == 0) {
        snprintf(value_out, max_len, "%d", s->failover_detection_ms);
    } else if (strcmp(name, "recovery-detection-ms") == 0) {
        snprintf(value_out, max_len, "%d", s->recovery_detection_ms);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_demux_ops = {
    .name = "st2110_redundancy_demux",
    .open = st2110_redundancy_demux_open,
    .close = st2110_redundancy_demux_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = NULL,
    .set_property = st2110_redundancy_demux_set_property,
    .get_property = st2110_redundancy_demux_get_property,
};

zst_element_t*
zst_st2110_redundancy_demux_create(void)
{
    st2110_redundancy_demux_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->failover_detection_ms = 500;
    s->recovery_detection_ms = 2000;
    pthread_mutex_init(&s->lock, NULL);
    s->initialized = false;
    s->highest_seq = 0;

    zst_element_t* el = zst_element_create(&g_demux_ops, s);
    if (!el) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }

    s->sink_primary_pad = zst_pad_create("sink_primary", ZST_PAD_SINK);
    s->sink_backup_pad = zst_pad_create("sink_backup", ZST_PAD_SINK);
    s->src_pad = zst_pad_create("src", ZST_PAD_SRC);

    if (!s->sink_primary_pad || !s->sink_backup_pad || !s->src_pad) {
        if (s->sink_primary_pad) zst_pad_destroy(s->sink_primary_pad);
        if (s->sink_backup_pad) zst_pad_destroy(s->sink_backup_pad);
        if (s->src_pad) zst_pad_destroy(s->src_pad);
        zst_element_destroy(el);
        return NULL;
    }

    s->sink_primary_pad->push = st2110_redundancy_demux_push;
    s->sink_backup_pad->push = st2110_redundancy_demux_push;

    zst_element_add_pad(el, s->sink_primary_pad);
    zst_element_add_pad(el, s->sink_backup_pad);
    zst_element_add_pad(el, s->src_pad);

    ZST_LOG_INFO("st2110_redundancy_demux", "created ST2110 redundancy demuxer element");
    return el;
}
