/*=============================================================================
    zst_bin.c — Composite element bins and ghost pads
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_bin.h"
#include "zst_bus.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZST_BIN_MAGIC       0x5A42494Eu /* "ZBIN" */
#define ZST_GHOST_PAD_MAGIC 0x5A475054u /* "ZGPT" */

typedef struct {
    uint32_t magic;
    char* name;
    zst_element_t** children;
    uint32_t nb_children;
    uint32_t cap_children;
    int eos_posted;
    int eos_forwarded;
} zst_bin_priv_t;

typedef struct {
    uint32_t magic;
    zst_pad_t* target;
    zst_pad_t* proxy_sink;
    int eos_seen;
} zst_ghost_pad_priv_t;

static zst_bin_priv_t*
bin_priv(zst_element_t* bin)
{
    if (!bin || !bin->priv) return NULL;
    zst_bin_priv_t* priv = bin->priv;
    return priv->magic == ZST_BIN_MAGIC ? priv : NULL;
}

static zst_ghost_pad_priv_t*
ghost_priv(zst_pad_t* pad)
{
    if (!pad || !pad->priv) return NULL;
    zst_ghost_pad_priv_t* priv = pad->priv;
    return priv->magic == ZST_GHOST_PAD_MAGIC ? priv : NULL;
}

static void
bin_sync_child_context(zst_element_t* bin, zst_element_t* child)
{
    if (!bin || !child) return;
    child->bus = bin->bus;
    child->pipeline = bin->pipeline;
    zst_element_set_clock(child, bin->clock);
}

static void
bin_sync_all_children(zst_element_t* bin)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (!priv) return;
    for (uint32_t i = 0; i < priv->nb_children; i++) {
        bin_sync_child_context(bin, priv->children[i]);
    }
}

static void
bin_reset_eos(zst_element_t* bin)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (priv) {
        priv->eos_posted = 0;
        priv->eos_forwarded = 0;
    }
    if (!bin) return;
    for (uint32_t i = 0; i < bin->nb_sink_pads; i++) {
        zst_ghost_pad_priv_t* gp = ghost_priv(bin->sink_pads[i]);
        if (gp) gp->eos_seen = 0;
    }
}

static int
bin_all_sink_ghosts_eos(zst_element_t* bin)
{
    if (!bin || bin->nb_sink_pads == 0) return 1;
    for (uint32_t i = 0; i < bin->nb_sink_pads; i++) {
        zst_ghost_pad_priv_t* gp = ghost_priv(bin->sink_pads[i]);
        if (gp && !gp->eos_seen) return 0;
    }
    return 1;
}

static void
bin_post_eos_once(zst_element_t* bin)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (!bin || !priv || priv->eos_posted) return;
    priv->eos_posted = 1;
    if (bin->bus) {
        zst_bus_post(bin->bus, zst_event_new_eos(bin));
    }
}

static zst_result_t
ghost_src_proxy_push(zst_pad_t* proxy, zst_buffer_t* buf)
{
    if (!proxy || !buf) return ZST_ERROR;
    zst_ghost_pad_priv_t* gp = ghost_priv((zst_pad_t*)proxy->priv);
    if (!gp) return ZST_ERROR;

    zst_pad_t* ghost = (zst_pad_t*)proxy->priv;
    if (zst_pad_is_linked(ghost)) {
        zst_result_t ret = zst_pad_push(ghost, buf);
        if (ret == ZST_OK && (buf->flags & ZST_BUFFER_FLAG_EOS)) {
            bin_post_eos_once(ghost->parent);
        }
        return ret;
    }

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        bin_post_eos_once(ghost->parent);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
ghost_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_ghost_pad_priv_t* gp = ghost_priv(pad);
    if (!gp || !gp->target || !buf) return ZST_ERROR;
    if (gp->target->direction != ZST_PAD_SINK || !gp->target->push) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        gp->eos_seen = 1;
        if (!bin_all_sink_ghosts_eos(pad->parent)) {
            return ZST_OK;
        }

        zst_bin_priv_t* binp = bin_priv(pad->parent);
        if (!binp) return ZST_ERROR;
        if (binp->eos_forwarded) return ZST_OK;
        binp->eos_forwarded = 1;

        zst_result_t first_error = ZST_OK;
        for (uint32_t i = 0; i < pad->parent->nb_sink_pads; i++) {
            zst_ghost_pad_priv_t* branch = ghost_priv(pad->parent->sink_pads[i]);
            if (!branch || !branch->target || !branch->target->push) continue;
            zst_result_t branch_ret = branch->target->push(branch->target, buf);
            if (branch_ret != ZST_OK && first_error == ZST_OK) {
                first_error = branch_ret;
            }
        }
        if (first_error == ZST_OK && pad->parent->nb_src_pads == 0) {
            bin_post_eos_once(pad->parent);
        }
        return first_error;
    }

    return gp->target->push(gp->target, buf);
}

