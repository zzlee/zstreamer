/*=============================================================================
    mp4_muxer.c — FFmpeg libavformat MP4 muxer implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_mp4_muxer.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_media_utils.h"

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

    uint8_t*         video_extradata;
    int              video_extradata_size;
    int              video_annexb;
    int              direct_file;
    char             location[256];
} mp4_muxer_t;

void
mp4_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static int
mp4_aac_freq_index(int sample_rate)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                 24000, 22050, 16000, 12000, 11025, 8000,
                                 7350 };
    for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++) {
        if (rates[i] == sample_rate) return i;
    }
    return 4; /* 44100 */
}

static int
mp4_copy_nal(uint8_t** dst, int* dst_size, const uint8_t* nal, int nal_size)
{
    uint8_t* p = av_mallocz(nal_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!p) return 0;
    memcpy(p, nal, nal_size);
    *dst = p;
    *dst_size = nal_size;
    return 1;
}

static int
mp4_parse_h264_extradata(mp4_muxer_t* s, const uint8_t* data, int size)
{
    if (!s || !data || size <= 0 || s->video_extradata) return s && s->video_extradata;

    uint8_t* sps = NULL;
    uint8_t* pps = NULL;
    int sps_size = 0;
    int pps_size = 0;

    int code_size = 0;
    int sc = zst_find_start_code(data, size, 0, &code_size);
    if (sc >= 0) {
        s->video_annexb = 1;
        int pos = sc;
        while (pos >= 0 && pos < size) {
            int nal_start = pos + code_size;
            int next_code_size = 0;
            int next = zst_find_start_code(data, size, nal_start, &next_code_size);
            int nal_end = next >= 0 ? next : size;
            while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
            int nal_size = nal_end - nal_start;
            if (nal_size > 0) {
                int nal_type = data[nal_start] & 0x1f;
                if (nal_type == 7 && !sps) {
                    mp4_copy_nal(&sps, &sps_size, data + nal_start, nal_size);
                } else if (nal_type == 8 && !pps) {
                    mp4_copy_nal(&pps, &pps_size, data + nal_start, nal_size);
                }
            }
            if (next < 0) break;
            code_size = next_code_size;
            pos = next;
        }
    } else if (size >= 5) {
        s->video_annexb = 0;
        int pos = 0;
        while (pos + 4 <= size) {
            int nal_size = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
            pos += 4;
            if (nal_size <= 0 || pos + nal_size > size) break;
            int nal_type = data[pos] & 0x1f;
            if (nal_type == 7 && !sps) {
                mp4_copy_nal(&sps, &sps_size, data + pos, nal_size);
            } else if (nal_type == 8 && !pps) {
                mp4_copy_nal(&pps, &pps_size, data + pos, nal_size);
            }
            pos += nal_size;
        }
    }

    if (!sps || !pps || sps_size < 4) {
        av_free(sps);
        av_free(pps);
        return 0;
    }

    int extra_size = 11 + sps_size + pps_size;
    uint8_t* extra = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extra) {
        av_free(sps);
        av_free(pps);
        return 0;
    }

    int p = 0;
    extra[p++] = 1;
    extra[p++] = sps[1];
    extra[p++] = sps[2];
    extra[p++] = sps[3];
    extra[p++] = 0xff;
    extra[p++] = 0xe1;
    extra[p++] = (uint8_t)(sps_size >> 8);
    extra[p++] = (uint8_t)(sps_size & 0xff);
    memcpy(extra + p, sps, sps_size);
    p += sps_size;
    extra[p++] = 1;
    extra[p++] = (uint8_t)(pps_size >> 8);
    extra[p++] = (uint8_t)(pps_size & 0xff);
    memcpy(extra + p, pps, pps_size);
    p += pps_size;

    av_free(sps);
    av_free(pps);
    s->video_extradata = extra;
    s->video_extradata_size = p;
    return 1;
}

