/*=============================================================================
    hls_sink.c — FFmpeg libavformat HLS sink implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_bus.h"
#include "zst_media_utils.h"
#include "zstreamer/elements/zst_hls_sink.h"
#include <pthread.h>

typedef struct {
    pthread_mutex_t  lock;
    AVFormatContext* fc;
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

    char             location[256];
    char             format[32];
    int              target_duration;
    int              playlist_length;
    char             video_codec_name[32];
    char             audio_codec_name[32];
} hls_sink_t;

static int
hls_aac_freq_index(int sample_rate)
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
hls_copy_nal(uint8_t** dst, int* dst_size, const uint8_t* nal, int nal_size)
{
    uint8_t* p = av_mallocz(nal_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!p) return 0;
    memcpy(p, nal, nal_size);
    *dst = p;
    *dst_size = nal_size;
    return 1;
}

static int
hls_parse_h264_extradata(hls_sink_t* s, const uint8_t* data, int size)
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
                    hls_copy_nal(&sps, &sps_size, data + nal_start, nal_size);
                } else if (nal_type == 8 && !pps) {
                    hls_copy_nal(&pps, &pps_size, data + nal_start, nal_size);
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
                hls_copy_nal(&sps, &sps_size, data + pos, nal_size);
            } else if (nal_type == 8 && !pps) {
                hls_copy_nal(&pps, &pps_size, data + pos, nal_size);
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
hls_annexb_to_avcc(const uint8_t* data, int size, int* out_size)
{
    *out_size = 0;
    int code_size = 0;
    int pos = zst_find_start_code(data, size, 0, &code_size);
    if (pos < 0) return NULL;

    /* Worst case: every 3 bytes is a start code replaced by 4 bytes -> size grows by 1/3 */
    uint8_t* out = malloc((size_t)size + (size_t)size / 2 + 4);
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