static zst_result_t
ghost_src_pull(zst_pad_t* pad, zst_buffer_t** out)
{
    zst_ghost_pad_priv_t* gp = ghost_priv(pad);
    if (!gp || !gp->target || !out) return ZST_ERROR;
    if (gp->target->direction != ZST_PAD_SRC || !gp->target->pull) return ZST_ERROR;
    zst_result_t ret = gp->target->pull(gp->target, out);
    if (ret == ZST_OK && out && *out && ((*out)->flags & ZST_BUFFER_FLAG_EOS)) {
        bin_post_eos_once(pad->parent);
    }
    return ret;
}

static void
ghost_pad_destroy_priv(zst_pad_t* pad)
{
    zst_ghost_pad_priv_t* gp = ghost_priv(pad);
    if (!gp) return;

    if (gp->proxy_sink) {
        zst_pad_destroy(gp->proxy_sink);
        gp->proxy_sink = NULL;
    }
    gp->target = NULL;
    gp->magic = 0;
    free(gp);
    pad->priv = NULL;
}

zst_pad_t*
zst_ghost_pad_create(const char* name, zst_pad_t* target)
{
    if (!target) return NULL;

    zst_pad_t* pad = zst_pad_create(name, target->direction);
    if (!pad) return NULL;

    zst_ghost_pad_priv_t* gp = calloc(1, sizeof(*gp));
    if (!gp) {
        zst_pad_destroy(pad);
        return NULL;
    }
    gp->magic = ZST_GHOST_PAD_MAGIC;
    pad->priv = gp;
    pad->destroy_priv = ghost_pad_destroy_priv;

    if (target->direction == ZST_PAD_SINK) {
        pad->push = ghost_sink_push;
        pad->pull = NULL;
    } else {
        pad->pull = ghost_src_pull;
        pad->push = NULL;
    }

    if (zst_ghost_pad_set_target(pad, target) != ZST_OK) {
        zst_pad_destroy(pad);
        return NULL;
    }
    return pad;
}

zst_result_t
zst_ghost_pad_set_target(zst_pad_t* ghost_pad, zst_pad_t* target)
{
    zst_ghost_pad_priv_t* gp = ghost_priv(ghost_pad);
    if (!gp || !target || ghost_pad->direction != target->direction) return ZST_ERROR;

    if (gp->proxy_sink) {
        zst_pad_destroy(gp->proxy_sink);
        gp->proxy_sink = NULL;
    }

    gp->target = target;
    gp->eos_seen = 0;

    if (target->direction == ZST_PAD_SRC) {
        zst_pad_t* proxy = zst_pad_create("ghost-proxy-sink", ZST_PAD_SINK);
        if (!proxy) {
            gp->target = NULL;
            return ZST_ERROR;
        }
        proxy->push = ghost_src_proxy_push;
        proxy->pull = NULL;
        proxy->priv = ghost_pad;
        if (zst_pad_link(target, proxy) != ZST_OK) {
            proxy->priv = NULL;
            zst_pad_destroy(proxy);
            gp->target = NULL;
            return ZST_ERROR;
        }
        gp->proxy_sink = proxy;
    }

    return ZST_OK;
}

zst_pad_t*
zst_ghost_pad_get_target(zst_pad_t* ghost_pad)
{
    zst_ghost_pad_priv_t* gp = ghost_priv(ghost_pad);
    return gp ? gp->target : NULL;
}

static zst_result_t
bin_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    if (!el || !out) return ZST_ERROR;
    *out = NULL;

    /* A source bin is driven by pulling from its exposed src ghost pad. */
    if (el->nb_sink_pads == 0 && el->nb_src_pads > 0 && el->src_pads[0]->pull) {
        return el->src_pads[0]->pull(el->src_pads[0], out);
    }

    return ZST_OK;
}

static zst_caps_t*
bin_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;
    zst_pad_t* target = zst_ghost_pad_get_target(pad);
    if (!target || target == pad) return NULL;
    return zst_pad_get_caps(target);
}

static zst_clock_t*
bin_provide_clock(zst_element_t* el)
{
    zst_bin_priv_t* priv = bin_priv(el);
    if (!priv) return NULL;
    for (uint32_t i = 0; i < priv->nb_children; i++) {
        zst_element_t* child = priv->children[i];
        if (child && child->ops && child->ops->provide_clock) {
            zst_clock_t* clock = child->ops->provide_clock(child);
            if (clock) return clock;
        }
    }
    return NULL;
}

static zst_buffer_pool_t*
bin_get_pool(zst_element_t* el)
{
    zst_bin_priv_t* priv = bin_priv(el);
    if (!priv) return NULL;
    for (uint32_t i = 0; i < priv->nb_children; i++) {
        zst_buffer_pool_t* pool = zst_element_get_pool(priv->children[i]);
        if (pool) return pool;
    }
    return NULL;
}

