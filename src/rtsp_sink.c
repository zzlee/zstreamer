/*=============================================================================
    rtsp_sink.c — RTSP sink element using FFmpeg/libavformat
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_rtsp_sink.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"

typedef struct {
    AVFormatContext* fc;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              video_eos;
    int              audio_eos;
    int              header_written;
    char             url[512];
    char             mount_point[128];
    int              listen_port;
    int              max_clients;
    int              rtcp_interval_ms;
    char             transport[32];
} rtsp_sink_t;

static void
rtsp_sink_update_url(rtsp_sink_t* s)
{
    if (!s) return;
    if (s->mount_point[0] == '\0') {
        strncpy(s->mount_point, "live", sizeof(s->mount_point) - 1);
        s->mount_point[sizeof(s->mount_point) - 1] = '\0';
    }
    snprintf(s->url, sizeof(s->url), "rtsp://0.0.0.0:%d/%s", s->listen_port, s->mount_point);
}

static int
rtsp_sink_codec_id_for_media_type(const char* media_type)
{
    if (!media_type) return AV_CODEC_ID_NONE;
    if (strcmp(media_type, "video/x-h264") == 0) {
        return AV_CODEC_ID_H264;
    }
    if (strcmp(media_type, "video/x-h265") == 0) {
        return AV_CODEC_ID_HEVC;
    }
    if (strcmp(media_type, "audio/aac") == 0) {
        return AV_CODEC_ID_AAC;
    }
    return AV_CODEC_ID_NONE;
}

static zst_caps_t*
rtsp_sink_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)el;
    (void)filter;

    if (!pad) return NULL;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (strcmp(pad->name, "video") == 0) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h264", 0, 0, 0.0, ""));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-h265", 0, 0, 0.0, ""));
        return caps;
    }

    if (strcmp(pad->name, "audio") == 0) {
        zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
        return caps;
    }

    zst_caps_destroy(caps);
    return NULL;
}

static zst_result_t
rtsp_sink_create_stream(rtsp_sink_t* s, zst_pad_t* pad, int* out_stream_idx)
{
    if (!s || !pad || !out_stream_idx) return ZST_ERROR;
    if (!pad->caps || !pad->caps->structs) return ZST_ERROR;

    const zst_caps_struct_t* caps = pad->caps->structs;
    int codec_id = rtsp_sink_codec_id_for_media_type(caps->media_type);
    if (codec_id == AV_CODEC_ID_NONE) return ZST_ERROR;

    AVStream* st = avformat_new_stream(s->fc, NULL);
    if (!st) return ZST_ERROR;

    st->codecpar->codec_type = caps->type == ZST_CAPS_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = codec_id;

    if (caps->type == ZST_CAPS_VIDEO) {
        if (caps->video.width > 0) {
            st->codecpar->width = caps->video.width;
        }
        if (caps->video.height > 0) {
            st->codecpar->height = caps->video.height;
        }
        st->time_base = (AVRational){1, 1000000000};
    } else if (caps->type == ZST_CAPS_AUDIO) {
        if (caps->audio.sample_rate > 0) {
            st->codecpar->sample_rate = caps->audio.sample_rate;
        }
        if (caps->audio.channels > 0) {
            st->codecpar->channels = caps->audio.channels;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
            av_channel_layout_default(&st->codecpar->ch_layout, caps->audio.channels);
#else
            if (caps->audio.channels == 1) {
                st->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
            } else if (caps->audio.channels == 2) {
                st->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
            }
#endif
        }
        st->time_base = (AVRational){1, 1000000000};
    }

    *out_stream_idx = st->index;
    return ZST_OK;
}

static zst_result_t
rtsp_sink_open(zst_element_t* el)
{
    rtsp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    s->fc = NULL;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->video_eos = 0;
    s->audio_eos = 0;
    s->header_written = 0;

    if (s->url[0] == '\0') {
        rtsp_sink_update_url(s);
    }

    if (s->transport[0] == '\0') {
        strncpy(s->transport, "tcp", sizeof(s->transport) - 1);
        s->transport[sizeof(s->transport) - 1] = '\0';
    }

    avformat_network_init();
    return ZST_OK;
}

static zst_result_t
rtsp_sink_close(zst_element_t* el)
{
    rtsp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (s->fc) {
        if (s->header_written) {
            av_write_trailer(s->fc);
        }
        if (s->fc->pb) {
            avio_closep(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }

    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->header_written = 0;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static zst_result_t
rtsp_sink_stop(zst_element_t* el)
{
    return rtsp_sink_close(el);
}

static zst_result_t
rtsp_sink_write_packet(rtsp_sink_t* s, zst_buffer_t* buf, int stream_idx)
{
    if (!s || !s->fc || stream_idx < 0) return ZST_ERROR;
    if (!buf) return ZST_ERROR;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        return ZST_OK;
    }

    if (!buf->memory.data || buf->memory.size == 0) {
        return ZST_OK;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    if (av_new_packet(pkt, (int)buf->memory.size) < 0) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    memcpy(pkt->data, buf->memory.data, buf->memory.size);
    pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->duration = av_rescale_q(buf->duration, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->stream_index = stream_idx;

    int ret = av_interleaved_write_frame(s->fc, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        ZST_LOG_ERROR("rtspsink", "failed to write packet: %s", errbuf);
        return ZST_ERROR;
    }

    return ZST_OK;
}

static void
rtsp_sink_check_eos(zst_element_t* el)
{
    rtsp_sink_t* s = el->priv;
    if (!s || !s->fc) return;

    int all_eos = 1;
    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");

    if (video_pad && video_pad->peer && !s->video_eos) all_eos = 0;
    if (audio_pad && audio_pad->peer && !s->audio_eos) all_eos = 0;

    if (all_eos && s->header_written) {
        av_write_trailer(s->fc);
        s->header_written = 0;
    }
}

static zst_result_t
rtsp_sink_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtsp_sink_t* s = el->priv;

    if (!s) return ZST_ERROR;
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        rtsp_sink_check_eos(el);
        return ZST_OK;
    }

    if (s->video_stream_idx < 0) return ZST_OK;
    return rtsp_sink_write_packet(s, buf, s->video_stream_idx);
}

static zst_result_t
rtsp_sink_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtsp_sink_t* s = el->priv;

    if (!s) return ZST_ERROR;
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        rtsp_sink_check_eos(el);
        return ZST_OK;
    }

    if (s->audio_stream_idx < 0) return ZST_OK;
    return rtsp_sink_write_packet(s, buf, s->audio_stream_idx);
}

static zst_result_t
rtsp_sink_start(zst_element_t* el)
{
    rtsp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (s->url[0] == '\0') {
        rtsp_sink_update_url(s);
    }

    if (avformat_alloc_output_context2(&s->fc, NULL, "rtsp", s->url) < 0 || !s->fc) {
        ZST_LOG_ERROR("rtspsink", "failed to allocate RTSP output context for %s", s->url);
        return ZST_ERROR;
    }

    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");

    if (video_pad && video_pad->peer) {
        if (rtsp_sink_create_stream(s, video_pad, &s->video_stream_idx) != ZST_OK) {
            ZST_LOG_ERROR("rtspsink", "failed to create video RTSP stream");
            goto fail;
        }
    }

    if (audio_pad && audio_pad->peer) {
        if (rtsp_sink_create_stream(s, audio_pad, &s->audio_stream_idx) != ZST_OK) {
            ZST_LOG_ERROR("rtspsink", "failed to create audio RTSP stream");
            goto fail;
        }
    }

    if (s->video_stream_idx < 0 && s->audio_stream_idx < 0) {
        ZST_LOG_ERROR("rtspsink", "no linked sink pads available for RTSP output");
        goto fail;
    }

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "rtsp_flags", "listen", 0);
    av_dict_set(&opts, "listen", "1", 0);
    if (s->transport[0]) {
        av_dict_set(&opts, "rtsp_transport", s->transport, 0);
    }
    if (s->rtcp_interval_ms > 0) {
        char interval[32];
        snprintf(interval, sizeof(interval), "%d", s->rtcp_interval_ms * 1000);
        av_dict_set(&opts, "rtcp_interval", interval, 0);
    }

    if (avformat_write_header(s->fc, &opts) < 0) {
        ZST_LOG_ERROR("rtspsink", "failed to write RTSP header");
        av_dict_free(&opts);
        goto fail;
    }
    av_dict_free(&opts);

    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;

fail:
    if (s->fc) {
        if (s->fc->pb) {
            avio_closep(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    return ZST_ERROR;
}

static zst_result_t
rtsp_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !name || !value) return ZST_ERROR;
    rtsp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0) {
        strncpy(s->url, value, sizeof(s->url) - 1);
        s->url[sizeof(s->url) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "listen_port") == 0 || strcmp(name, "listen-port") == 0) {
        s->listen_port = atoi(value);
        if (s->listen_port <= 0) s->listen_port = 8554;
        rtsp_sink_update_url(s);
        return ZST_OK;
    }
    if (strcmp(name, "mount_point") == 0 || strcmp(name, "mount-point") == 0) {
        strncpy(s->mount_point, value, sizeof(s->mount_point) - 1);
        s->mount_point[sizeof(s->mount_point) - 1] = '\0';
        rtsp_sink_update_url(s);
        return ZST_OK;
    }
    if (strcmp(name, "transport") == 0) {
        strncpy(s->transport, value, sizeof(s->transport) - 1);
        s->transport[sizeof(s->transport) - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "max_clients") == 0 || strcmp(name, "max-clients") == 0) {
        s->max_clients = atoi(value);
        if (s->max_clients <= 0) s->max_clients = 1;
        return ZST_OK;
    }
    if (strcmp(name, "rtcp_interval") == 0 || strcmp(name, "rtcp-interval") == 0 ||
        strcmp(name, "rtcp-interval-ms") == 0) {
        s->rtcp_interval_ms = atoi(value);
        if (s->rtcp_interval_ms < 0) s->rtcp_interval_ms = 0;
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
rtsp_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !name || !value_out || max_len == 0) return ZST_ERROR;
    rtsp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    if (strcmp(name, "url") == 0) {
        strncpy(value_out, s->url, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "listen_port") == 0 || strcmp(name, "listen-port") == 0) {
        snprintf(value_out, max_len, "%d", s->listen_port);
        return ZST_OK;
    }
    if (strcmp(name, "mount_point") == 0 || strcmp(name, "mount-point") == 0) {
        strncpy(value_out, s->mount_point, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "transport") == 0) {
        strncpy(value_out, s->transport, max_len - 1);
        value_out[max_len - 1] = '\0';
        return ZST_OK;
    }
    if (strcmp(name, "max_clients") == 0 || strcmp(name, "max-clients") == 0) {
        snprintf(value_out, max_len, "%d", s->max_clients);
        return ZST_OK;
    }
    if (strcmp(name, "rtcp_interval") == 0 || strcmp(name, "rtcp-interval") == 0 ||
        strcmp(name, "rtcp-interval-ms") == 0) {
        snprintf(value_out, max_len, "%d", s->rtcp_interval_ms);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static const zst_element_ops_t g_ops = {
    .name = "rtspsink",
    .open = rtsp_sink_open,
    .close = rtsp_sink_close,
    .start = rtsp_sink_start,
    .stop = rtsp_sink_stop,
    .get_caps = rtsp_sink_get_caps,
    .set_property = rtsp_sink_set_property,
    .get_property = rtsp_sink_get_property,
};

zst_element_t*
zst_rtsp_sink_create(void)
{
    rtsp_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->listen_port = 8554;
    priv->max_clients = 1;
    priv->rtcp_interval_ms = 5000;
    strncpy(priv->mount_point, "live", sizeof(priv->mount_point) - 1);
    strncpy(priv->transport, "tcp", sizeof(priv->transport) - 1);
    rtsp_sink_update_url(priv);

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* video = zst_pad_create("video", ZST_PAD_SINK);
    zst_pad_t* audio = zst_pad_create("audio", ZST_PAD_SINK);
    if (!video || !audio) {
        zst_element_destroy(el);
        return NULL;
    }

    video->push = rtsp_sink_video_push;
    audio->push = rtsp_sink_audio_push;

    if (zst_element_add_pad(el, video) != ZST_OK ||
        zst_element_add_pad(el, audio) != ZST_OK) {
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}



zst_element_t*
zst_rtsp_sink_create_with_config(const zst_rtsp_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_rtsp_sink_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("rtspsink");
    if (!el) return NULL;

    if (config->url) {
        zst_element_set_property_string(el, "url", config->url);
    }
    if (config->listen_port > 0) {
        zst_element_set_property_int(el, "listen-port", config->listen_port);
    }
    if (config->mount_point) {
        zst_element_set_property_string(el, "mount-point", config->mount_point);
    }
    if (config->transport) {
        zst_element_set_property_string(el, "transport", config->transport);
    }
    if (config->max_clients > 0) {
        zst_element_set_property_int(el, "max-clients", config->max_clients);
    }
    if (config->rtcp_interval_ms > 0) {
        zst_element_set_property_int(el, "rtcp-interval-ms", config->rtcp_interval_ms);
    }

    return el;
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtspsink") == 0) {
        return zst_rtsp_sink_create();
    }
    return NULL;
}

static const zst_pad_template_t g_rtspsink_pads[] = {
    { "video", ZST_PAD_SINK, "ANY" },
    { "audio", ZST_PAD_SINK, "ANY" }
};

static const zst_property_spec_t g_rtspsink_properties[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "rtsp://0.0.0.0:8554/live", "RTSP listen URL" },
    { "listen-port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "8554", "RTSP listen port" },
    { "mount-point", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "live", "RTSP mount point" },
    { "transport", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "tcp", "Preferred RTSP transport" },
    { "max-clients", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Maximum concurrent clients requested by the application" },
    { "rtcp-interval-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "5000", "RTCP sender report interval in milliseconds" }
};

static const zst_element_desc_t g_rtspsink_elements[] = {
    {
        .name = "rtspsink",
        .long_name = "RTSP Sink",
        .category = "Sink/Network",
        .description = "Publishes audio/video to an RTSP endpoint",
        .author = "zstreamer",
        .properties = g_rtspsink_properties,
        .nb_properties = sizeof(g_rtspsink_properties) / sizeof(g_rtspsink_properties[0]),
        .pads = g_rtspsink_pads,
        .nb_pads = sizeof(g_rtspsink_pads) / sizeof(g_rtspsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtspsink_plugin",
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
        *nb_elements_out = sizeof(g_rtspsink_elements) / sizeof(g_rtspsink_elements[0]);
    }
    return g_rtspsink_elements;
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
