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

typedef struct {
    int failover_detection_ms;
    int recovery_detection_ms;

    zst_pad_t* sink_primary_pad;
    zst_pad_t* sink_backup_pad;
    zst_pad_t* src_pad;

    bool using_primary;
    int64_t last_primary_pts;
} st2110_redundancy_demux_t;

static zst_result_t
st2110_redundancy_demux_primary_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_redundancy_demux_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    s->last_primary_pts = buf->pts;

    if (!s->using_primary) {
        s->using_primary = true;
        ZST_LOG_INFO("st2110_redundancy_demux", "Recovered to primary stream");

        if (pad->parent->bus) {
            /* Create and post custom failover event.
             * (We use ZST_EVENT_REDUNDANCY_FAILOVER to signal switch back as well,
             * or we could add a separate recovery event. For now, just emit warning.) */
            zst_event_t* ev = zst_event_new_warning(pad->parent, ZST_OK, "Recovered to primary stream");
            if (ev) zst_bus_post(pad->parent->bus, ev);
        }
    }

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
st2110_redundancy_demux_backup_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_redundancy_demux_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    /* Simple failover check based on pts difference.
     * In a real system, we'd use a timer or sequence numbers.
     * For now, if the primary is missing and we receive a backup packet,
     * we check if we should fail over.
     */
    if (s->using_primary) {
        /* In this mock implementation, if we receive a backup buffer whose PTS is significantly
         * larger than the last primary PTS, we assume the primary failed.
         * The PTS is in typical clock units (e.g., 90kHz or 48kHz). We use a generic
         * approximation here (assume 90kHz max) or just raw time if pts is in ns.
         * For a mock implementation, we'll keep the logic simple but acknowledge
         * different clock rates. */
        int64_t diff = buf->pts - s->last_primary_pts;
        int64_t threshold = (int64_t)s->failover_detection_ms * 90; // Default to 90kHz for video

        if (diff > threshold) {
            s->using_primary = false;
            ZST_LOG_WARN("st2110_redundancy_demux", "Failing over to backup stream");

            if (pad->parent->bus) {
                zst_event_t* ev = calloc(1, sizeof(*ev));
                if (ev) {
                    ev->type = ZST_EVENT_REDUNDANCY_FAILOVER;
                    ev->src = pad->parent;
                    zst_bus_post(pad->parent->bus, ev);
                }
            }
        }
    }

    if (!s->using_primary) {
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

static zst_result_t
st2110_redundancy_demux_open(zst_element_t* el)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (!s) return ZST_ERROR;

    s->using_primary = true;
    s->last_primary_pts = 0;

    return ZST_OK;
}

static zst_result_t
st2110_redundancy_demux_close(zst_element_t* el)
{
    st2110_redundancy_demux_t* s = el ? el->priv : NULL;
    if (s) {
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
    s->using_primary = true;
    s->last_primary_pts = 0;

    zst_element_t* el = zst_element_create(&g_demux_ops, s);
    if (!el) {
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

    s->sink_primary_pad->push = st2110_redundancy_demux_primary_push;
    s->sink_backup_pad->push = st2110_redundancy_demux_backup_push;

    zst_element_add_pad(el, s->sink_primary_pad);
    zst_element_add_pad(el, s->sink_backup_pad);
    zst_element_add_pad(el, s->src_pad);

    ZST_LOG_INFO("st2110_redundancy_demux", "created ST2110 redundancy demuxer element");
    return el;
}
