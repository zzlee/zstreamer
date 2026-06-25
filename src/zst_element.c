/*=============================================================================
    zst_element.c - Element lifecycle, state machine, pad management
=============================================================================*/

#include "zst_element.h"
#include "zst_bus.h"
#include "zst_plugin.h"
#include "zst_clock.h"
#include "zst_buffer.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int zst_bin_element_is_bin(zst_element_t* el);
extern zst_result_t zst_bin_element_change_state(zst_element_t* el,
                                                 zst_state_t old_state,
                                                 zst_state_t new_state);
extern int zst_bin_element_destroy(zst_element_t* el);

zst_element_t*
zst_element_create(const zst_element_ops_t* ops, void* priv)
{
    if (!ops) return NULL;

    zst_element_t* el = calloc(1, sizeof(*el));
    if (!el) return NULL;

    el->ops          = ops;
    __atomic_store_n(&el->state, ZST_STATE_NULL, __ATOMIC_RELEASE);
    el->src_pads     = NULL;
    el->nb_src_pads  = 0;
    el->sink_pads    = NULL;
    el->nb_sink_pads = 0;
    el->stream_infos = NULL;
    el->stream_pads  = NULL;
    el->nb_streams   = 0;
    el->priv         = priv;
    el->plugin       = NULL;
    el->desc         = NULL;
    el->clock        = NULL;
    el->pipeline     = NULL;
    el->graph_rank   = 0;
    atomic_init(&el->is_queued, false);
    atomic_init(&el->sched_task_refs, 0);
    atomic_init(&el->pool_sizing_seen_pool, NULL);
    atomic_init(&el->pool_sizing_seen_min_buffers, 0);
    atomic_init(&el->pool_sizing_seen_max_buffers, 0);
    atomic_init(&el->pool_sizing_seen_buffer_size, 0);
    atomic_init(&el->pool_sizing_seen_buffer_type, 0);
    pthread_mutex_init(&el->state_lock, NULL);

    /* Pre-allocate the scheduling token to optimize dispatch latency.
     * The token's base reference count is managed by the element itself. */
    el->sched_token = zst_buffer_create(ZST_BUFFER_USER);
    if (el->sched_token) {
        el->sched_token->memory.priv = el;
    } else {
        free(el);
        return NULL;
    }

    return el;
}

void
zst_element_destroy(zst_element_t* el)
{
    if (!el) return;

    /* Destroy all pads */
    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        zst_pad_unlink(el->src_pads[i]);
        zst_pad_unref(el->src_pads[i]);
    }
    free(el->src_pads);

    for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
        zst_pad_unlink(el->sink_pads[i]);
        zst_pad_unref(el->sink_pads[i]);
    }
    free(el->sink_pads);

    for (uint32_t i = 0; i < el->nb_streams; i++) {
        zst_stream_info_clear(&el->stream_infos[i]);
    }
    free(el->stream_infos);
    free(el->stream_pads);

    if (el->clock) {
        zst_clock_unref(el->clock);
    }

    /* Scheduler task tokens carry a raw element pointer.  Wait for any queued
     * or executing scheduler dispatches to release that pointer before the
     * element storage goes away. */
    for (uint32_t spins = 0;
         atomic_load_explicit(&el->sched_task_refs, memory_order_acquire) > 0 && spins < 10000;
         spins++) {
        struct timespec req = {0, 100000};
        nanosleep(&req, NULL);
    }
    if (el->sched_token) {
        el->sched_token->memory.priv = NULL;
    }

    /* Release the pre-allocated scheduling token */
    if (el->sched_token) {
        zst_buffer_unref(el->sched_token);
        el->sched_token = NULL;
    }

    if (!zst_bin_element_destroy(el)) {
        free(el->priv);
        el->priv = NULL;
    }

    pthread_mutex_destroy(&el->state_lock);

    zst_plugin_t* plugin = el->plugin;
    free(el);

    if (plugin) {
        zst_plugin_unref(plugin);
    }
}

