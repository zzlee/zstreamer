/*=============================================================================
    audio_mixer.c — Audio mixer element — multiple audio inputs mixed into one
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_audio_mixer.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_queue.h"
#include "zst_log.h"
#include "zst_clock.h"

#define MAX_INPUTS 64

static inline double clip_double(double val) {
    if (val > 1.0) return 1.0;
    if (val < -1.0) return -1.0;
    return val;
}

/* ── Private format codes (mirrors audio_test_src.c) ─────────────────── */
#define ZST_AUDIO_FMT_S16LE 0u
#define ZST_AUDIO_FMT_F32LE 3u

typedef struct {
    char        name[32];
    zst_pad_t*  pad;
    zst_queue_t* queue;
    zst_buffer_t* pending;
    double      volume;
    double      pan;        /* -1.0 (left) to 1.0 (right) */
    bool        mute;
    bool        eos;
    bool        active;
} audio_mixer_input_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
    bool            running;

    audio_mixer_input_t inputs[MAX_INPUTS];
    uint32_t            num_inputs;
    uint32_t            next_pad_index;

    zst_pad_t*          srcpad;
    zst_buffer_pool_t*  pool;

    uint32_t            sample_rate;
    uint32_t            channels;
    uint32_t            format;     /* 0=S16LE, 3=F32LE */
    uint32_t            latency;    /* max wait for missing aligned inputs, in ms */
    zst_time_t          max_lateness; /* QoS drop threshold in ns; 0 disables */
    uint64_t            dropped_late;
    uint32_t            block_samples;
    zst_time_t          block_duration;
    zst_time_t          next_pts;
    bool                have_next_pts;

    bool                eos_sent;

    double*             fmix;
    size_t              fmix_capacity;
} audio_mixer_t;

/* ── Forward declarations ────────────────────────────────────────────── */
static zst_result_t audio_mixer_open(zst_element_t* el);
static zst_result_t audio_mixer_close(zst_element_t* el);
static zst_result_t audio_mixer_start(zst_element_t* el);
static zst_result_t audio_mixer_stop(zst_element_t* el);
static zst_result_t audio_mixer_set_property(zst_element_t* el, const char* name, const char* value);
static zst_result_t audio_mixer_get_property(zst_element_t* el, const char* name, char* out, size_t max);
static zst_caps_t*  audio_mixer_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter);
static void*        audio_mixer_worker(void* arg);

static void audio_mixer_buf_free(zst_buffer_t* buf);

/* ── Ops vtable ──────────────────────────────────────────────────────── */
static const zst_element_ops_t g_audio_mixer_ops = {
    .name          = "audiomixer",
    .open          = audio_mixer_open,
    .close         = audio_mixer_close,
    .start         = audio_mixer_start,
    .stop          = audio_mixer_stop,
    .set_property  = audio_mixer_set_property,
    .get_property  = audio_mixer_get_property,
    .get_caps      = audio_mixer_get_caps,
};

/* ── Public API ──────────────────────────────────────────────────────── */

zst_element_t*
zst_audio_mixer_create(void)
{
    return zst_audio_mixer_create_with_config(NULL);
}

zst_element_t*
zst_audio_mixer_create_with_config(const zst_audio_mixer_config_t* config)
{
    audio_mixer_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);

    zst_element_t* el = zst_element_create(&g_audio_mixer_ops, s);
    if (!el) {
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }

    s->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    if (!s->srcpad || zst_element_add_pad(el, s->srcpad) != ZST_OK) {
        if (s->srcpad) zst_pad_destroy(s->srcpad);
        zst_element_destroy(el);
        return NULL;
    }

    /* Defaults */
    s->sample_rate = 48000;
    s->channels    = 2;
    s->format      = ZST_AUDIO_FMT_F32LE;
    s->latency     = 20;

    if (config) {
        if (config->latency > 0) s->latency = config->latency;
    }

    return el;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static zst_result_t
audio_mixer_open(zst_element_t* el)
{
    (void)el;
    /* Nothing to open beyond what's in create */
    return ZST_OK;
}