static uint8_t*
mp4_annexb_to_avcc(const uint8_t* data, int size, int* out_size)
{
    *out_size = 0;
    int code_size = 0;
    int pos = zst_find_start_code(data, size, 0, &code_size);
    if (pos < 0) return NULL;

    uint8_t* out = malloc((size_t)size + 4);
    if (!out) return NULL;
    int out_pos = 0;

    while (pos >= 0 && pos < size) {
        int nal_start = pos + code_size;
        int next_code_size = 0;
        int next = zst_find_start_code(data, size, nal_start, &next_code_size);
        int nal_end = next >= 0 ? next : size;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        int nal_size = nal_end - nal_start;
        if (nal_size > 0) {
            out[out_pos++] = (uint8_t)(nal_size >> 24);
            out[out_pos++] = (uint8_t)(nal_size >> 16);
            out[out_pos++] = (uint8_t)(nal_size >> 8);
            out[out_pos++] = (uint8_t)(nal_size);
            memcpy(out + out_pos, data + nal_start, nal_size);
            out_pos += nal_size;
        }
        if (next < 0) break;
        code_size = next_code_size;
        pos = next;
    }

    *out_size = out_pos;
    return out;
}

static int
mp4_mux_write_packet(void* opaque, uint8_t* buf, int buf_size)
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
    out_buf->destroy = mp4_buf_free;
    
    zst_pad_t* src_pad = zst_element_get_pad(el, "src");
    if (src_pad && src_pad->peer) {
        zst_pad_push(src_pad, out_buf);
    }
    
    zst_buffer_unref(out_buf);
    return buf_size;
}