static zst_result_t
hls_write_header(zst_element_t* el)
{
    hls_sink_t* s = el->priv;
    
    if (s->location[0] == '\0') {
        return ZST_ERROR;
    }

    if (avformat_alloc_output_context2(&s->fc, NULL, "hls", s->location) < 0) {
        return ZST_ERROR;
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
        if (strcmp(s->video_codec_name, "h265") == 0 || strcmp(s->video_codec_name, "hevc") == 0) {
            st->codecpar->codec_id = AV_CODEC_ID_HEVC;
        }
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
        if (strcmp(s->audio_codec_name, "opus") == 0) {
            st->codecpar->codec_id = AV_CODEC_ID_OPUS;
        } else if (strcmp(s->audio_codec_name, "pcm") == 0) {
            st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        }
        st->codecpar->sample_rate = s->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        av_channel_layout_default(&st->codecpar->ch_layout, s->channels);
#else
        st->codecpar->channels = s->channels;
        st->codecpar->channel_layout = s->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
        if (st->codecpar->codec_id == AV_CODEC_ID_AAC) {
            st->codecpar->extradata_size = 2;
            st->codecpar->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!st->codecpar->extradata) return ZST_ERROR;
            int freq_idx = hls_aac_freq_index(s->sample_rate);
            int object_type = 2; /* AAC LC */
            st->codecpar->extradata[0] = (uint8_t)((object_type << 3) | (freq_idx >> 1));
            st->codecpar->extradata[1] = (uint8_t)(((freq_idx & 1) << 7) | (s->channels << 3));
        } else if (st->codecpar->codec_id == AV_CODEC_ID_OPUS) {
            st->codecpar->extradata_size = 19;
            st->codecpar->extradata = av_mallocz(19 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (st->codecpar->extradata) {
                uint8_t* ed = st->codecpar->extradata;
                memcpy(ed, "OpusHead", 8);
                ed[8] = 1;
                ed[9] = s->channels;
                ed[10] = 0;
                ed[11] = 0;
                ed[12] = (uint8_t)(s->sample_rate);
                ed[13] = (uint8_t)(s->sample_rate >> 8);
                ed[14] = (uint8_t)(s->sample_rate >> 16);
                ed[15] = (uint8_t)(s->sample_rate >> 24);
                ed[16] = 0;
                ed[17] = 0;
                ed[18] = 0;
            }
        }
        st->time_base = (AVRational){1, 1000000000};
        s->audio_stream_idx = st->index;
    }
    
    AVDictionary* opts = NULL;
    char target_dur[16];
    snprintf(target_dur, sizeof(target_dur), "%d", s->target_duration);
    av_dict_set(&opts, "hls_time", target_dur, 0);

    char list_size[16];
    snprintf(list_size, sizeof(list_size), "%d", s->playlist_length);
    av_dict_set(&opts, "hls_list_size", list_size, 0);
    
    if (strcmp(s->format, "fmp4") == 0) {
        av_dict_set(&opts, "hls_segment_type", "fmp4", 0);
    } else {
        av_dict_set(&opts, "hls_segment_type", "mpegts", 0);
    }
    av_dict_set(&opts, "hls_flags", "independent_segments", 0);

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
hls_write(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    hls_sink_t* s = el->priv;

    if (!s->header_written) return ZST_ERROR;
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    uint8_t* converted = NULL;
    uint8_t* packet_data = buf->memory.data;
    int packet_size = (int)buf->memory.size;
    int converted_size = 0;
    if (stream_idx == s->video_stream_idx && s->video_annexb) {
        if (strcmp(s->format, "fmp4") == 0) {
            converted = hls_annexb_to_avcc(buf->memory.data, (int)buf->memory.size, &converted_size);
            if (converted && converted_size > 0) {
                packet_data = converted;
                packet_size = converted_size;
            }
        }
    }

    if (av_new_packet(pkt, packet_size) < 0) {
        free(converted);
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    memcpy(pkt->data, packet_data, packet_size);

    if (buf->pts != (zst_time_t)-1) {
        pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    } else {
        pkt->pts = AV_NOPTS_VALUE;
    }
    
    if (buf->dts != (zst_time_t)-1) {
        pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    } else {
        pkt->dts = AV_NOPTS_VALUE;
    }
    
    if (buf->duration > 0) {
        pkt->duration = av_rescale_q(buf->duration, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    } else {
        pkt->duration = 0;
    }
    pkt->stream_index = stream_idx;

    if (buf->flags & ZST_BUFFER_FLAG_KEYFRAME) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

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
hls_check_eos(zst_element_t* el)
{
    hls_sink_t* s = el->priv;
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
hls_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    hls_sink_t* s = el->priv;
    zst_result_t ret = ZST_OK;
    
    pthread_mutex_lock(&s->lock);
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        hls_check_eos(el);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        if (!hls_parse_h264_extradata(s, buf->memory.data, (int)buf->memory.size)) {
            pthread_mutex_unlock(&s->lock);
            return ZST_ERROR;
        }
        if (hls_write_header(el) != ZST_OK) {
            pthread_mutex_unlock(&s->lock);
            return ZST_ERROR;
        }
    }

    if (s->video_stream_idx >= 0) {
        ret = hls_write(el, buf, s->video_stream_idx);
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_result_t
hls_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    hls_sink_t* s = el->priv;
    zst_result_t ret = ZST_OK;
    
    pthread_mutex_lock(&s->lock);
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        hls_check_eos(el);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }
    
    if (!s->header_written) {
        if (s->video_linked && !s->video_extradata) {
            pthread_mutex_unlock(&s->lock);
            return ZST_OK;
        }
        if (hls_write_header(el) != ZST_OK) {
            pthread_mutex_unlock(&s->lock);
            return ZST_ERROR;
        }
    }

    if (s->audio_stream_idx >= 0) {
        ret = hls_write(el, buf, s->audio_stream_idx);
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_result_t
hls_start(zst_element_t* el)
{
    hls_sink_t* s = el->priv;
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
hls_stop(zst_element_t* el)
{
    hls_sink_t* s = el->priv;
    if (s->fc && s->header_written) {
        av_write_trailer(s->fc);
    }
    
    if (s->fc) {
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
hls_set_property(zst_element_t* el, const char* name, const char* value)
{
    hls_sink_t* s = el->priv;
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
        return ZST_OK;
    } else if (strcmp(name, "video-codec") == 0) {
        snprintf(s->video_codec_name, sizeof(s->video_codec_name), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "audio-codec") == 0) {
        snprintf(s->audio_codec_name, sizeof(s->audio_codec_name), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "target-duration") == 0) {
        s->target_duration = v;
        return ZST_OK;
    } else if (strcmp(name, "playlist-length") == 0) {
        s->playlist_length = v;
        return ZST_OK;
    } else if (strcmp(name, "format") == 0) {
        snprintf(s->format, sizeof(s->format), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
hls_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    hls_sink_t* s = el->priv;
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
    } else if (strcmp(name, "video-codec") == 0) {
        snprintf(value_out, max_len, "%s", s->video_codec_name);
        return ZST_OK;
    } else if (strcmp(name, "audio-codec") == 0) {
        snprintf(value_out, max_len, "%s", s->audio_codec_name);
        return ZST_OK;
    } else if (strcmp(name, "target-duration") == 0) {
        snprintf(value_out, max_len, "%d", s->target_duration);
        return ZST_OK;
    } else if (strcmp(name, "playlist-length") == 0) {
        snprintf(value_out, max_len, "%d", s->playlist_length);
        return ZST_OK;
    } else if (strcmp(name, "format") == 0) {
        snprintf(value_out, max_len, "%s", s->format);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name  = "hls_sink",
    .start = hls_start,
    .stop  = hls_stop,
    .set_property = hls_set_property,
    .get_property = hls_get_property,
};

zst_element_t*
zst_hls_sink_create(void)
{
    hls_sink_t* priv = calloc(1, sizeof(hls_sink_t));
    if (!priv) return NULL;
    
    pthread_mutex_init(&priv->lock, NULL);
    priv->target_duration = 5;
    priv->playlist_length = 5;
    strcpy(priv->format, "fmp4");

    zst_element_t* el = zst_element_create(&g_ops, priv);
    
    zst_pad_t* video_pad = zst_pad_create("video", ZST_PAD_SINK);
    video_pad->push = hls_video_push;
    zst_element_add_pad(el, video_pad);
    
    zst_pad_t* audio_pad = zst_pad_create("audio", ZST_PAD_SINK);
    audio_pad->push = hls_audio_push;
    zst_element_add_pad(el, audio_pad);

    return el;
}

zst_element_t*
zst_hls_sink_create_with_config(const zst_hls_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_hls_sink_config_t)) return NULL;
    zst_element_t* el = zst_hls_sink_create();
    if (!el) return NULL;

    if (config->width > 0) zst_element_set_property_uint(el, "width", config->width);
    if (config->height > 0) zst_element_set_property_uint(el, "height", config->height);
    if (config->fps > 0) zst_element_set_property_uint(el, "fps", config->fps);
    if (config->sample_rate > 0) zst_element_set_property_uint(el, "sample-rate", config->sample_rate);
    if (config->channels > 0) zst_element_set_property_uint(el, "channels", config->channels);
    if (config->location) zst_element_set_property_string(el, "location", config->location);
    if (config->video_codec) zst_element_set_property_string(el, "video-codec", config->video_codec);
    if (config->audio_codec) zst_element_set_property_string(el, "audio-codec", config->audio_codec);
    if (config->target_duration > 0) zst_element_set_property_int(el, "target-duration", config->target_duration);
    if (config->playlist_length > 0) zst_element_set_property_int(el, "playlist-length", config->playlist_length);
    if (config->format) zst_element_set_property_string(el, "format", config->format);

    return el;
}