zst_result_t
zst_element_set_state(zst_element_t* el, zst_state_t state)
{
    if (!el) return ZST_ERROR;

    if (state != ZST_STATE_NULL && state != ZST_STATE_READY &&
        state != ZST_STATE_PAUSED && state != ZST_STATE_PLAYING) {
        return ZST_ERROR;
    }

    zst_state_t current_state = __atomic_load_n(&el->state, __ATOMIC_ACQUIRE);
    if (current_state == state) return ZST_OK;

    /* Call the appropriate lifecycle hook */
    zst_result_t ret = ZST_OK;

    /* Transition NULL -> READY */
    if (current_state < ZST_STATE_READY && state >= ZST_STATE_READY) {
        if (el->ops->open)
            ret = el->ops->open(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition READY -> PAUSED */
    if (current_state < ZST_STATE_PAUSED && state >= ZST_STATE_PAUSED) {
        if (el->ops->preroll)
            ret = el->ops->preroll(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition PAUSED -> PLAYING */
    if (current_state < ZST_STATE_PLAYING && state >= ZST_STATE_PLAYING) {
        if (el->ops->start)
            ret = el->ops->start(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition PLAYING -> PAUSED */
    if (current_state >= ZST_STATE_PLAYING && state < ZST_STATE_PLAYING) {
        if (el->ops->stop)
            ret = el->ops->stop(el);
        if (ret != ZST_OK) return ret;
    }

    /* Transition PAUSED / READY -> NULL */
    if (current_state >= ZST_STATE_READY && state < ZST_STATE_READY) {
        if (el->ops->close)
            ret = el->ops->close(el);
        if (ret != ZST_OK) return ret;
    }

    if (zst_bin_element_is_bin(el)) {
        ret = zst_bin_element_change_state(el, current_state, state);
        if (ret != ZST_OK) return ret;
    }

    __atomic_store_n(&el->state, state, __ATOMIC_RELEASE);
    if (state != current_state && el->bus) {
        zst_event_t* ev = zst_event_new_state_changed(el, current_state, state);
        zst_bus_post(el->bus, ev);
    }
    return ret;
}

zst_pad_t*
zst_element_get_pad(zst_element_t* el, const char* name)
{
    if (!el || !name) return NULL;

    /* Search source pads */
    for (uint32_t i = 0; i < el->nb_src_pads; i++) {
        if (el->src_pads[i]->name &&
            strcmp(el->src_pads[i]->name, name) == 0)
            return el->src_pads[i];
    }

    /* Search sink pads */
    for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
        if (el->sink_pads[i]->name &&
            strcmp(el->sink_pads[i]->name, name) == 0)
            return el->sink_pads[i];
    }

    return NULL;
}

zst_result_t
zst_element_add_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad) return ZST_ERROR;

    pthread_mutex_lock(&el->state_lock);
    pad->parent = el;

    if (pad->direction == ZST_PAD_SRC) {
        zst_pad_t** pads = realloc(el->src_pads,
                                  (el->nb_src_pads + 1) * sizeof(zst_pad_t*));
        if (!pads) {
            pthread_mutex_unlock(&el->state_lock);
            return ZST_ERROR;
        }
        pads[el->nb_src_pads++] = pad;
        el->src_pads = pads;
    } else {
        zst_pad_t** pads = realloc(el->sink_pads,
                                  (el->nb_sink_pads + 1) * sizeof(zst_pad_t*));
        if (!pads) {
            pthread_mutex_unlock(&el->state_lock);
            return ZST_ERROR;
        }
        pads[el->nb_sink_pads++] = pad;
        el->sink_pads = pads;
    }
    pthread_mutex_unlock(&el->state_lock);

    return ZST_OK;
}

static zst_result_t
copy_stream_info(zst_stream_info_t* dest, const zst_stream_info_t* src)
{
    if (!dest || !src) return ZST_ERROR;
    memset(dest, 0, sizeof(*dest));
    *dest = *src;
    dest->struct_size = sizeof(*dest);
    dest->name = src->name ? strdup(src->name) : NULL;
    dest->language = src->language ? strdup(src->language) : NULL;
    dest->caps = src->caps ? zst_caps_copy(src->caps) : NULL;
    if ((src->name && !dest->name) ||
        (src->language && !dest->language) ||
        (src->caps && !dest->caps)) {
        zst_stream_info_clear(dest);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static void
fill_stream_info_from_pad(zst_pad_t* pad, uint32_t index, zst_stream_info_t* info_out)
{
    memset(info_out, 0, sizeof(*info_out));
    info_out->struct_size = sizeof(*info_out);
    info_out->id = (zst_stream_id_t)index;
    info_out->index = index;
    info_out->status = ZST_STREAM_STATUS_PRESENT;

    if (pad && pad->name) {
        info_out->name = strdup(pad->name);
    }

    if (pad && pad->caps) {
        info_out->caps = zst_caps_copy(pad->caps);
        if (pad->caps->structs) {
            if (strstr(pad->caps->structs->media_type, "video")) {
                info_out->kind = ZST_MEDIA_VIDEO;
            } else if (strstr(pad->caps->structs->media_type, "audio")) {
                info_out->kind = ZST_MEDIA_AUDIO;
            } else if (strstr(pad->caps->structs->media_type, "text")) {
                info_out->kind = ZST_MEDIA_TEXT;
            } else {
                info_out->kind = ZST_MEDIA_DATA;
            }
        }
    }
}

static zst_result_t
add_stream_entry_locked(zst_element_t* el, zst_pad_t* pad, const zst_stream_info_t* stream_info)
{
    zst_stream_info_t info;
    if (stream_info) {
        if (copy_stream_info(&info, stream_info) != ZST_OK) return ZST_ERROR;
    } else {
        fill_stream_info_from_pad(pad, el->nb_streams, &info);
    }

    if (info.struct_size == 0) info.struct_size = sizeof(info);
    if (!info.name && pad && pad->name) info.name = strdup(pad->name);

    zst_stream_info_t* infos = realloc(el->stream_infos,
                                       (el->nb_streams + 1) * sizeof(*infos));
    if (!infos) {
        zst_stream_info_clear(&info);
        return ZST_ERROR;
    }
    el->stream_infos = infos;

    zst_pad_t** pads = realloc(el->stream_pads,
                               (el->nb_streams + 1) * sizeof(*pads));
    if (!pads) {
        zst_stream_info_clear(&info);
        return ZST_ERROR;
    }
    el->stream_pads = pads;

    el->stream_infos[el->nb_streams] = info;
    el->stream_pads[el->nb_streams] = pad;
    el->nb_streams++;
    return ZST_OK;
}

static int
find_stream_entry_by_pad_locked(zst_element_t* el, zst_pad_t* pad)
{
    for (uint32_t i = 0; i < el->nb_streams; i++) {
        if (el->stream_pads[i] == pad) return (int)i;
    }
    return -1;
}

static void
remove_stream_entry_locked(zst_element_t* el, uint32_t index)
{
    if (!el || index >= el->nb_streams) return;
    zst_stream_info_clear(&el->stream_infos[index]);
    if (index + 1 < el->nb_streams) {
        memmove(&el->stream_infos[index], &el->stream_infos[index + 1],
                (el->nb_streams - index - 1) * sizeof(*el->stream_infos));
        memmove(&el->stream_pads[index], &el->stream_pads[index + 1],
                (el->nb_streams - index - 1) * sizeof(*el->stream_pads));
    }
    el->nb_streams--;
    if (el->nb_streams == 0) {
        free(el->stream_infos);
        free(el->stream_pads);
        el->stream_infos = NULL;
        el->stream_pads = NULL;
    }
}

uint32_t
zst_element_get_stream_count(zst_element_t* el)
{
    if (!el) return 0;
    if (el->ops && el->ops->get_stream_count) {
        return el->ops->get_stream_count(el);
    }

    pthread_mutex_lock(&el->state_lock);
    uint32_t count = el->nb_streams ? el->nb_streams : el->nb_src_pads;
    pthread_mutex_unlock(&el->state_lock);
    return count;
}

zst_result_t
zst_element_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out)
{
    if (!el || !info_out) return ZST_ERROR;
    if (el->ops && el->ops->get_stream_info) {
        return el->ops->get_stream_info(el, index, info_out);
    }

    pthread_mutex_lock(&el->state_lock);
    if (el->nb_streams > 0) {
        if (index >= el->nb_streams) {
            pthread_mutex_unlock(&el->state_lock);
            return ZST_ERROR;
        }
        zst_result_t ret = copy_stream_info(info_out, &el->stream_infos[index]);
        pthread_mutex_unlock(&el->state_lock);
        return ret;
    }

    if (index >= el->nb_src_pads) {
        pthread_mutex_unlock(&el->state_lock);
        return ZST_ERROR;
    }

    fill_stream_info_from_pad(el->src_pads[index], index, info_out);
    pthread_mutex_unlock(&el->state_lock);

    return ZST_OK;
}

zst_pad_t*
zst_element_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id)
{
    if (!el) return NULL;
    if (el->ops && el->ops->get_stream_pad) {
        return el->ops->get_stream_pad(el, stream_id);
    }

    pthread_mutex_lock(&el->state_lock);
    zst_pad_t* pad = NULL;
    for (uint32_t i = 0; i < el->nb_streams; i++) {
        if (el->stream_infos[i].id == stream_id) {
            pad = el->stream_pads[i];
            break;
        }
    }
    if (!pad && el->nb_streams == 0 && stream_id < el->nb_src_pads) {
        pad = el->src_pads[stream_id];
    }
    pthread_mutex_unlock(&el->state_lock);
    return pad;
}

zst_result_t
zst_element_add_dynamic_pad(zst_element_t* el, zst_pad_t* pad, const zst_stream_info_t* stream_info)
{
    if (!el || !pad) return ZST_ERROR;

    zst_result_t ret = zst_element_add_pad(el, pad);
    if (ret != ZST_OK) return ret;

    zst_stream_info_t event_info;
    memset(&event_info, 0, sizeof(event_info));

    pthread_mutex_lock(&el->state_lock);
    ret = add_stream_entry_locked(el, pad, stream_info);
    if (ret == ZST_OK) {
        ret = copy_stream_info(&event_info, &el->stream_infos[el->nb_streams - 1]);
    }
    pthread_mutex_unlock(&el->state_lock);

    if (ret != ZST_OK) {
        zst_element_remove_pad(el, pad);
        return ret;
    }

    if (el->pipeline) {
        zst_pipeline_mark_buffer_pool_sizing_dirty(el->pipeline);
    }

    if (el->bus) {
        zst_event_t* stream_ev = zst_event_new_stream_added(el, &event_info);
        if (stream_ev) zst_bus_post(el->bus, stream_ev);
        zst_event_t* pad_ev = zst_event_new_pad_added(el, pad, &event_info);
        if (pad_ev) zst_bus_post(el->bus, pad_ev);
    }

    zst_stream_info_clear(&event_info);
    return ZST_OK;
}

zst_result_t
zst_element_remove_dynamic_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad) return ZST_ERROR;

    zst_pad_ref(pad);
    zst_stream_id_t stream_id = 0;
    pthread_mutex_lock(&el->state_lock);
    int stream_index = find_stream_entry_by_pad_locked(el, pad);
    if (stream_index >= 0) {
        stream_id = el->stream_infos[stream_index].id;
    } else {
        for (uint32_t i = 0; i < el->nb_src_pads; i++) {
            if (el->src_pads[i] == pad) {
                stream_id = (zst_stream_id_t)i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&el->state_lock);

    zst_result_t ret = zst_element_remove_pad(el, pad);
    if (ret == ZST_OK) {
        if (el->pipeline) {
            zst_pipeline_mark_buffer_pool_sizing_dirty(el->pipeline);
        }
        if (el->bus) {
            zst_event_t* stream_ev = zst_event_new_stream_removed(el, stream_id);
            if (stream_ev) zst_bus_post(el->bus, stream_ev);
            zst_event_t* pad_ev = zst_event_new_pad_removed(el, pad, stream_id);
            if (pad_ev) zst_bus_post(el->bus, pad_ev);
        }
    }
    zst_pad_unref(pad);
    return ret;
}

zst_result_t
zst_element_snapshot_src_pads(zst_element_t* el, zst_pad_t*** pads_out, uint32_t* count_out)
{
    if (!el || !pads_out || !count_out) return ZST_ERROR;

    pthread_mutex_lock(&el->state_lock);
    uint32_t count = el->nb_src_pads;
    if (count == 0) {
        *pads_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&el->state_lock);
        return ZST_OK;
    }

    zst_pad_t** snapshot = malloc(count * sizeof(zst_pad_t*));
    if (!snapshot) {
        pthread_mutex_unlock(&el->state_lock);
        return ZST_ERROR;
    }

    for (uint32_t i = 0; i < count; i++) {
        snapshot[i] = zst_pad_ref(el->src_pads[i]);
    }

    *pads_out = snapshot;
    *count_out = count;
    pthread_mutex_unlock(&el->state_lock);

    return ZST_OK;
}

zst_result_t
zst_element_snapshot_sink_pads(zst_element_t* el, zst_pad_t*** pads_out, uint32_t* count_out)
{
    if (!el || !pads_out || !count_out) return ZST_ERROR;

    pthread_mutex_lock(&el->state_lock);
    uint32_t count = el->nb_sink_pads;
    if (count == 0) {
        *pads_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&el->state_lock);
        return ZST_OK;
    }

    zst_pad_t** snapshot = malloc(count * sizeof(zst_pad_t*));
    if (!snapshot) {
        pthread_mutex_unlock(&el->state_lock);
        return ZST_ERROR;
    }

    for (uint32_t i = 0; i < count; i++) {
        snapshot[i] = zst_pad_ref(el->sink_pads[i]);
    }

    *pads_out = snapshot;
    *count_out = count;
    pthread_mutex_unlock(&el->state_lock);

    return ZST_OK;
}

void
zst_element_pad_snapshot_free(zst_pad_t** pads, uint32_t count)
{
    if (!pads) return;
    for (uint32_t i = 0; i < count; i++) {
        zst_pad_unref(pads[i]);
    }
    free(pads);
}

zst_result_t
zst_element_remove_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad) return ZST_ERROR;

    pthread_mutex_lock(&el->state_lock);
    if (pad->direction == ZST_PAD_SRC) {
        for (uint32_t i = 0; i < el->nb_src_pads; i++) {
            if (el->src_pads[i] == pad) {
                int stream_index = find_stream_entry_by_pad_locked(el, pad);
                if (stream_index >= 0) remove_stream_entry_locked(el, (uint32_t)stream_index);
                memmove(&el->src_pads[i], &el->src_pads[i + 1],
                        (el->nb_src_pads - i - 1) * sizeof(zst_pad_t*));
                el->nb_src_pads--;
                pthread_mutex_unlock(&el->state_lock);
                zst_pad_unlink(pad);
                pad->parent = NULL;
                zst_pad_unref(pad);
                return ZST_OK;
            }
        }
    } else {
        for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
            if (el->sink_pads[i] == pad) {
                int stream_index = find_stream_entry_by_pad_locked(el, pad);
                if (stream_index >= 0) remove_stream_entry_locked(el, (uint32_t)stream_index);
                memmove(&el->sink_pads[i], &el->sink_pads[i + 1],
                        (el->nb_sink_pads - i - 1) * sizeof(zst_pad_t*));
                el->nb_sink_pads--;
                pthread_mutex_unlock(&el->state_lock);
                zst_pad_unlink(pad);
                pad->parent = NULL;
                zst_pad_unref(pad);
                return ZST_OK;
            }
        }
    }
    pthread_mutex_unlock(&el->state_lock);

    return ZST_ERROR;
}

