/*=============================================================================
    st2110_redundancy_mux.c — SMPTE ST 2022-7 Redundancy Muxer
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
#include "zstreamer/elements/zst_st2110_redundancy.h"

typedef struct {
    bool fec_enabled;
    char primary_addr[64];
    char backup_addr[64];
    int primary_port;
    int backup_port;

    zst_pad_t* sink_pad;
    zst_pad_t* src_primary_pad;
    zst_pad_t* src_backup_pad;
} st2110_redundancy_mux_t;

static zst_result_t
st2110_redundancy_mux_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    st2110_redundancy_mux_t* s = pad->parent->priv;
    if (!s) return ZST_ERROR;

    /* Push the buffer to the primary pad */
    if (s->src_primary_pad && s->src_primary_pad->peer) {
        zst_buffer_t* primary_buf = zst_buffer_ref(buf);
        if (primary_buf) {
            zst_pad_push(s->src_primary_pad, primary_buf);
            zst_buffer_unref(primary_buf);
        }
    }

    /* Push the buffer to the backup pad */
    if (s->src_backup_pad && s->src_backup_pad->peer) {
        zst_buffer_t* backup_buf = zst_buffer_ref(buf);
        if (backup_buf) {
            zst_pad_push(s->src_backup_pad, backup_buf);
            zst_buffer_unref(backup_buf);
        }
    }

    return ZST_OK;
}

static zst_result_t
st2110_redundancy_mux_open(zst_element_t* el)
{
    (void)el;
    return ZST_OK;
}

static zst_result_t
st2110_redundancy_mux_close(zst_element_t* el)
{
    st2110_redundancy_mux_t* s = el ? el->priv : NULL;
    if (s) {
        free(s);
        el->priv = NULL;
    }
    return ZST_OK;
}

static zst_result_t
st2110_redundancy_mux_set_property(zst_element_t* el, const char* name, const char* value)
{
    st2110_redundancy_mux_t* s = el ? el->priv : NULL;
    if (!s || !name || !value) return ZST_ERROR;

    if (strcmp(name, "fec-enabled") == 0) {
        s->fec_enabled = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "primary-addr") == 0) {
        strncpy(s->primary_addr, value, sizeof(s->primary_addr) - 1);
        return ZST_OK;
    }
    if (strcmp(name, "backup-addr") == 0) {
        strncpy(s->backup_addr, value, sizeof(s->backup_addr) - 1);
        return ZST_OK;
    }
    if (strcmp(name, "primary-port") == 0) {
        s->primary_port = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "backup-port") == 0) {
        s->backup_port = atoi(value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
st2110_redundancy_mux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    st2110_redundancy_mux_t* s = el ? el->priv : NULL;
    if (!s || !name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "fec-enabled") == 0) {
        snprintf(value_out, max_len, "%s", s->fec_enabled ? "true" : "false");
    } else if (strcmp(name, "primary-addr") == 0) {
        snprintf(value_out, max_len, "%s", s->primary_addr);
    } else if (strcmp(name, "backup-addr") == 0) {
        snprintf(value_out, max_len, "%s", s->backup_addr);
    } else if (strcmp(name, "primary-port") == 0) {
        snprintf(value_out, max_len, "%d", s->primary_port);
    } else if (strcmp(name, "backup-port") == 0) {
        snprintf(value_out, max_len, "%d", s->backup_port);
    } else {
        return ZST_ERROR;
    }

    value_out[max_len - 1] = '\0';
    return ZST_OK;
}

static zst_element_ops_t g_mux_ops = {
    .name = "st2110_redundancy_mux",
    .open = st2110_redundancy_mux_open,
    .close = st2110_redundancy_mux_close,
    .start = NULL,
    .stop = NULL,
    .process = NULL,
    .get_caps = NULL,
    .set_property = st2110_redundancy_mux_set_property,
    .get_property = st2110_redundancy_mux_get_property,
};

zst_element_t*
zst_st2110_redundancy_mux_create(void)
{
    st2110_redundancy_mux_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fec_enabled = false;
    strncpy(s->primary_addr, "239.255.10.0", sizeof(s->primary_addr) - 1);
    strncpy(s->backup_addr, "239.255.11.0", sizeof(s->backup_addr) - 1);
    s->primary_port = 5004;
    s->backup_port = 5004;

    zst_element_t* el = zst_element_create(&g_mux_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }

    s->sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
    s->src_primary_pad = zst_pad_create("src_primary", ZST_PAD_SRC);
    s->src_backup_pad = zst_pad_create("src_backup", ZST_PAD_SRC);

    if (!s->sink_pad || !s->src_primary_pad || !s->src_backup_pad) {
        if (s->sink_pad) zst_pad_destroy(s->sink_pad);
        if (s->src_primary_pad) zst_pad_destroy(s->src_primary_pad);
        if (s->src_backup_pad) zst_pad_destroy(s->src_backup_pad);
        zst_element_destroy(el);
        return NULL;
    }

    s->sink_pad->push = st2110_redundancy_mux_pad_push;
    zst_element_add_pad(el, s->sink_pad);
    zst_element_add_pad(el, s->src_primary_pad);
    zst_element_add_pad(el, s->src_backup_pad);

    ZST_LOG_INFO("st2110_redundancy_mux", "created ST2110 redundancy muxer element");
    return el;
}
