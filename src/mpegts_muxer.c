/*=============================================================================
    mpegts_muxer.c — FFmpeg libavformat MPEG-TS muxer implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_mpegts_muxer.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_log.h"

typedef struct {
    AVFormatContext* fc;
    uint8_t*         avio_buf;
    size_t           avio_buf_size;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              header_written;
    
    int              video_linked;
    int              audio_linked;
    int              video_eos;
    int              audio_eos;

    int              width;
    int              height;
    int              fps;
    int              sample_rate;
    int              channels;
    int              video_codec_id;

    int              direct_file;
    char             location[256];
} mpegts_muxer_t;

static void
mpegts_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static int
mpegts_mux_write_packet(void* opaque, uint8_t* buf, int buf_size)
{
    zst_element_t* el = opaque;
    
    zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!out_buf) return -1;
    
    uint8_t* data = malloc(buf_size);
    if (!data) {
        zst_buffer_unref(out_buf);
        return -1;
    }
    memcpy(data, buf, buf_size);
    
    out_buf->memory.type = ZST_MEMORY_CPU;
    out_buf->memory.data = data;
    out_buf->memory.size = buf_size;
    out_buf->destroy = mpegts_buf_free;
    
    zst_pad_t* src_pad = zst_element_get_pad(el, "src");
    if (src_pad && src_pad->peer) {
        zst_pad_push(src_pad, out_buf);
    }
    zst_buffer_unref(out_buf);
    return buf_size;
}

static zst_result_t
mpegts_mux_write_header(zst_element_t* el)
{
    mpegts_muxer_t* s = el->priv;
    
    if (avformat_alloc_output_context2(&s->fc, NULL, "mpegts", s->direct_file ? s->location : NULL) < 0) {
        ZST_LOG_ERROR("tsmux", "Failed to allocate output context");
        return ZST_ERROR;
    }

    if (s->direct_file) {
        if (avio_open(&s->fc->pb, s->location, AVIO_FLAG_WRITE) < 0) {
            ZST_LOG_ERROR("tsmux", "Failed to open output location: %s", s->location);
            avformat_free_context(s->fc);
            s->fc = NULL;
            return ZST_ERROR;
        }
    } else {
        s->avio_buf_size = 4096;
        s->avio_buf = av_malloc(s->avio_buf_size);
        s->fc->pb = avio_alloc_context(
            s->avio_buf, s->avio_buf_size,
            1, el, NULL, mpegts_mux_write_packet, NULL
        );
        if (!s->fc->pb) {
            avformat_free_context(s->fc);
            s->fc = NULL;
            return ZST_ERROR;
        }
        s->fc->pb->seekable = 0;
        s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    
    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");
    
    s->video_linked = (video_pad && video_pad->peer) ? 1 : 0;
    s->audio_linked = (audio_pad && audio_pad->peer) ? 1 : 0;
    
    if (s->video_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) return ZST_ERROR;
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = s->video_codec_id;
        st->codecpar->width = s->width;
        st->codecpar->height = s->height;
        st->time_base = (AVRational){1, 1000000000};
        s->video_stream_idx = st->index;
    }
    
    if (s->audio_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) return ZST_ERROR;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_AAC;
        st->codecpar->sample_rate = s->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        av_channel_layout_default(&st->codecpar->ch_layout, s->channels);
#else
        st->codecpar->channels = s->channels;
        st->codecpar->channel_layout = s->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
        st->codecpar->extradata_size = 2;
        st->codecpar->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (st->codecpar->extradata) {
            static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                         24000, 22050, 16000, 12000, 11025, 8000,
                                         7350 };
            int freq_idx = 4;
            for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++) {
                if (rates[i] == s->sample_rate) { freq_idx = i; break; }
            }
            int object_type = 2; // AAC LC
            st->codecpar->extradata[0] = (uint8_t)((object_type << 3) | (freq_idx >> 1));
            st->codecpar->extradata[1] = (uint8_t)(((freq_idx & 1) << 7) | (s->channels << 3));
        }
        st->time_base = (AVRational){1, 1000000000};
        s->audio_stream_idx = st->index;
    }
    
    AVDictionary* opts = NULL;
    if (avformat_write_header(s->fc, &opts) < 0) {
        ZST_LOG_ERROR("tsmux", "Failed to write header");
        av_dict_free(&opts);
        return ZST_ERROR;
    }
    av_dict_free(&opts);
    
    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static void
mpegts_mux_check_eos(zst_element_t* el)
{
    mpegts_muxer_t* s = el->priv;
    int video_done = !s->video_linked || s->video_eos;
    int audio_done = !s->audio_linked || s->audio_eos;
    
    if (video_done && audio_done) {
        if (s->fc && s->header_written) {
            av_write_trailer(s->fc);
            s->header_written = 0;
        }
        zst_pad_t* src_pad = zst_element_get_pad(el, "src");
        if (src_pad && src_pad->peer) {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(src_pad, eos_buf);
                zst_buffer_unref(eos_buf);
            }
        } else if (el->bus) {
            zst_bus_post(el->bus, zst_event_new_eos(el));
        }
    }
}

static zst_result_t
mpegts_mux_write(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    mpegts_muxer_t* s = el->priv;

    if (!s->header_written) return ZST_ERROR;
    
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
    
    if (av_interleaved_write_frame(s->fc, pkt) < 0) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    
    av_packet_free(&pkt);
    return ZST_OK;
}

static zst_result_t
mpegts_mux_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mpegts_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        mpegts_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        s->video_codec_id = AV_CODEC_ID_H264; // default
        if (pad->caps && pad->caps->structs) {
            const zst_caps_struct_t* caps = pad->caps->structs;
            if (strcmp(caps->media_type, "video/x-h265") == 0) {
                s->video_codec_id = AV_CODEC_ID_HEVC;
            }
            if (caps->video.width > 0) s->width = caps->video.width;
            if (caps->video.height > 0) s->height = caps->video.height;
        }
        if (mpegts_mux_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->video_stream_idx >= 0) {
        return mpegts_mux_write(el, buf, s->video_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mpegts_mux_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mpegts_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        mpegts_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        if (pad->caps && pad->caps->structs) {
            const zst_caps_struct_t* caps = pad->caps->structs;
            if (caps->audio.sample_rate > 0) s->sample_rate = caps->audio.sample_rate;
            if (caps->audio.channels > 0) s->channels = caps->audio.channels;
        }
        if (s->video_linked && !s->header_written) {
            // Wait for video packet to arrive so we can determine video codec (H264 vs H265)
            return ZST_OK;
        }
        if (mpegts_mux_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->audio_stream_idx >= 0) {
        return mpegts_mux_write(el, buf, s->audio_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mpegts_mux_start(zst_element_t* el)
{
    mpegts_muxer_t* s = el->priv;
    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");
    s->video_linked = (video_pad && video_pad->peer) ? 1 : 0;
    s->audio_linked = (audio_pad && audio_pad->peer) ? 1 : 0;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->header_written = 0;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static zst_result_t
mpegts_mux_stop(zst_element_t* el)
{
    mpegts_muxer_t* s = el->priv;
    if (s->fc && s->header_written) {
        av_write_trailer(s->fc);
    }
    
    if (s->fc) {
        if (s->fc->pb) {
            if (s->direct_file) {
                avio_closep(&s->fc->pb);
            } else {
                av_freep(&s->fc->pb->buffer);
                avio_context_free(&s->fc->pb);
            }
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    s->header_written = 0;
    return ZST_OK;
}

static zst_result_t
mpegts_mux_set_property(zst_element_t* el, const char* name, const char* value)
{
    mpegts_muxer_t* s = el->priv;
    int v = atoi(value);
    if (strcmp(name, "width") == 0) {
        if (v <= 0) return ZST_ERROR;
        s->width = v;
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        if (v <= 0) return ZST_ERROR;
        s->height = v;
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0 || strcmp(name, "framerate") == 0) {
        if (v <= 0) return ZST_ERROR;
        s->fps = v;
        return ZST_OK;
    } else if (strcmp(name, "sample-rate") == 0 || strcmp(name, "rate") == 0) {
        if (v <= 0) return ZST_ERROR;
        s->sample_rate = v;
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        if (v <= 0) return ZST_ERROR;
        s->channels = v;
        return ZST_OK;
    } else if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(s->location, sizeof(s->location), "%s", value);
        s->direct_file = s->location[0] != '\0';
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
mpegts_mux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mpegts_muxer_t* s = el->priv;
    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%d", s->width);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%d", s->height);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0 || strcmp(name, "framerate") == 0) {
        snprintf(value_out, max_len, "%d", s->fps);
        return ZST_OK;
    } else if (strcmp(name, "sample-rate") == 0 || strcmp(name, "rate") == 0) {
        snprintf(value_out, max_len, "%d", s->sample_rate);
        return ZST_OK;
    } else if (strcmp(name, "channels") == 0) {
        snprintf(value_out, max_len, "%d", s->channels);
        return ZST_OK;
    } else if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(value_out, max_len, "%s", s->location);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name  = "tsmux",
    .start = mpegts_mux_start,
    .stop  = mpegts_mux_stop,
    .set_property = mpegts_mux_set_property,
    .get_property = mpegts_mux_get_property,
};

zst_element_t*
zst_mpegts_muxer_create(void)
{
    zst_element_t* el;
    mpegts_muxer_t* priv;
    zst_pad_t* video;
    zst_pad_t* audio;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->width = 640;
    priv->height = 480;
    priv->fps = 30;
    priv->sample_rate = 44100;
    priv->channels = 2;
    priv->video_codec_id = AV_CODEC_ID_H264;

    el = zst_element_create(&g_ops, priv);

    video = zst_pad_create("video", ZST_PAD_SINK);
    audio = zst_pad_create("audio", ZST_PAD_SINK);
    src   = zst_pad_create("src",   ZST_PAD_SRC);

    video->push = mpegts_mux_video_push;
    audio->push = mpegts_mux_audio_push;

    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);
    zst_element_add_pad(el, src);

    return el;
}

zst_element_t*
zst_mpegts_muxer_create_with_config(const zst_mpegts_muxer_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_mpegts_muxer_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("tsmux");
    if (!el) return NULL;

    if (config->width > 0) {
        zst_element_set_property_uint(el, "width", config->width);
    }
    if (config->height > 0) {
        zst_element_set_property_uint(el, "height", config->height);
    }
    if (config->fps > 0) {
        zst_element_set_property_uint(el, "fps", config->fps);
    }
    if (config->sample_rate > 0) {
        zst_element_set_property_uint(el, "sample-rate", config->sample_rate);
    }
    if (config->channels > 0) {
        zst_element_set_property_uint(el, "channels", config->channels);
    }
    if (config->location) {
        zst_element_set_property_string(el, "location", config->location);
    }

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "tsmux") == 0) {
        return zst_mpegts_muxer_create();
    }
    return NULL;
}

static const zst_property_spec_t g_tsmux_properties[] = {
    { "width", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "sample-rate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Audio sample rate" },
    { "channels", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "Audio channels" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path (optional)" }
};

static const zst_pad_template_t g_tsmux_pads[] = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac;audio/x-opus" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/mpegts" }
};

static const zst_element_desc_t g_tsmux_elements[] = {
    {
        .name = "tsmux",
        .long_name = "MPEG-TS Muxer",
        .category = "Muxer/File",
        .description = "Muxes encoded audio/video into MPEG-TS (.ts)",
        .author = "zstreamer",
        .properties = g_tsmux_properties,
        .nb_properties = sizeof(g_tsmux_properties) / sizeof(g_tsmux_properties[0]),
        .pads = g_tsmux_pads,
        .nb_pads = sizeof(g_tsmux_pads) / sizeof(g_tsmux_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "mpegtsmuxer_plugin",
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
        *nb_elements_out = sizeof(g_tsmux_elements) / sizeof(g_tsmux_elements[0]);
    }
    return g_tsmux_elements;
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