void
zst_element_set_clock(zst_element_t* el, zst_clock_t* clock)
{
    if (!el) return;
    if (el->clock == clock) return;
    if (el->clock) {
        zst_clock_unref(el->clock);
    }
    el->clock = clock ? zst_clock_ref(clock) : NULL;
}

zst_result_t
zst_element_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !name || !value) return ZST_ERROR;
    if (!el->ops->set_property) return ZST_ERROR;
    return el->ops->set_property(el, name, value);
}

zst_result_t
zst_element_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !name || !value_out || max_len == 0) return ZST_ERROR;
    if (!el->ops->get_property) return ZST_ERROR;
    return el->ops->get_property(el, name, value_out, max_len);
}

static const zst_property_spec_t*
zst_element_find_property_spec(zst_element_t* el, const char* name)
{
    if (!el || !el->desc || !name) return NULL;
    for (uint32_t i = 0; i < el->desc->nb_properties; i++) {
        const zst_property_spec_t* spec = &el->desc->properties[i];
        if (spec->name && strcmp(spec->name, name) == 0) {
            return spec;
        }
    }
    return NULL;
}

static zst_result_t
zst_element_check_property_type(zst_element_t* el, const char* name,
                                zst_property_type_t expected,
                                uint32_t access_flag)
{
    const zst_property_spec_t* spec = zst_element_find_property_spec(el, name);
    if (!spec) return ZST_OK;
    if ((spec->flags & access_flag) == 0) return ZST_ERROR;
    if (spec->type == expected) return ZST_OK;
    if (expected == ZST_PROPERTY_STRING && spec->type == ZST_PROPERTY_ENUM) return ZST_OK;
    if ((expected == ZST_PROPERTY_INT && spec->type == ZST_PROPERTY_UINT) ||
        (expected == ZST_PROPERTY_UINT && spec->type == ZST_PROPERTY_INT)) {
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_result_t
zst_element_set_property_string(zst_element_t* el, const char* name, const char* value)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_STRING,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_set_property(el, name, value);
}

zst_result_t
zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_INT,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%" PRId64, value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_UINT,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%" PRIu64, value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_double(zst_element_t* el, const char* name, double value)
{
    char buf[64];
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_DOUBLE,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    snprintf(buf, sizeof(buf), "%.17g", value);
    return zst_element_set_property(el, name, buf);
}

zst_result_t
zst_element_set_property_bool(zst_element_t* el, const char* name, bool value)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_BOOL,
                                        ZST_PROPERTY_WRITABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_set_property(el, name, value ? "true" : "false");
}

