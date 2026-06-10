/*=============================================================================
    srt_parser.c — SRT subtitle parser element
    Produces `ZST_BUFFER_USER` buffers with text in memory.data and pts/duration
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_srt.h"
#include "zst_clock.h"
#include <unistd.h>

typedef struct {
    char path[1024];
    zst_pad_t* srcpad;
    pthread_t thread;
    int running;
    struct {
        zst_time_t start_ns;
        zst_time_t duration_ns;
        char* text;
    } *entries;
    size_t n_entries;
} srt_parser_t;

static zst_time_t
time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (zst_time_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static zst_time_t
parse_timestamp_ns(const char* s)
{
    int hh, mm, ss, ms;
    if (sscanf(s, "%d:%d:%d,%d", &hh, &mm, &ss, &ms) == 4) {
        zst_time_t ns = hh;
        ns = (ns * 60 + mm) * 60 + ss;
        ns = ns * 1000 + ms;
        return ns * 1000000LL;
    }
    return 0;
}

static int
parse_srt_file(srt_parser_t* s)
{
    FILE* f = fopen(s->path, "r");
    if (!f) {
        ZST_LOG_ERROR("srt", "Could not open SRT file: %s", s->path);
        return -1;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    size_t cap = 16;
    s->entries = calloc(cap, sizeof(*s->entries));
    s->n_entries = 0;

    while ((read = getline(&line, &len, f)) != -1) {
        /* Skip leading index line */
        if (read == 0) continue;
        /* Trim whitespace */
        while (read > 0 && (line[read-1] == '\n' || line[read-1] == '\r')) line[--read] = '\0';
        if (read == 0) continue;

        /* If line is digits (index), skip */
        char* p = line;
        int all_digits = 1;
        while (*p) { if (*p < '0' || *p > '9') { all_digits = 0; break; } p++; }
        if (all_digits) {
            /* read timestamp line next */
            if ((read = getline(&line, &len, f)) == -1) break;
            while (read > 0 && (line[read-1] == '\n' || line[read-1] == '\r')) line[--read] = '\0';
            char* arrow = strstr(line, "-->");
            if (!arrow) continue;
            char start_str[64] = {0}, end_str[64] = {0};
            sscanf(line, "%63s --> %63s", start_str, end_str);
            zst_time_t start_ns = parse_timestamp_ns(start_str);
            zst_time_t end_ns = parse_timestamp_ns(end_str);
            zst_time_t dur_ns = (end_ns > start_ns) ? (end_ns - start_ns) : 0;

            /* Read text lines until blank line */
            size_t text_cap = 256;
            char* text = calloc(1, text_cap);
            size_t text_len = 0;
            while ((read = getline(&line, &len, f)) != -1) {
                while (read > 0 && (line[read-1] == '\n' || line[read-1] == '\r')) line[--read] = '\0';
                if (read == 0) break;
                if (text_len + read + 2 > text_cap) {
                    text_cap *= 2;
                    text = realloc(text, text_cap);
                }
                if (text_len > 0) text[text_len++] = '\n';
                memcpy(text + text_len, line, read);
                text_len += read;
                text[text_len] = '\0';
            }

            if (s->n_entries + 1 > cap) {
                cap *= 2;
                s->entries = realloc(s->entries, cap * sizeof(*s->entries));
            }
            s->entries[s->n_entries].start_ns = start_ns;
            s->entries[s->n_entries].duration_ns = dur_ns;
            s->entries[s->n_entries].text = text;
            s->n_entries++;
        }
    }

    free(line);
    fclose(f);
    return 0;
}

static void*
srt_thread_fn(void* arg)
{
    srt_parser_t* s = arg;
    if (!s) return NULL;

    zst_time_t base_ns = 0;
    if (s->srcpad && s->srcpad->parent && s->srcpad->parent->clock) {
        base_ns = zst_clock_get_time(s->srcpad->parent->clock);
    } else {
        base_ns = time_now_ns();
    }

    s->running = 1;
    for (size_t i = 0; i < s->n_entries && s->running; ++i) {
        zst_time_t target = base_ns + s->entries[i].start_ns;
        while (s->running) {
            zst_time_t now = 0;
            if (s->srcpad && s->srcpad->parent && s->srcpad->parent->clock) {
                now = zst_clock_get_time(s->srcpad->parent->clock);
            } else {
                now = time_now_ns();
            }
            if (now >= target) break;
            zst_time_t diff = target - now;
            struct timespec ts = { .tv_sec = diff / 1000000000LL, .tv_nsec = diff % 1000000000LL };
            if (ts.tv_sec == 0 && ts.tv_nsec > 0) {
                nanosleep(&ts, NULL);
            } else if (ts.tv_sec > 0) {
                sleep((unsigned int)ts.tv_sec);
            }
        }

        if (!s->running) break;

        /* Create buffer and push */
        zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_USER);
        if (!buf) continue;
        size_t tlen = strlen(s->entries[i].text) + 1;
        buf->memory.data = malloc(tlen);
        memcpy(buf->memory.data, s->entries[i].text, tlen);
        buf->memory.size = tlen;
        buf->type = ZST_BUFFER_USER;
        buf->pts = base_ns + s->entries[i].start_ns;
        buf->duration = s->entries[i].duration_ns;

        if (s->srcpad) {
            zst_pad_push(s->srcpad, buf);
        }
        zst_buffer_unref(buf);
    }

    s->running = 0;
    return NULL;
}

static zst_result_t srt_open(zst_element_t* el)
{
    srt_parser_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (parse_srt_file(s) != 0) return ZST_ERROR;
    return ZST_OK;
}

static zst_result_t srt_close(zst_element_t* el)
{
    srt_parser_t* s = el->priv;
    if (!s) return ZST_ERROR;
    for (size_t i = 0; i < s->n_entries; ++i) {
        free(s->entries[i].text);
    }
    free(s->entries);
    s->entries = NULL;
    s->n_entries = 0;
    return ZST_OK;
}

static zst_result_t srt_start(zst_element_t* el)
{
    srt_parser_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (pthread_create(&s->thread, NULL, srt_thread_fn, s) != 0) {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t srt_stop(zst_element_t* el)
{
    srt_parser_t* s = el->priv;
    if (!s) return ZST_ERROR;
    s->running = 0;
    pthread_join(s->thread, NULL);
    return ZST_OK;
}

static zst_caps_t* srt_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el; (void)pad; (void)filter;
    /* src pad is user/text payload — no strict caps required */
    return NULL;
}

static const zst_element_ops_t g_ops = {
    .name = "srt_parser",
    .open = srt_open,
    .close = srt_close,
    .start = srt_start,
    .stop = srt_stop,
    .process = NULL,
    .get_caps = srt_get_caps
};

zst_element_t* zst_srt_parser_create(const char* path)
{
    srt_parser_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    if (path) strncpy(priv->path, path, sizeof(priv->path)-1);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) { free(priv); return NULL; }

    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    if (strcmp(name, "srt_parser") == 0) {
        return zst_srt_parser_create(NULL);
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "srt_parser_plugin",
        .author = "zstreamer",
        .version = "0.1.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
