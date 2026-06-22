/*=============================================================================
    http_source.c — Reads buffers from an HTTP/HTTPS resource
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <libavformat/avformat.h>
#include <libavutil/opt.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_http_source.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_log.h"
#include "zst_clock.h"

typedef struct {
    AVIOContext* pb;
    char url[512];
    char user_agent[256];
    char headers[512];
    int timeout_ms;
    int chunk_size;
    bool reconnect;
    int reconnect_delay_ms;
    int max_reconnect_attempts;
    uint64_t bytes_read;
    int64_t content_length;
    zst_buffer_pool_t* pool;
} http_source_t;

static bool
has_extension(const char* ext, const char* target)
{
    if (!ext || !target) return false;
    while (*ext && *target) {
        char c1 = *ext;
        char c2 = *target;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
        ext++;
        target++;
    }
    return *ext == '\0' && *target == '\0';
}

static const char*
http_source_determine_media_type(const char* url, const char* mime_type)
{
    if (mime_type && strlen(mime_type) > 0) {
        return mime_type;
    }
    if (!url) return "application/octet-stream";
    
    char path[512];
    strncpy(path, url, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char* q = strchr(path, '?');
    if (q) *q = '\0';
    
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    
    if (has_extension(ext, ".h264") || has_extension(ext, ".264")) {
        return "video/x-h264";
    } else if (has_extension(ext, ".h265") || has_extension(ext, ".265")) {
        return "video/x-h265";
    } else if (has_extension(ext, ".aac")) {
        return "audio/aac";
    } else if (has_extension(ext, ".txt")) {
        return "text/plain";
    } else if (has_extension(ext, ".mp4")) {
        return "video/mp4";
    } else if (has_extension(ext, ".yuv")) {
        return "video/x-raw";
    } else if (has_extension(ext, ".pcm")) {
        return "audio/x-raw";
    }
    return "application/octet-stream";
}

static uint32_t
determine_buffer_type(const char* media_type)
{
    if (strcmp(media_type, "video/x-raw") == 0) {
        return ZST_BUFFER_VIDEO_FRAME;
    } else if (strcmp(media_type, "audio/x-raw") == 0) {
        return ZST_BUFFER_AUDIO_FRAME;
    } else if (strcmp(media_type, "video/x-h264") == 0) {
        return ZST_BUFFER_VIDEO_PACKET;
    } else if (strcmp(media_type, "video/x-h265") == 0) {
        return ZST_BUFFER_VIDEO_PACKET;
    } else if (strcmp(media_type, "audio/aac") == 0) {
        return ZST_BUFFER_AUDIO_PACKET;
    }
    return ZST_BUFFER_USER;
}

static zst_result_t
http_source_connect(zst_element_t* el)
{
    http_source_t* s = el->priv;
    if (!s || s->url[0] == '\0') {
        ZST_LOG_ERROR("httpsrc", "URL is empty");
        return ZST_ERROR;
    }

    if (s->pb) {
        avio_closep(&s->pb);
        s->pb = NULL;
    }

    AVDictionary* opts = NULL;
    
    // Set User-Agent
    if (s->user_agent[0]) {
        av_dict_set(&opts, "user_agent", s->user_agent, 0);
    } else {
        av_dict_set(&opts, "user_agent", "zstreamer/0.1.0", 0);
    }

    // Set custom headers
    if (s->headers[0]) {
        av_dict_set(&opts, "headers", s->headers, 0);
    }

    // Set timeout (rw_timeout is in microseconds)
    int64_t timeout_us = (int64_t)s->timeout_ms * 1000;
    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%lld", (long long)timeout_us);
    av_dict_set(&opts, "rw_timeout", timeout_str, 0);
    av_dict_set(&opts, "timeout", timeout_str, 0);

    // If we have read some bytes already, request that offset directly
    if (s->bytes_read > 0) {
        char offset_str[32];
        snprintf(offset_str, sizeof(offset_str), "%llu", (unsigned long long)s->bytes_read);
        av_dict_set(&opts, "offset", offset_str, 0);
        ZST_LOG_INFO("httpsrc", "Requesting URL with offset %llu", (unsigned long long)s->bytes_read);
    }

    ZST_LOG_INFO("httpsrc", "Connecting to %s", s->url);
    int ret = avio_open2(&s->pb, s->url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        ZST_LOG_ERROR("httpsrc", "Failed to open URL %s: %s (%d)", s->url, errbuf, ret);
        return ZST_ERROR;
    }

    // Retrieve Content-Length if possible
    int64_t size = avio_size(s->pb);
    if (size > 0) {
        s->content_length = size;
        ZST_LOG_INFO("httpsrc", "Content-Length: %lld", (long long)s->content_length);
    } else {
        s->content_length = -1;
        ZST_LOG_INFO("httpsrc", "Content-Length: unknown");
    }

    return ZST_OK;
}

static zst_result_t
http_source_open(zst_element_t* el)
{
    http_source_t* s = el->priv;
    s->bytes_read = 0;

    if (s->chunk_size <= 0) {
        s->chunk_size = 4096;
    }

    avformat_network_init();

    zst_result_t ret = http_source_connect(el);
    if (ret != ZST_OK) {
        return ret;
    }

    // Set caps on src pad based on Content-Type/mime_type
    char* mime_type = NULL;
    av_opt_get(s->pb, "mime_type", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&mime_type);
    const char* media_type = http_source_determine_media_type(s->url, mime_type);

    zst_caps_t* caps = zst_caps_create();
    zst_caps_struct_t* caps_struct = NULL;
    if (strncmp(media_type, "video/", 6) == 0) {
        caps_struct = zst_caps_struct_create_video(media_type, 0, 0, 0.0, "");
    } else if (strncmp(media_type, "audio/", 6) == 0) {
        caps_struct = zst_caps_struct_create_audio(media_type, 0, 0, "");
    } else {
        caps_struct = calloc(1, sizeof(*caps_struct));
        if (caps_struct) {
            strncpy(caps_struct->media_type, media_type, sizeof(caps_struct->media_type) - 1);
            caps_struct->type = ZST_CAPS_ANY;
        }
    }
    if (caps_struct) {
        zst_caps_append(caps, caps_struct);
    }
    
    zst_pad_t* src_pad = zst_element_get_pad(el, "src");
    if (src_pad) {
        zst_pad_set_caps(src_pad, caps);
    }
    zst_caps_destroy(caps);
    if (mime_type) {
        av_free(mime_type);
    }

    uint32_t buffer_type = determine_buffer_type(media_type);
    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = (size_t)s->chunk_size,
        .buffer_type = buffer_type
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        ZST_LOG_ERROR("httpsrc", "failed to create buffer pool");
        if (s->pb) {
            avio_closep(&s->pb);
            s->pb = NULL;
        }
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t
http_source_close(zst_element_t* el)
{
    http_source_t* s = el->priv;
    if (s->pb) {
        avio_closep(&s->pb);
        s->pb = NULL;
    }
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    avformat_network_deinit();
    return ZST_OK;
}

static zst_result_t
http_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    http_source_t* s = el->priv;

    if (!s->pb) {
        return ZST_ERROR;
    }

    size_t to_read = (size_t)s->chunk_size;
    if (s->content_length >= 0) {
        if (s->bytes_read >= (uint64_t)s->content_length) {
            return ZST_EOF;
        }
        if ((uint64_t)s->content_length - s->bytes_read < to_read) {
            to_read = (size_t)((uint64_t)s->content_length - s->bytes_read);
        }
    }

    if (to_read == 0) {
        return ZST_EOF;
    }

    zst_buffer_t* buf = NULL;
    if (zst_buffer_pool_acquire(s->pool, &buf, 0, 0) != ZST_OK) {
        return ZST_ERROR;
    }

    buf->memory.size = (size_t)s->chunk_size;

    int n = 0;
    int reconnect_attempts = 0;
    while (1) {
        n = avio_read(s->pb, buf->memory.data, (int)to_read);
        if (n > 0) {
            break;
        } else if (n == AVERROR_EOF) {
            zst_buffer_unref(buf);
            return ZST_EOF;
        } else {
            char errbuf[128];
            av_strerror(n, errbuf, sizeof(errbuf));
            ZST_LOG_WARN("httpsrc", "Read error: %s (%d)", errbuf, n);

            if (s->reconnect && (s->max_reconnect_attempts < 0 || reconnect_attempts < s->max_reconnect_attempts)) {
                reconnect_attempts++;
                ZST_LOG_INFO("httpsrc", "Reconnecting (attempt %d)...", reconnect_attempts);
                
                if (s->reconnect_delay_ms > 0) {
                    struct timespec ts;
                    ts.tv_sec = s->reconnect_delay_ms / 1000;
                    ts.tv_nsec = (long)(s->reconnect_delay_ms % 1000) * 1000000L;
                    nanosleep(&ts, NULL);
                }
                
                if (http_source_connect(el) == ZST_OK) {
                    continue;
                }
            }
            
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
    }

    buf->memory.size = n;
    s->bytes_read += n;

    if (el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = 0;
    }
    buf->duration = 0;

    *out = buf;
    return ZST_OK;
}

static zst_caps_t*
http_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)pad;
    (void)filter;
    http_source_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    char* mime_type = NULL;
    if (s->pb) {
        av_opt_get(s->pb, "mime_type", AV_OPT_SEARCH_CHILDREN, (uint8_t**)&mime_type);
    }
    const char* media_type = http_source_determine_media_type(s->url, mime_type);

    zst_caps_struct_t* caps_struct = NULL;
    if (strncmp(media_type, "video/", 6) == 0) {
        caps_struct = zst_caps_struct_create_video(media_type, 0, 0, 0.0, "");
    } else if (strncmp(media_type, "audio/", 6) == 0) {
        caps_struct = zst_caps_struct_create_audio(media_type, 0, 0, "");
    } else {
        caps_struct = calloc(1, sizeof(*caps_struct));
        if (caps_struct) {
            strncpy(caps_struct->media_type, media_type, sizeof(caps_struct->media_type) - 1);
            caps_struct->type = ZST_CAPS_ANY;
        }
    }

    if (caps_struct) {
        zst_caps_append(caps, caps_struct);
    }
    if (mime_type) {
        av_free(mime_type);
    }
    return caps;
}

static zst_result_t
http_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    http_source_t* s = el->priv;
    if (strcmp(name, "url") == 0 || strcmp(name, "uri") == 0) {
        strncpy(s->url, value, sizeof(s->url) - 1);
        s->url[sizeof(s->url) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "user-agent") == 0 || strcmp(name, "user_agent") == 0) {
        strncpy(s->user_agent, value, sizeof(s->user_agent) - 1);
        s->user_agent[sizeof(s->user_agent) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "headers") == 0) {
        strncpy(s->headers, value, sizeof(s->headers) - 1);
        s->headers[sizeof(s->headers) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "timeout") == 0) {
        s->timeout_ms = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "chunk-size") == 0 || strcmp(name, "chunk_size") == 0) {
        s->chunk_size = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "reconnect") == 0) {
        s->reconnect = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    } else if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        s->reconnect_delay_ms = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        s->max_reconnect_attempts = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
http_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    http_source_t* s = el->priv;
    if (strcmp(name, "url") == 0 || strcmp(name, "uri") == 0) {
        strncpy(value_out, s->url, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "user-agent") == 0 || strcmp(name, "user_agent") == 0) {
        strncpy(value_out, s->user_agent, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "headers") == 0) {
        strncpy(value_out, s->headers, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "timeout") == 0) {
        snprintf(value_out, max_len, "%d", s->timeout_ms);
        return ZST_OK;
    } else if (strcmp(name, "chunk-size") == 0 || strcmp(name, "chunk_size") == 0) {
        snprintf(value_out, max_len, "%d", s->chunk_size);
        return ZST_OK;
    } else if (strcmp(name, "reconnect") == 0) {
        snprintf(value_out, max_len, "%s", s->reconnect ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        snprintf(value_out, max_len, "%d", s->reconnect_delay_ms);
        return ZST_OK;
    } else if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        snprintf(value_out, max_len, "%d", s->max_reconnect_attempts);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    http_source_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "httpsrc",
    .open = http_source_open,
    .close = http_source_close,
    .process = http_source_process,
    .get_caps = http_source_get_caps,
    .set_property = http_source_set_property,
    .get_property = http_source_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_http_source_create(const char* url)
{
    zst_element_t* el;
    http_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (url) {
        strncpy(priv->url, url, sizeof(priv->url) - 1);
        priv->url[sizeof(priv->url) - 1] = '\0';
    }
    
    strncpy(priv->user_agent, "zstreamer/0.1.0", sizeof(priv->user_agent) - 1);
    priv->timeout_ms = 5000;
    priv->chunk_size = 4096;
    priv->reconnect = false;
    priv->reconnect_delay_ms = 500;
    priv->max_reconnect_attempts = -1;
    priv->content_length = -1;

    el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    src = zst_pad_create("src", ZST_PAD_SRC);
    if (!src) {
        zst_element_destroy(el);
        return NULL;
    }

    zst_element_add_pad(el, src);
    return el;
}



zst_element_t*
zst_http_source_create_with_config(const zst_http_source_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_http_source_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("httpsrc");
    if (!el) return NULL;

    if (config->url) {
        zst_element_set_property_string(el, "url", config->url);
    }
    if (config->uri) {
        zst_element_set_property_string(el, "uri", config->uri);
    }
    if (config->user_agent) {
        zst_element_set_property_string(el, "user-agent", config->user_agent);
    }
    if (config->headers) {
        zst_element_set_property_string(el, "headers", config->headers);
    }
    if (config->timeout > 0) {
        zst_element_set_property_int(el, "timeout", config->timeout);
    }
    if (config->chunk_size > 0) {
        zst_element_set_property_uint(el, "chunk-size", config->chunk_size);
    }
    zst_element_set_property_bool(el, "reconnect", config->reconnect);
    if (config->reconnect_delay_ms > 0) {
        zst_element_set_property_int(el, "reconnect-delay-ms", config->reconnect_delay_ms);
    }
    if (config->max_reconnect_attempts > 0) {
        zst_element_set_property_int(el, "max-reconnect-attempts", config->max_reconnect_attempts);
    }

    return el;
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "httpsrc") == 0 || strcmp(name, "http_source") == 0) {
        return zst_http_source_create("");
    }
    return NULL;
}

static const zst_property_spec_t g_httpsrc_properties[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "URL of the HTTP/HTTPS resource" },
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "URI of the HTTP/HTTPS resource" },
    { "user-agent", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "zstreamer/0.1.0", "User-Agent header value" },
    { "headers", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "", "Custom HTTP request headers" },
    { "timeout", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "5000", "Connection & read timeout in milliseconds" },
    { "chunk-size", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "4096", "Maximum bytes to read per buffer" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "false", "Reconnect on connection loss" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "500", "Delay between reconnect attempts in milliseconds" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "-1", "Maximum reconnect attempts; -1 means unlimited" }
};

static const zst_pad_template_t g_httpsrc_pads[] = {
    { "src", ZST_PAD_SRC, "ANY" }
};

static const zst_element_desc_t g_httpsrc_elements[] = {
    {
        .name = "httpsrc",
        .long_name = "HTTP Source",
        .category = "Source/Network",
        .description = "Reads buffers from HTTP/HTTPS server",
        .author = "zstreamer",
        .properties = g_httpsrc_properties,
        .nb_properties = sizeof(g_httpsrc_properties) / sizeof(g_httpsrc_properties[0]),
        .pads = g_httpsrc_pads,
        .nb_pads = sizeof(g_httpsrc_pads) / sizeof(g_httpsrc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "httpsrc_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_httpsrc_elements) / sizeof(g_httpsrc_elements[0]);
    }
    return g_httpsrc_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