zst_result_t
zst_element_get_property_string(zst_element_t* el, const char* name,
                                char* value_out, size_t max_len)
{
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_STRING,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    return zst_element_get_property(el, name, value_out, max_len);
}

zst_result_t
zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out)
{
    char buf[64];
    char* end = NULL;
    long long value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_INT,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtoll(buf, &end, 10);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = (int64_t)value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out)
{
    char buf[64];
    char* end = NULL;
    unsigned long long value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_UINT,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtoull(buf, &end, 10);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = (uint64_t)value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out)
{
    char buf[64];
    char* end = NULL;
    double value;
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_DOUBLE,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    errno = 0;
    value = strtod(buf, &end);
    if (errno || end == buf || (end && *end != '\0')) return ZST_ERROR;
    *value_out = value;
    return ZST_OK;
}

zst_result_t
zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out)
{
    char buf[16];
    if (!value_out) return ZST_ERROR;
    if (zst_element_check_property_type(el, name, ZST_PROPERTY_BOOL,
                                        ZST_PROPERTY_READABLE) != ZST_OK) {
        return ZST_ERROR;
    }
    if (zst_element_get_property(el, name, buf, sizeof(buf)) != ZST_OK) return ZST_ERROR;
    if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
        *value_out = true;
        return ZST_OK;
    }
    if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0) {
        *value_out = false;
        return ZST_OK;
    }
    return ZST_ERROR;
}