static zst_result_t
bin_change_state(zst_element_t* el, zst_state_t old_state, zst_state_t new_state)
{
    (void)old_state;
    zst_bin_priv_t* priv = bin_priv(el);
    if (!priv) return ZST_ERROR;

    bin_sync_all_children(el);
    if (new_state >= ZST_STATE_READY) {
        bin_reset_eos(el);
    }

    for (uint32_t i = 0; i < priv->nb_children; i++) {
        zst_result_t ret = zst_element_set_state(priv->children[i], new_state);
        if (ret != ZST_OK) {
            for (uint32_t j = 0; j < i; j++) {
                zst_element_set_state(priv->children[j], old_state);
            }
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_error(priv->children[i], ret,
                                                           "Bin child failed to set state"));
            }
            return ret;
        }
    }
    return ZST_OK;
}

static void
bin_destroy(zst_element_t* el)
{
    zst_bin_priv_t* priv = bin_priv(el);
    if (!priv) return;

    for (uint32_t i = priv->nb_children; i > 0; i--) {
        zst_element_t* child = priv->children[i - 1];
        if (child) {
            zst_element_set_state(child, ZST_STATE_NULL);
            child->bus = NULL;
            child->pipeline = NULL;
            zst_element_destroy(child);
        }
    }

    free(priv->children);
    free(priv->name);
    priv->magic = 0;
    free(priv);
}

static zst_element_ops_t g_bin_ops = {
    .name = "bin",
    .process = bin_process,
    .get_caps = bin_get_caps,
    .provide_clock = bin_provide_clock,
    .get_pool = bin_get_pool,
};

int
zst_bin_element_is_bin(zst_element_t* el)
{
    return bin_priv(el) != NULL;
}

zst_result_t
zst_bin_element_change_state(zst_element_t* el, zst_state_t old_state, zst_state_t new_state)
{
    return bin_change_state(el, old_state, new_state);
}

int
zst_bin_element_destroy(zst_element_t* el)
{
    if (!bin_priv(el)) return 0;
    bin_destroy(el);
    el->priv = NULL;
    return 1;
}

zst_element_t*
zst_bin_create(const char* name)
{
    zst_bin_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    priv->magic = ZST_BIN_MAGIC;
    priv->name = name ? strdup(name) : NULL;

    zst_element_t* el = zst_element_create(&g_bin_ops, priv);
    if (!el) {
        free(priv->name);
        free(priv);
        return NULL;
    }
    return el;
}

zst_result_t
zst_bin_add(zst_element_t* bin, zst_element_t* child)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (!priv || !child) return ZST_ERROR;

    for (uint32_t i = 0; i < priv->nb_children; i++) {
        if (priv->children[i] == child) return ZST_ERROR;
    }

    if (priv->nb_children == priv->cap_children) {
        uint32_t new_cap = priv->cap_children ? priv->cap_children * 2u : 4u;
        zst_element_t** children = realloc(priv->children,
                                           new_cap * sizeof(*children));
        if (!children) return ZST_ERROR;
        priv->children = children;
        priv->cap_children = new_cap;
    }

    child->parent_bin = bin;
    bin_sync_child_context(bin, child);
    priv->children[priv->nb_children++] = child;

    zst_state_t bin_state = __atomic_load_n(&bin->state, __ATOMIC_ACQUIRE);
    if (bin_state != ZST_STATE_NULL) {
        zst_result_t ret = zst_element_set_state(child, bin_state);
        if (ret != ZST_OK) {
            priv->nb_children--;
            child->bus = NULL;
            child->pipeline = NULL;
            child->parent_bin = NULL;
            zst_element_set_clock(child, NULL);
            return ret;
        }
    }

    return ZST_OK;
}

zst_result_t
zst_bin_remove(zst_element_t* bin, zst_element_t* child)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (!priv || !child) return ZST_ERROR;

    for (uint32_t i = 0; i < priv->nb_children; i++) {
        if (priv->children[i] != child) continue;

        zst_element_set_state(child, ZST_STATE_NULL);
        child->bus = NULL;
        child->pipeline = NULL;
        child->parent_bin = NULL;
        zst_element_set_clock(child, NULL);

        for (uint32_t j = i; j + 1 < priv->nb_children; j++) {
            priv->children[j] = priv->children[j + 1];
        }
        priv->nb_children--;
        return ZST_OK;
    }
    return ZST_ERROR;
}

uint32_t
zst_bin_get_child_count(zst_element_t* bin)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    return priv ? priv->nb_children : 0;
}

zst_element_t*
zst_bin_get_child(zst_element_t* bin, uint32_t index)
{
    zst_bin_priv_t* priv = bin_priv(bin);
    if (!priv || index >= priv->nb_children) return NULL;
    return priv->children[index];
}

zst_result_t
zst_bin_add_ghost_pad(zst_element_t* bin, zst_pad_t* ghost_pad)
{
    if (!bin_priv(bin) || !ghost_priv(ghost_pad)) return ZST_ERROR;
    return zst_element_add_pad(bin, ghost_pad);
}