static zst_result_t
audio_mixer_close(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    if (!s) return ZST_OK;

    /* Ensure the worker has stopped */
    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }

    /* Destroy per-input queues and pending buffers */
    for (uint32_t i = 0; i < s->num_inputs; i++) {
        if (s->inputs[i].pending) {
            zst_buffer_unref(s->inputs[i].pending);
            s->inputs[i].pending = NULL;
        }
        if (s->inputs[i].queue) {
            zst_queue_destroy(s->inputs[i].queue);
            s->inputs[i].queue = NULL;
        }
        s->inputs[i].pad = NULL;
        s->inputs[i].active = false;
        s->inputs[i].eos = false;
    }
    s->num_inputs = 0;

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    if (s->fmix) {
        free(s->fmix);
        s->fmix = NULL;
    }
    s->fmix_capacity = 0;

    s->eos_sent = false;
    s->have_next_pts = false;

    return ZST_OK;
}

static zst_result_t
audio_mixer_start(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    s->running  = true;
    s->eos_sent = false;
    s->have_next_pts = false;
    pthread_create(&s->thread, NULL, audio_mixer_worker, el);
    return ZST_OK;
}

static zst_result_t
audio_mixer_stop(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    if (!s) return ZST_OK;

    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }
    return ZST_OK;
}

/* ── Caps ────────────────────────────────────────────────────────────── */

static zst_caps_t*
audio_mixer_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    audio_mixer_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    const char* fmt_str = (s->format == ZST_AUDIO_FMT_S16LE) ? "S16LE" : "F32LE";

    if (pad == s->srcpad) {
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, fmt_str));
    } else {
        /* Sink pads accept both S16LE and F32LE */
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, "S16LE"));
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, "F32LE"));
    }
    return caps;
}

/* ── Sink pad push callback ──────────────────────────────────────────── */

static zst_result_t
audio_mixer_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf || !pad->priv) return ZST_ERROR;
    audio_mixer_input_t* in = (audio_mixer_input_t*)pad->priv;
    zst_element_t* el = pad->parent;
    audio_mixer_t* s = el->priv;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        pthread_mutex_lock(&s->mutex);
        if (in->active) in->eos = true;
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        return ZST_OK;
    }

    pthread_mutex_lock(&s->mutex);
    bool accept = s->running && in->active && in->queue != NULL;
    zst_time_t max_lateness = s->max_lateness;
    pthread_mutex_unlock(&s->mutex);
    if (!accept) {
        return ZST_OK;
    }

    if (max_lateness > 0 && el->clock && buf->pts > 0) {
        zst_time_t current = zst_clock_get_time(el->clock);
        if (current > buf->pts && (current - buf->pts) > max_lateness) {
            pthread_mutex_lock(&s->mutex);
            s->dropped_late++;
            pthread_mutex_unlock(&s->mutex);
            return ZST_OK;
        }
    }

    zst_result_t res = zst_queue_push(in->queue, buf, UINT32_MAX);
    if (res == ZST_OK) {
        pthread_mutex_lock(&s->mutex);
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    return res;
}

/* ── Pad property helpers ────────────────────────────────────────────── */

static audio_mixer_input_t*
audio_mixer_find_input_by_name(audio_mixer_t* s, const char* name)
{
    for (uint32_t i = 0; i < s->num_inputs; i++) {
        if (s->inputs[i].active && strcmp(s->inputs[i].name, name) == 0)
            return &s->inputs[i];
    }
    return NULL;
}