zst_buffer_pool_t*
zst_element_get_pool(zst_element_t* el)
{
    if (!el || !el->ops || !el->ops->get_pool) return NULL;
    return el->ops->get_pool(el);
}

zst_result_t
zst_element_seek(zst_element_t* el, double rate, const zst_segment_t* segment)
{
    if (!el || !segment || rate == 0.0) return ZST_ERROR;
    if (segment->stop != ZST_SEGMENT_STOP_NONE && segment->stop < segment->start) {
        return ZST_ERROR;
    }

    zst_segment_t applied = *segment;
    applied.rate = rate;

    /* Generic source support for byte-addressable elements such as filesrc:
     * if the element exposes offset/length properties, map the segment range
     * onto those properties.  Timestamp-based sources can ignore this and
     * still benefit from downstream segment propagation/clipping. */
    int byte_seek = 0;
    if (el->ops && el->ops->set_property && applied.start <= INT64_MAX) {
        char value[64];
        snprintf(value, sizeof(value), "%" PRIu64, (uint64_t)applied.start);
        byte_seek = (zst_element_set_property(el, "offset", value) == ZST_OK);

        if (byte_seek && applied.stop == ZST_SEGMENT_STOP_NONE) {
            zst_element_set_property(el, "length", "-1");
        } else if (byte_seek && applied.stop >= applied.start &&
                   applied.stop - applied.start <= INT64_MAX) {
            snprintf(value, sizeof(value), "%" PRIu64,
                     (uint64_t)(applied.stop - applied.start));
            zst_element_set_property(el, "length", value);
        }
    }

    if (!byte_seek) {
        for (uint32_t i = 0; i < el->nb_sink_pads; i++) {
            zst_pad_set_segment(el->sink_pads[i], &applied);
        }
        for (uint32_t i = 0; i < el->nb_src_pads; i++) {
            zst_pad_push_segment(el->src_pads[i], &applied);
        }
    }

    if (el->bus) {
        zst_event_t* ev = zst_event_new_segment(el, &applied);
        if (ev) zst_bus_post(el->bus, ev);
    }

    return ZST_OK;
}