static zst_result_t
mp4_mux_write_header(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    
    if (avformat_alloc_output_context2(&s->fc, NULL, "mp4", s->direct_file ? s->location : NULL) < 0) {
        return ZST_ERROR;
    }

    if (s->direct_file) {
        if (avio_open(&s->fc->pb, s->location, AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(s->fc);
            s->fc = NULL;
            return ZST_ERROR;
        }
    } else {
        s->avio_buf_size = 4096;
        s->avio_buf = av_malloc(s->avio_buf_size);
        s->fc->pb = avio_alloc_context(
            s->avio_buf, s->avio_buf_size,
            1, el, NULL, mp4_mux_write_packet, NULL
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
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264;
        st->codecpar->width = s->width;
        st->codecpar->height = s->height;
        if (s->video_extradata && s->video_extradata_size > 0) {
            st->codecpar->extradata = av_mallocz(s->video_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!st->codecpar->extradata) return ZST_ERROR;
            memcpy(st->codecpar->extradata, s->video_extradata, s->video_extradata_size);
            st->codecpar->extradata_size = s->video_extradata_size;
        }
        st->time_base = (AVRational){1, 1000000000};
        s->video_stream_idx = st->index;
    }
    
    if (s->audio_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
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
        if (!st->codecpar->extradata) return ZST_ERROR;
        int freq_idx = mp4_aac_freq_index(s->sample_rate);
        int object_type = 2; /* AAC LC */
        st->codecpar->extradata[0] = (uint8_t)((object_type << 3) | (freq_idx >> 1));
        st->codecpar->extradata[1] = (uint8_t)(((freq_idx & 1) << 7) | (s->channels << 3));
        st->time_base = (AVRational){1, 1000000000};
        s->audio_stream_idx = st->index;
    }
    
    AVDictionary* opts = NULL;
    if (!s->direct_file) {
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof+frag_discont", 0);
        av_dict_set(&opts, "avoid_negative_ts", "disabled", 0);
    }
    if (avformat_write_header(s->fc, &opts) < 0) {
        av_dict_free(&opts);
        return ZST_ERROR;
    }
    av_dict_free(&opts);
    
    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static zst_result_t
mp4_mux_write(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    mp4_muxer_t* s = el->priv;

    if (!s->header_written) return ZST_ERROR;
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    uint8_t* converted = NULL;
    uint8_t* packet_data = buf->memory.data;
    int packet_size = (int)buf->memory.size;
    int converted_size = 0;
    if (stream_idx == s->video_stream_idx && s->video_annexb) {
        converted = mp4_annexb_to_avcc(buf->memory.data, (int)buf->memory.size, &converted_size);
        if (converted && converted_size > 0) {
            packet_data = converted;
            packet_size = converted_size;
        }
    }

    if (av_new_packet(pkt, packet_size) < 0) {
        free(converted);
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    memcpy(pkt->data, packet_data, packet_size);

    pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->duration = av_rescale_q(buf->duration, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->stream_index = stream_idx;


    if (av_interleaved_write_frame(s->fc, pkt) < 0) {
        free(converted);
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    
    free(converted);
    av_packet_free(&pkt);
    return ZST_OK;
}

static void
mp4_mux_check_eos(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    int all_eos = 1;
    if (s->video_linked && !s->video_eos) all_eos = 0;
    if (s->audio_linked && !s->audio_eos) all_eos = 0;
    
    if (all_eos) {
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
mp4_mux_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        mp4_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        if (!mp4_parse_h264_extradata(s, buf->memory.data, (int)buf->memory.size)) {
            return ZST_ERROR;
        }
        if (mp4_mux_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->video_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->video_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mp4_mux_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        mp4_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        if (s->video_linked && !s->video_extradata) {
            return ZST_OK;
        }
        if (mp4_mux_write_header(el) != ZST_OK) return ZST_ERROR;
    }

    if (s->audio_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->audio_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mp4_mux_start(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
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
mp4_mux_stop(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
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
    if (s->video_extradata) {
        av_free(s->video_extradata);
        s->video_extradata = NULL;
        s->video_extradata_size = 0;
    }
    s->header_written = 0;
    return ZST_OK;
}

static zst_result_t
mp4_mux_set_property(zst_element_t* el, const char* name, const char* value)
{
    mp4_muxer_t* s = el->priv;
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
mp4_mux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mp4_muxer_t* s = el->priv;
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
    .name  = "mp4mux",
    .start = mp4_mux_start,
    .stop  = mp4_mux_stop,
    .set_property = mp4_mux_set_property,
    .get_property = mp4_mux_get_property,
};

zst_element_t*
zst_mp4_muxer_create(void)
{
    zst_element_t* el;
    mp4_muxer_t* priv;
    zst_pad_t* video;
    zst_pad_t* audio;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    priv->width = 640;
    priv->height = 480;
    priv->fps = 30;
    priv->sample_rate = 44100;
    priv->channels = 2;

    el = zst_element_create(&g_ops, priv);

    video = zst_pad_create("video", ZST_PAD_SINK);
    audio = zst_pad_create("audio", ZST_PAD_SINK);
    src   = zst_pad_create("src",   ZST_PAD_SRC);

    video->push = mp4_mux_video_push;
    audio->push = mp4_mux_audio_push;

    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);
    zst_element_add_pad(el, src);

    return el;
}

zst_element_t*
zst_mp4_muxer_create_with_config(const zst_mp4_muxer_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_mp4_muxer_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("mp4mux");
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
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "mp4mux") == 0) {
        return zst_mp4_muxer_create();
    }
    return NULL;
}

static const zst_property_spec_t g_mp4mux_properties[] = {
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "framerate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Alias for fps" },
    { "sample-rate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Audio sample rate" },
    { "rate", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "44100", "Alias for sample-rate" },
    { "channels", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "2", "Audio channels count" },
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Output file path" },
    { "path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for location" }
};

static const zst_pad_template_t g_mp4mux_pads[] = {
    { "video", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "audio", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/x-aac" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/quicktime" }
};

static const zst_element_desc_t g_mp4mux_elements[] = {
    {
        .name = "mp4mux",
        .long_name = "MP4 Muxer",
        .category = "Muxer/File",
        .description = "Muxes encoded audio/video into MP4",
        .author = "zstreamer",
        .properties = g_mp4mux_properties,
        .nb_properties = sizeof(g_mp4mux_properties) / sizeof(g_mp4mux_properties[0]),
        .pads = g_mp4mux_pads,
        .nb_pads = sizeof(g_mp4mux_pads) / sizeof(g_mp4mux_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "mp4muxer_plugin",
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
        *nb_elements_out = sizeof(g_mp4mux_elements) / sizeof(g_mp4mux_elements[0]);
    }
    return g_mp4mux_elements;
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