static zst_result_t
audio_mixer_set_pad_property(audio_mixer_input_t* in, const char* prop, const char* value)
{
    if (strcmp(prop, "volume") == 0) {
        in->volume = strtod(value, NULL);
        if (in->volume < 0.0) in->volume = 0.0;
        return ZST_OK;
    }
    if (strcmp(prop, "pan") == 0) {
        in->pan = strtod(value, NULL);
        if (in->pan < -1.0) in->pan = -1.0;
        if (in->pan > 1.0)  in->pan = 1.0;
        return ZST_OK;
    }
    if (strcmp(prop, "mute") == 0) {
        in->mute = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
audio_mixer_get_pad_property(audio_mixer_input_t* in, const char* prop, char* out, size_t max)
{
    if (strcmp(prop, "volume") == 0) {
        snprintf(out, max, "%f", in->volume);
        return ZST_OK;
    }
    if (strcmp(prop, "pan") == 0) {
        snprintf(out, max, "%f", in->pan);
        return ZST_OK;
    }
    if (strcmp(prop, "mute") == 0) {
        snprintf(out, max, "%s", in->mute ? "true" : "false");
        return ZST_OK;
    }
    return ZST_ERROR;
}

/* ── Request-pad ─────────────────────────────────────────────────────── */

zst_pad_t*
zst_audio_mixer_request_pad(zst_element_t* el, const char* name)
{
    if (!el || !el->priv) return NULL;
    audio_mixer_t* s = el->priv;

    pthread_mutex_lock(&s->mutex);

    uint32_t slot = MAX_INPUTS;
    for (uint32_t i = 0; i < MAX_INPUTS; i++) {
        if (!s->inputs[i].active && !s->inputs[i].queue && !s->inputs[i].pad) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_INPUTS) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    audio_mixer_input_t* in = &s->inputs[slot];
    memset(in, 0, sizeof(*in));
    if (name && strlen(name) > 0) {
        strncpy(in->name, name, sizeof(in->name) - 1);
        in->name[sizeof(in->name) - 1] = '\0';
    } else {
        snprintf(in->name, sizeof(in->name), "sink_%u", s->next_pad_index++);
    }

    in->volume = 1.0;
    in->pan    = 0.0;
    in->mute   = false;
    in->eos    = false;
    in->active = true;

    zst_queue_config_t qcfg = {
        .mode         = ZST_QUEUE_SYNC,
        .max_buffers  = 10,
        .max_bytes    = 0,
        .max_duration = 0
    };
    in->queue = zst_queue_create(&qcfg);
    if (!in->queue) {
        in->active = false;
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    zst_pad_t* pad = zst_pad_create(in->name, ZST_PAD_SINK);
    if (!pad) {
        zst_queue_destroy(in->queue);
        in->queue = NULL;
        in->active = false;
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    pad->push       = audio_mixer_sink_push;
    pad->priv       = in;
    pad->destroy_priv = NULL;
    in->pad         = pad;

    if (zst_element_add_pad(el, pad) != ZST_OK) {
        zst_pad_destroy(pad);
        zst_queue_destroy(in->queue);
        in->pad = NULL;
        in->queue = NULL;
        in->active = false;
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    if (slot >= s->num_inputs) s->num_inputs = slot + 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    return pad;
}

zst_result_t
zst_audio_mixer_release_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !el->priv || !pad) return ZST_ERROR;
    audio_mixer_t* s = el->priv;

    zst_queue_t* queue = NULL;
    zst_buffer_t* pending = NULL;
    bool found = false;

    pthread_mutex_lock(&s->mutex);
    for (uint32_t i = 0; i < s->num_inputs; i++) {
        audio_mixer_input_t* in = &s->inputs[i];
        if (in->active && in->pad == pad) {
            found = true;
            in->active = false;
            in->eos = true;
            queue = in->queue;
            pending = in->pending;
            in->queue = NULL;
            in->pending = NULL;
            if (in->pad) in->pad->priv = NULL;
            in->pad = NULL;
            in->name[0] = '\0';
            break;
        }
    }
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    if (!found) return ZST_ERROR;

    if (queue) zst_queue_destroy(queue);
    if (pending) zst_buffer_unref(pending);
    return zst_element_remove_pad(el, pad);
}

/* ── Property splitter (pad_name::prop syntax) ───────────────────────── */

static int
split_pad_prop(const char* name, char* pad, size_t pad_len, const char** prop_out)
{
    const char* sep = strstr(name, "::");
    if (!sep) return 0;
    size_t n = (size_t)(sep - name);
    if (n == 0 || n >= pad_len) return 0;
    memcpy(pad, name, n);
    pad[n] = '\0';
    *prop_out = sep + 2;
    return 1;
}

/* ── Set / get property ──────────────────────────────────────────────── */

static zst_result_t
audio_mixer_set_property(zst_element_t* el, const char* name, const char* value)
{
    audio_mixer_t* s = el->priv;
    const char* prop = NULL;
    char pad_name[32];

    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        pthread_mutex_lock(&s->mutex);
        audio_mixer_input_t* in = audio_mixer_find_input_by_name(s, pad_name);
        zst_result_t r = in ? audio_mixer_set_pad_property(in, prop, value) : ZST_ERROR;
        pthread_mutex_unlock(&s->mutex);
        return r;
    } else if (strcmp(name, "request-pad") == 0) {
        zst_pad_t* p = zst_audio_mixer_request_pad(el, (strlen(value) > 0) ? value : NULL);
        return p ? ZST_OK : ZST_ERROR;
    } else if (strcmp(name, "release-pad") == 0) {
        zst_pad_t* p = zst_element_get_pad(el, value);
        return p ? zst_audio_mixer_release_pad(el, p) : ZST_ERROR;
    } else if (strcmp(name, "latency") == 0) {
        pthread_mutex_lock(&s->mutex);
        s->latency = (uint32_t)strtoul(value, NULL, 10);
        pthread_mutex_unlock(&s->mutex);
        return ZST_OK;
    } else if (strcmp(name, "max-lateness") == 0) {
        pthread_mutex_lock(&s->mutex);
        s->max_lateness = (zst_time_t)strtoull(value, NULL, 10);
        pthread_mutex_unlock(&s->mutex);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
audio_mixer_get_property(zst_element_t* el, const char* name, char* out, size_t max)
{
    audio_mixer_t* s = el->priv;
    const char* prop = NULL;
    char pad_name[32];

    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        pthread_mutex_lock(&s->mutex);
        audio_mixer_input_t* in = audio_mixer_find_input_by_name(s, pad_name);
        zst_result_t r = in ? audio_mixer_get_pad_property(in, prop, out, max) : ZST_ERROR;
        pthread_mutex_unlock(&s->mutex);
        return r;
    } else if (strcmp(name, "latency") == 0) {
        pthread_mutex_lock(&s->mutex);
        uint32_t latency = s->latency;
        pthread_mutex_unlock(&s->mutex);
        snprintf(out, max, "%u", latency);
        return ZST_OK;
    } else if (strcmp(name, "max-lateness") == 0) {
        pthread_mutex_lock(&s->mutex);
        zst_time_t max_lateness = s->max_lateness;
        pthread_mutex_unlock(&s->mutex);
        snprintf(out, max, "%llu", (unsigned long long)max_lateness);
        return ZST_OK;
    } else if (strcmp(name, "dropped-late") == 0) {
        pthread_mutex_lock(&s->mutex);
        uint64_t dropped = s->dropped_late;
        pthread_mutex_unlock(&s->mutex);
        snprintf(out, max, "%llu", (unsigned long long)dropped);
        return ZST_OK;
    }

    return ZST_ERROR;
}

/* ── Buffer payload destructor (for manually allocated frames) ───────── */

static void
audio_mixer_buf_free(zst_buffer_t* buf)
{
    if (!buf) return;
    if (!buf->pool && !buf->memory.release && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
    if (buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

static zst_time_t
audio_mixer_samples_to_ns(uint32_t samples, uint32_t rate)
{
    if (rate == 0) return 0;
    return (zst_time_t)(((uint64_t)samples * 1000000000ULL) / rate);
}

static uint32_t
audio_mixer_ns_to_samples(zst_time_t ns, uint32_t rate)
{
    if (rate == 0) return 0;
    return (uint32_t)(((uint64_t)ns * rate) / 1000000000ULL);
}

static int
audio_mixer_cond_timedwait_ms(pthread_cond_t* cond, pthread_mutex_t* mutex, uint32_t ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &ts);
}

static bool
audio_mixer_input_has_data(audio_mixer_input_t* in)
{
    return in && in->active && (in->pending || (in->queue && zst_queue_size(in->queue) > 0));
}

static inline double
audio_mixer_calculate_gain(double volume, uint32_t out_channels, uint32_t c, double pan)
{
    double gain = volume;
    if (out_channels >= 2) {
        if (c == 0) gain *= (pan <= 0.0 ? 1.0 : (1.0 - pan));
        else if (c == 1) gain *= (pan >= 0.0 ? 1.0 : (1.0 + pan));
    }
    return gain;
}

/* ── Worker thread ───────────────────────────────────────────────────── */

static void*
audio_mixer_worker(void* arg)
{
    zst_element_t* el = (zst_element_t*)arg;
    audio_mixer_t* s = el->priv;

    while (1) {
        pthread_mutex_lock(&s->mutex);

        while (s->running) {
            bool all_ready = true;
            bool any_ready = false;
            bool all_eos = true;
            uint32_t active_count = 0;

            for (uint32_t i = 0; i < s->num_inputs; i++) {
                audio_mixer_input_t* in = &s->inputs[i];
                if (!in->active) continue;
                active_count++;
                bool has_data = audio_mixer_input_has_data(in);
                if (in->eos && !has_data) continue;
                all_eos = false;
                if (has_data) {
                    any_ready = true;
                } else {
                    all_ready = false;
                }
            }

            if (all_eos && active_count > 0) {
                s->eos_sent = true;
                pthread_mutex_unlock(&s->mutex);

                zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
                if (eos_buf) {
                    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                    zst_pad_push(s->srcpad, eos_buf);
                    zst_buffer_unref(eos_buf);
                }

                pthread_mutex_lock(&s->mutex);
                goto worker_done;
            }

            if (any_ready && all_ready) break;

            /* Once the timeline is established, or after the configured
             * latency window expires, produce a block and fill missing pads
             * with silence instead of stalling the whole mixer. */
            if (any_ready && (s->have_next_pts || s->latency > 0)) {
                if (s->latency == 0) break;
                int wait_res = audio_mixer_cond_timedwait_ms(&s->cond, &s->mutex, s->latency);
                if (wait_res == ETIMEDOUT) break;
                continue;
            }

            pthread_cond_wait(&s->cond, &s->mutex);
        }

        if (!s->running) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }

        /* Pull one queued head into each pad's pending slot.  The pending slot
         * lets us keep a future PTS buffer while generating silence for gaps. */
        for (uint32_t i = 0; i < s->num_inputs; i++) {
            audio_mixer_input_t* in = &s->inputs[i];
            if (!in->active || in->pending || !in->queue) continue;
            zst_buffer_t* buf = NULL;
            if (zst_queue_pop(in->queue, &buf, 0) == ZST_OK && buf) {
                in->pending = buf;
            }
        }

        zst_time_t mix_pts = s->have_next_pts ? s->next_pts : 0;
        bool have_target = s->have_next_pts;
        uint32_t samples_to_mix = s->block_samples;
        zst_time_t mix_duration = s->block_duration;

        for (uint32_t i = 0; i < s->num_inputs; i++) {
            audio_mixer_input_t* in = &s->inputs[i];
            if (!in->active || !in->pending) continue;
            zst_audio_frame_t* af = (zst_audio_frame_t*)in->pending->payload;
            if (!af) continue;
            zst_time_t pts = in->pending->pts;
            if (!have_target || pts < mix_pts) {
                mix_pts = pts;
                have_target = true;
            }
            if (samples_to_mix == 0 && af->nb_samples > 0) {
                samples_to_mix = af->nb_samples;
            }
            if (mix_duration == 0) {
                mix_duration = in->pending->duration ? in->pending->duration
                                                     : audio_mixer_samples_to_ns(af->nb_samples, af->sample_rate ? af->sample_rate : s->sample_rate);
            }
        }

        if (!have_target || samples_to_mix == 0) {
            pthread_mutex_unlock(&s->mutex);
            continue;
        }
        if (mix_duration == 0) mix_duration = audio_mixer_samples_to_ns(samples_to_mix, s->sample_rate);
        if (mix_duration == 0) mix_duration = 10000000ULL; /* 10ms fallback */
        s->block_samples = samples_to_mix;
        s->block_duration = mix_duration;

        /* Drop fully stale pending buffers and replace them with newer queued
         * data if available.  This is the alignment-side QoS path; the clock
         * based max-lateness path runs at ingress in audio_mixer_sink_push(). */
        for (uint32_t i = 0; i < s->num_inputs; i++) {
            audio_mixer_input_t* in = &s->inputs[i];
            if (!in->active) continue;
            bool changed = true;
            while (changed) {
                changed = false;
                if (in->pending) {
                    zst_audio_frame_t* af = (zst_audio_frame_t*)in->pending->payload;
                    zst_time_t dur = in->pending->duration;
                    if (!dur && af) dur = audio_mixer_samples_to_ns(af->nb_samples, af->sample_rate ? af->sample_rate : s->sample_rate);
                    zst_time_t end = in->pending->pts + dur;
                    if (dur > 0 && end <= mix_pts) {
                        zst_buffer_unref(in->pending);
                        in->pending = NULL;
                        s->dropped_late++;
                        changed = true;
                    }
                }
                if (!in->pending && in->queue && zst_queue_pop(in->queue, &in->pending, 0) == ZST_OK && in->pending) {
                    changed = true;
                }
            }
        }

        /* ⚡ Optimization: reuse fmix buffer to avoid heap alloc/free per block.
         * calloc → realloc+memset has the same zeroing cost per iteration, but
         * eliminates the malloc/free syscall pair and reduces heap fragmentation. */
        size_t required_capacity = (size_t)samples_to_mix * s->channels;
        if (s->fmix_capacity < required_capacity) {
            double* new_fmix = realloc(s->fmix, required_capacity * sizeof(double));
            if (!new_fmix) {
                pthread_mutex_unlock(&s->mutex);
                continue;
            }
            s->fmix = new_fmix;
            s->fmix_capacity = required_capacity;
        }
        memset(s->fmix, 0, required_capacity * sizeof(double));
        double* fmix = s->fmix;

        zst_buffer_t* consumed[MAX_INPUTS];
        memset(consumed, 0, sizeof(consumed));

        for (uint32_t i = 0; i < s->num_inputs; i++) {
            audio_mixer_input_t* in = &s->inputs[i];
            if (!in->active || !in->pending) continue;

            zst_buffer_t* buf = in->pending;
            zst_audio_frame_t* af = (zst_audio_frame_t*)buf->payload;
            if (!af || !af->data || af->channels == 0) continue;

            zst_time_t start = buf->pts;
            if (start > mix_pts) {
                /* Future buffer: leave it pending and emit silence for this pad. */
                continue;
            }

            if (in->mute) {
                consumed[i] = in->pending;
                in->pending = NULL;
                continue;
            }

            uint32_t offset_samples = 0;
            if (mix_pts > start) {
                offset_samples = audio_mixer_ns_to_samples(mix_pts - start, af->sample_rate ? af->sample_rate : s->sample_rate);
                if (offset_samples >= af->nb_samples) continue;
            }

            uint32_t available = af->nb_samples - offset_samples;
            uint32_t frames = available < samples_to_mix ? available : samples_to_mix;
            uint32_t in_channels = af->channels;
            uint32_t out_channels = s->channels;
            double volume = in->volume;

            if (af->format == ZST_AUDIO_FMT_S16LE) {
                int16_t* src = (int16_t*)af->data;
                for (uint32_t f = 0; f < frames; f++) {
                    for (uint32_t c = 0; c < out_channels; c++) {
                        uint32_t sc = (c < in_channels) ? c : (in_channels - 1);
                        double gain = audio_mixer_calculate_gain(volume, out_channels, c, in->pan);
                        size_t si = ((size_t)offset_samples + f) * in_channels + sc;
                        size_t di = (size_t)f * out_channels + c;
                        fmix[di] += ((double)src[si] / 32768.0) * gain;
                    }
                }
            } else if (af->format == ZST_AUDIO_FMT_F32LE) {
                float* src = (float*)af->data;
                for (uint32_t f = 0; f < frames; f++) {
                    for (uint32_t c = 0; c < out_channels; c++) {
                        uint32_t sc = (c < in_channels) ? c : (in_channels - 1);
                        double gain = audio_mixer_calculate_gain(volume, out_channels, c, in->pan);
                        size_t si = ((size_t)offset_samples + f) * in_channels + sc;
                        size_t di = (size_t)f * out_channels + c;
                        fmix[di] += (double)src[si] * gain;
                    }
                }
            }

            /* This implementation consumes one complete input buffer per mixed
             * block.  Future buffers remain pending; gaps are filled above. */
            consumed[i] = in->pending;
            in->pending = NULL;
        }

        pthread_mutex_unlock(&s->mutex);

        zst_buffer_t* out_buf = zst_buffer_create_with_pool(s->pool);
        if (!out_buf) {
            out_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
            if (out_buf) {
                zst_audio_frame_t* oaf = calloc(1, sizeof(zst_audio_frame_t));
                if (!oaf) {
                    zst_buffer_unref(out_buf);
                    out_buf = NULL;
                } else {
                    uint32_t bpf = (s->format == ZST_AUDIO_FMT_S16LE) ? 2 : 4;
                    size_t data_size = (size_t)samples_to_mix * s->channels * bpf;
                    out_buf->payload = oaf;
                    out_buf->destroy = audio_mixer_buf_free;
                    out_buf->memory.data = calloc(1, data_size);
                    out_buf->memory.size = data_size;
                    oaf->data        = out_buf->memory.data;
                    oaf->sample_rate = s->sample_rate;
                    oaf->channels    = s->channels;
                    oaf->format      = s->format;
                    oaf->nb_samples  = samples_to_mix;
                }
            }
        } else {
            zst_audio_frame_t* oaf = (zst_audio_frame_t*)out_buf->payload;
            if (!oaf) {
                oaf = calloc(1, sizeof(zst_audio_frame_t));
                out_buf->payload = oaf;
                out_buf->destroy = audio_mixer_buf_free;
            }
            if (oaf) {
                oaf->sample_rate = s->sample_rate;
                oaf->channels    = s->channels;
                oaf->format      = s->format;
                oaf->nb_samples  = samples_to_mix;
                oaf->data        = out_buf->memory.data;
            }
        }

        if (out_buf) {
            out_buf->pts      = mix_pts;
            out_buf->duration = mix_duration;

            zst_audio_frame_t* oaf = (zst_audio_frame_t*)out_buf->payload;
            if (oaf && oaf->data) {
                if (s->format == ZST_AUDIO_FMT_S16LE) {
                    int16_t* dst = (int16_t*)oaf->data;
                    for (uint32_t j = 0; j < samples_to_mix * s->channels; j++) {
                        double val = fmix[j];
                        val = clip_double(val);
                        dst[j] = (int16_t)(val * 32767.0);
                    }
                } else {
                    float* dst = (float*)oaf->data;
                    for (uint32_t j = 0; j < samples_to_mix * s->channels; j++) {
                        double val = fmix[j];
                        val = clip_double(val);
                        dst[j] = (float)val;
                    }
                }
            }

            zst_pad_push(s->srcpad, out_buf);
            zst_buffer_unref(out_buf);
        }

        pthread_mutex_lock(&s->mutex);
        s->next_pts = mix_pts + mix_duration;
        s->have_next_pts = true;
        pthread_mutex_unlock(&s->mutex);

        for (uint32_t i = 0; i < MAX_INPUTS; i++) {
            if (consumed[i]) zst_buffer_unref(consumed[i]);
        }
        continue;

    worker_done:
        pthread_mutex_unlock(&s->mutex);
        break;
    }

    return NULL;
}
