/*=============================================================================
    rtmp_sink.c — RTMP sink element using FFmpeg/libavformat
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_rtmp_sink.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_bus.h"

#ifndef AV_INPUT_BUFFER_PADDING_SIZE
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#endif

typedef struct {
    AVFormatContext* fc;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              video_eos;
    int              audio_eos;
    int              header_written;
    char             url[512];

    int              video_linked;
    int              audio_linked;

    /* Configurable properties */
    int              live;
    int              reconnect;
    int              reconnect_delay_ms;
    int              max_reconnect_attempts;

    /* Video configuration metadata (extradata) */
    uint8_t*         video_extradata;
    int              video_extradata_size;
    int              video_annexb;

    /* Reconnection/Thread-safety state */
    pthread_mutex_t  lock;
    int              lock_initialized;
    int              running;
} rtmp_sink_t;

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
rtmp_find_start_code(const uint8_t* data, int size, int offset, int* code_size)
{
    for (int i = offset; i + 3 <= size; i++) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *code_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *code_size = 3;
            return i;
        }
    }
    return -1;
}

static int
rtmp_copy_nal(uint8_t** dst, int* dst_size, const uint8_t* nal, int nal_size)
{
    uint8_t* p = av_mallocz(nal_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!p) return 0;
    memcpy(p, nal, nal_size);
    *dst = p;
    *dst_size = nal_size;
    return 1;
}

static int
rtmp_parse_h264_extradata(rtmp_sink_t* s, const uint8_t* data, int size)
{
    if (!s || !data || size <= 0 || s->video_extradata) return s && s->video_extradata;

    uint8_t* sps = NULL;
    uint8_t* pps = NULL;
    int sps_size = 0;
    int pps_size = 0;

    int code_size = 0;
    int sc = rtmp_find_start_code(data, size, 0, &code_size);
    if (sc >= 0) {
        s->video_annexb = 1;
        int pos = sc;
        while (pos >= 0 && pos < size) {
            int nal_start = pos + code_size;
            int next_code_size = 0;
            int next = rtmp_find_start_code(data, size, nal_start, &next_code_size);
            int nal_end = next >= 0 ? next : size;
            while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
            int nal_size = nal_end - nal_start;
            if (nal_size > 0) {
                int nal_type = data[nal_start] & 0x1f;
                if (nal_type == 7 && !sps) {
                    rtmp_copy_nal(&sps, &sps_size, data + nal_start, nal_size);
                } else if (nal_type == 8 && !pps) {
                    rtmp_copy_nal(&pps, &pps_size, data + nal_start, nal_size);
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
                rtmp_copy_nal(&sps, &sps_size, data + pos, nal_size);
            } else if (nal_type == 8 && !pps) {
                rtmp_copy_nal(&pps, &pps_size, data + pos, nal_size);
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

static int
rtmp_parse_h265_extradata(rtmp_sink_t* s, const uint8_t* data, int size)
{
    if (!s || !data || size <= 0 || s->video_extradata) return s && s->video_extradata;

    uint8_t* vps = NULL;
    uint8_t* sps = NULL;
    uint8_t* pps = NULL;
    int vps_size = 0;
    int sps_size = 0;
    int pps_size = 0;

    int code_size = 0;
    int sc = rtmp_find_start_code(data, size, 0, &code_size);
    if (sc >= 0) {
        s->video_annexb = 1;
        int pos = sc;
        while (pos >= 0 && pos < size) {
            int nal_start = pos + code_size;
            int next_code_size = 0;
            int next = rtmp_find_start_code(data, size, nal_start, &next_code_size);
            int nal_end = next >= 0 ? next : size;
            while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
            int nal_size = nal_end - nal_start;
            if (nal_size > 1) {
                int nal_type = (data[nal_start] >> 1) & 0x3f;
                if (nal_type == 32 && !vps) {
                    rtmp_copy_nal(&vps, &vps_size, data + nal_start, nal_size);
                } else if (nal_type == 33 && !sps) {
                    rtmp_copy_nal(&sps, &sps_size, data + nal_start, nal_size);
                } else if (nal_type == 34 && !pps) {
                    rtmp_copy_nal(&pps, &pps_size, data + nal_start, nal_size);
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
            int nal_type = (data[pos] >> 1) & 0x3f;
            if (nal_type == 32 && !vps) {
                rtmp_copy_nal(&vps, &vps_size, data + pos, nal_size);
            } else if (nal_type == 33 && !sps) {
                rtmp_copy_nal(&sps, &sps_size, data + pos, nal_size);
            } else if (nal_type == 34 && !pps) {
                rtmp_copy_nal(&pps, &pps_size, data + pos, nal_size);
            }
            pos += nal_size;
        }
    }

    if (!vps || !sps || !pps || vps_size < 2 || sps_size < 2 || pps_size < 2) {
        av_free(vps);
        av_free(sps);
        av_free(pps);
        return 0;
    }

    int extra_size = 23 + (3 * 5) + vps_size + sps_size + pps_size;
    uint8_t* extra = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extra) {
        av_free(vps);
        av_free(sps);
        av_free(pps);
        return 0;
    }

    int p = 0;
    extra[p++] = 1;      // configurationVersion

    if (sps_size >= 15) {
        extra[p++] = sps[3]; // general_profile_space, general_tier_flag, general_profile_idc
        extra[p++] = sps[4]; // general_profile_compatibility_flags (byte 1)
        extra[p++] = sps[5]; // general_profile_compatibility_flags (byte 2)
        extra[p++] = sps[6]; // general_profile_compatibility_flags (byte 3)
        extra[p++] = sps[7]; // general_profile_compatibility_flags (byte 4)
        extra[p++] = sps[8]; // general_constraint_indicator_flags (byte 1)
        extra[p++] = sps[9]; // general_constraint_indicator_flags (byte 2)
        extra[p++] = sps[10]; // general_constraint_indicator_flags (byte 3)
        extra[p++] = sps[11]; // general_constraint_indicator_flags (byte 4)
        extra[p++] = sps[12]; // general_constraint_indicator_flags (byte 5)
        extra[p++] = sps[13]; // general_constraint_indicator_flags (byte 6)
        extra[p++] = sps[14]; // general_level_idc
    } else {
        extra[p++] = 0x01;
        extra[p++] = 0x60; extra[p++] = 0x00; extra[p++] = 0x00; extra[p++] = 0x00;
        extra[p++] = 0x90; extra[p++] = 0x00; extra[p++] = 0x00; extra[p++] = 0x00; extra[p++] = 0x00; extra[p++] = 0x00;
        extra[p++] = 93;
    }

    extra[p++] = 0xf0;
    extra[p++] = 0x00;
    extra[p++] = 0xfc;
    extra[p++] = 0xfd;
    extra[p++] = 0xf8;
    extra[p++] = 0xf8;
    extra[p++] = 0x00;
    extra[p++] = 0x00;
    extra[p++] = 0x0f;
    extra[p++] = 3;

    // Array 1: VPS
    extra[p++] = 0x80 | 32;
    extra[p++] = 0x00; extra[p++] = 0x01;
    extra[p++] = (uint8_t)(vps_size >> 8);
    extra[p++] = (uint8_t)(vps_size & 0xff);
    memcpy(extra + p, vps, vps_size);
    p += vps_size;

    // Array 2: SPS
    extra[p++] = 0x80 | 33;
    extra[p++] = 0x00; extra[p++] = 0x01;
    extra[p++] = (uint8_t)(sps_size >> 8);
    extra[p++] = (uint8_t)(sps_size & 0xff);
    memcpy(extra + p, sps, sps_size);
    p += sps_size;

    // Array 3: PPS
    extra[p++] = 0x80 | 34;
    extra[p++] = 0x00; extra[p++] = 0x01;
    extra[p++] = (uint8_t)(pps_size >> 8);
    extra[p++] = (uint8_t)(pps_size & 0xff);
    memcpy(extra + p, pps, pps_size);
    p += pps_size;

    av_free(vps);
    av_free(sps);
    av_free(pps);

    s->video_extradata = extra;
    s->video_extradata_size = p;
    return 1;
}

static uint8_t*
rtmp_annexb_to_avcc(const uint8_t* data, int size, int* out_size)
{
    *out_size = 0;
    int code_size = 0;
    int pos = rtmp_find_start_code(data, size, 0, &code_size);
    if (pos < 0) return NULL;

    uint8_t* out = malloc((size_t)size + 4);
    if (!out) return NULL;
    int out_pos = 0;

    while (pos >= 0 && pos < size) {
        int nal_start = pos + code_size;
        int next_code_size = 0;
        int next = rtmp_find_start_code(data, size, nal_start, &next_code_size);
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
rtmp_sink_connect_and_write_header(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;

    if (s->fc) {
        if (s->fc->pb) {
            avio_closep(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    s->header_written = 0;

    if (avformat_alloc_output_context2(&s->fc, NULL, "flv", s->url) < 0 || !s->fc) {
        ZST_LOG_ERROR("rtmpsink", "Failed to create FLV output context");
        return ZST_ERROR;
    }

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "rtmp_live", s->live ? "live" : "recorded", 0);

    /* Credentials are automatically handled by FFmpeg via native url parsing:
       rtmp://username:password@host:port/app/stream */
    if (avio_open2(&s->fc->pb, s->url, AVIO_FLAG_WRITE, NULL, &opts) < 0) {
        ZST_LOG_ERROR("rtmpsink", "Failed to open RTMP URL: %s", s->url);
        av_dict_free(&opts);
        avformat_free_context(s->fc);
        s->fc = NULL;
        return ZST_ERROR;
    }
    av_dict_free(&opts);

    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");

    int stream_count = 0;

    if (s->video_linked && video_pad) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) goto fail;
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264; // default

        if (video_pad->caps && video_pad->caps->structs) {
            const zst_caps_struct_t* caps = video_pad->caps->structs;
            if (strcmp(caps->media_type, "video/x-h265") == 0) {
                st->codecpar->codec_id = AV_CODEC_ID_HEVC;
            }
            if (caps->video.width > 0) {
                st->codecpar->width = caps->video.width;
            }
            if (caps->video.height > 0) {
                st->codecpar->height = caps->video.height;
            }
        }

        if (s->video_extradata && s->video_extradata_size > 0) {
            st->codecpar->extradata = av_mallocz(s->video_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (st->codecpar->extradata) {
                memcpy(st->codecpar->extradata, s->video_extradata, s->video_extradata_size);
                st->codecpar->extradata_size = s->video_extradata_size;
            }
        }

        st->time_base = (AVRational){1, 1000};
        s->video_stream_idx = stream_count++;
    }

    if (s->audio_linked && audio_pad) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        if (!st) goto fail;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_AAC; // default

        int sample_rate = 44100;
        int channels = 2;

        if (audio_pad->caps && audio_pad->caps->structs) {
            const zst_caps_struct_t* caps = audio_pad->caps->structs;
            if (caps->audio.sample_rate > 0) {
                sample_rate = caps->audio.sample_rate;
            }
            if (caps->audio.channels > 0) {
                channels = caps->audio.channels;
            }
        }

        st->codecpar->sample_rate = sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        av_channel_layout_default(&st->codecpar->ch_layout, channels);
#else
        st->codecpar->channels = channels;
        st->codecpar->channel_layout = channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif

        /* Construct AAC AudioSpecificConfig extradata (2 bytes) */
        st->codecpar->extradata_size = 2;
        st->codecpar->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (st->codecpar->extradata) {
            int freq_idx = mp4_aac_freq_index(sample_rate);
            int object_type = 2; /* AAC LC */
            st->codecpar->extradata[0] = (uint8_t)((object_type << 3) | (freq_idx >> 1));
            st->codecpar->extradata[1] = (uint8_t)(((freq_idx & 1) << 7) | (channels << 3));
        }

        st->time_base = (AVRational){1, 1000};
        s->audio_stream_idx = stream_count++;
    }

    if (avformat_write_header(s->fc, NULL) < 0) {
        ZST_LOG_ERROR("rtmpsink", "Failed to write header to RTMP stream");
        goto fail;
    }

    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    ZST_LOG_INFO("rtmpsink", "Started RTMP stream to %s", s->url);
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
rtmp_sink_ensure_connected(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (s->header_written) return ZST_OK;

    int attempts = 0;
    int delay_ms = s->reconnect_delay_ms > 0 ? s->reconnect_delay_ms : 500;

    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        zst_result_t res = rtmp_sink_connect_and_write_header(el);
        if (res == ZST_OK) {
            return ZST_OK;
        }

        if (!s->reconnect) {
            return ZST_ERROR;
        }

        if (s->max_reconnect_attempts >= 0 && attempts >= s->max_reconnect_attempts) {
            ZST_LOG_ERROR("rtmpsink", "Maximum reconnect attempts (%d) reached", s->max_reconnect_attempts);
            return ZST_ERROR;
        }

        attempts++;
        ZST_LOG_INFO("rtmpsink", "Reconnect attempt %d failed. Retrying in %d ms...", attempts, delay_ms);

        /* Sleep in small intervals to keep shutdown responsive */
        int slept = 0;
        while (slept < delay_ms && __atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
            int step = (delay_ms - slept > 100) ? 100 : (delay_ms - slept);
            struct timespec ts;
            ts.tv_sec = step / 1000;
            ts.tv_nsec = (long)(step % 1000) * 1000000L;
            nanosleep(&ts, NULL);
            slept += step;
        }

        /* Exponential backoff: double the delay up to 30 seconds maximum */
        delay_ms *= 2;
        if (delay_ms > 30000) {
            delay_ms = 30000;
        }
    }

    return ZST_ERROR;
}

static zst_result_t
rtmp_sink_write_locked(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    rtmp_sink_t* s = el->priv;

    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        if (rtmp_sink_ensure_connected(el) != ZST_OK) {
            return ZST_ERROR;
        }

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return ZST_ERROR;

        uint8_t* converted = NULL;
        uint8_t* packet_data = buf->memory.data;
        int packet_size = (int)buf->memory.size;
        int converted_size = 0;

        zst_pad_t* video_pad = zst_element_get_pad(el, "video");
        int is_video = (video_pad && stream_idx == s->video_stream_idx);
        if (is_video && s->video_annexb) {
            converted = rtmp_annexb_to_avcc(buf->memory.data, (int)buf->memory.size, &converted_size);
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

        int ret = av_interleaved_write_frame(s->fc, pkt);
        free(converted);
        av_packet_free(&pkt);

        if (ret >= 0) {
            return ZST_OK; /* Successful write */
        }

        ZST_LOG_ERROR("rtmpsink", "Failed to write frame to RTMP stream (error %d)", ret);

        s->header_written = 0;
        if (s->fc) {
            if (s->fc->pb) {
                avio_closep(&s->fc->pb);
            }
            avformat_free_context(s->fc);
            s->fc = NULL;
        }

        if (!s->reconnect) {
            return ZST_ERROR;
        }
    }

    return ZST_ERROR;
}

static void
rtmp_sink_check_eos(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    int all_eos = 1;
    if (s->video_linked && !s->video_eos) all_eos = 0;
    if (s->audio_linked && !s->audio_eos) all_eos = 0;

    if (all_eos) {
        if (s->fc && s->header_written) {
            av_write_trailer(s->fc);
            s->header_written = 0;
        }
        if (el->bus) {
            zst_bus_post(el->bus, zst_event_new_eos(el));
        }
    }
}

static zst_result_t
rtmp_sink_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtmp_sink_t* s = el->priv;

    pthread_mutex_lock(&s->lock);

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        rtmp_sink_check_eos(el);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (!s->header_written && !s->video_extradata) {
        zst_pad_t* video_pad = zst_element_get_pad(el, "video");
        int is_h265 = 0;
        if (video_pad && video_pad->caps && video_pad->caps->structs) {
            if (strcmp(video_pad->caps->structs->media_type, "video/x-h265") == 0) {
                is_h265 = 1;
            }
        }
        if (is_h265) {
            rtmp_parse_h265_extradata(s, buf->memory.data, (int)buf->memory.size);
        } else {
            rtmp_parse_h264_extradata(s, buf->memory.data, (int)buf->memory.size);
        }
    }

    if (rtmp_sink_ensure_connected(el) != ZST_OK) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    zst_result_t res = ZST_OK;
    if (s->video_stream_idx >= 0) {
        res = rtmp_sink_write_locked(el, buf, s->video_stream_idx);
    }

    pthread_mutex_unlock(&s->lock);
    return res;
}

static zst_result_t
rtmp_sink_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    rtmp_sink_t* s = el->priv;

    pthread_mutex_lock(&s->lock);

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        rtmp_sink_check_eos(el);
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    if (!s->header_written) {
        if (s->video_linked && !s->video_extradata) {
            /* Wait for video packet to get video extradata first */
            pthread_mutex_unlock(&s->lock);
            return ZST_OK;
        }
    }

    if (rtmp_sink_ensure_connected(el) != ZST_OK) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    zst_result_t res = ZST_OK;
    if (s->audio_stream_idx >= 0) {
        res = rtmp_sink_write_locked(el, buf, s->audio_stream_idx);
    }

    pthread_mutex_unlock(&s->lock);
    return res;
}

static zst_result_t
rtmp_sink_open(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;
    pthread_mutex_init(&s->lock, NULL);
    s->lock_initialized = 1;
    return ZST_OK;
}

static zst_result_t
rtmp_sink_close(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (s->lock_initialized) {
        pthread_mutex_destroy(&s->lock);
        s->lock_initialized = 0;
    }
    return ZST_OK;
}

static zst_result_t
rtmp_sink_start(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;
    if (s->url[0] == '\0') {
        ZST_LOG_ERROR("rtmpsink", "RTMP URL not set");
        return ZST_ERROR;
    }

    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");
    s->video_linked = (video_pad && video_pad->peer) ? 1 : 0;
    s->audio_linked = (audio_pad && audio_pad->peer) ? 1 : 0;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->header_written = 0;
    s->video_eos = 0;
    s->audio_eos = 0;
    __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
    return ZST_OK;
}

static zst_result_t
rtmp_sink_stop(zst_element_t* el)
{
    rtmp_sink_t* s = el->priv;
    if (!s) return ZST_ERROR;

    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s->lock);

    if (s->fc && s->header_written) {
        av_write_trailer(s->fc);
    }

    if (s->fc) {
        if (s->fc->pb) {
            avio_closep(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    s->header_written = 0;

    if (s->video_extradata) {
        av_free(s->video_extradata);
        s->video_extradata = NULL;
        s->video_extradata_size = 0;
    }

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
rtmp_sink_set_property(zst_element_t* el, const char* name, const char* value)
{
    if (!el || !el->priv || !name || !value) return ZST_ERROR;
    rtmp_sink_t* s = el->priv;
    if (strcmp(name, "url") == 0 || strcmp(name, "rtmp_url") == 0 || strcmp(name, "rtmp-url") == 0) {
        snprintf(s->url, sizeof(s->url), "%s", value);
        return ZST_OK;
    }
    if (strcmp(name, "live") == 0) {
        s->live = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "reconnect") == 0) {
        s->reconnect = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        s->reconnect_delay_ms = atoi(value);
        if (s->reconnect_delay_ms < 0) s->reconnect_delay_ms = 0;
        return ZST_OK;
    }
    if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        s->max_reconnect_attempts = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
rtmp_sink_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    if (!el || !el->priv || !name || !value_out || max_len == 0) return ZST_ERROR;
    rtmp_sink_t* s = el->priv;
    if (strcmp(name, "url") == 0 || strcmp(name, "rtmp_url") == 0 || strcmp(name, "rtmp-url") == 0) {
        snprintf(value_out, max_len, "%s", s->url);
        return ZST_OK;
    }
    if (strcmp(name, "live") == 0) {
        snprintf(value_out, max_len, "%s", s->live ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "reconnect") == 0) {
        snprintf(value_out, max_len, "%s", s->reconnect ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "reconnect-delay-ms") == 0 || strcmp(name, "reconnect_delay_ms") == 0) {
        snprintf(value_out, max_len, "%d", s->reconnect_delay_ms);
        return ZST_OK;
    }
    if (strcmp(name, "max-reconnect-attempts") == 0 || strcmp(name, "max_reconnect_attempts") == 0) {
        snprintf(value_out, max_len, "%d", s->max_reconnect_attempts);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_caps_t*
rtmp_sink_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
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

static zst_element_ops_t g_ops = {
    .name  = "rtmpsink",
    .open  = rtmp_sink_open,
    .close = rtmp_sink_close,
    .start = rtmp_sink_start,
    .stop  = rtmp_sink_stop,
    .set_property = rtmp_sink_set_property,
    .get_property = rtmp_sink_get_property,
    .get_caps = rtmp_sink_get_caps,
};

zst_element_t*
zst_rtmp_sink_create(void)
{
    rtmp_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->live = 1;
    priv->reconnect = 0;
    priv->reconnect_delay_ms = 500;
    priv->max_reconnect_attempts = -1;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* video = zst_pad_create("video", ZST_PAD_SINK);
    zst_pad_t* audio = zst_pad_create("audio", ZST_PAD_SINK);

    if (!video || !audio) {
        if (video) zst_pad_destroy(video);
        if (audio) zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    video->push = rtmp_sink_video_push;
    audio->push = rtmp_sink_audio_push;

    if (zst_element_add_pad(el, video) != ZST_OK) {
        zst_pad_destroy(video);
        zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    if (zst_element_add_pad(el, audio) != ZST_OK) {
        zst_pad_destroy(audio);
        zst_element_destroy(el);
        return NULL;
    }

    return el;
}



zst_element_t*
zst_rtmp_sink_create_with_config(const zst_rtmp_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_rtmp_sink_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("rtmpsink");
    if (!el) return NULL;

    if (config->url) {
        zst_element_set_property_string(el, "url", config->url);
    }

    return el;
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "rtmpsink") == 0) {
        return zst_rtmp_sink_create();
    }
    return NULL;
}

static const zst_pad_template_t g_rtmpsink_pads[] = {
    { "video", ZST_PAD_SINK, "video/x-h264" },
    { "audio", ZST_PAD_SINK, "audio/x-aac" }
};

static const zst_property_spec_t g_rtmpsink_properties[] = {
    { "url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "RTMP Destination URL" },
    { "rtmp_url", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for url" },
    { "live", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Use live RTMP mode" },
    { "reconnect", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Reconnect on publish failure" },
    { "reconnect-delay-ms", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "500", "Delay between reconnect attempts" },
    { "max-reconnect-attempts", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum reconnect attempts; -1 means unlimited" }
};

static const zst_element_desc_t g_rtmpsink_elements[] = {
    {
        .name = "rtmpsink",
        .long_name = "RTMP Sink",
        .category = "Sink/Network",
        .description = "Publishes audio/video to an RTMP endpoint",
        .author = "zstreamer",
        .properties = g_rtmpsink_properties,
        .nb_properties = sizeof(g_rtmpsink_properties) / sizeof(g_rtmpsink_properties[0]),
        .pads = g_rtmpsink_pads,
        .nb_pads = sizeof(g_rtmpsink_pads) / sizeof(g_rtmpsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "rtmpsink_plugin",
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
        *nb_elements_out = sizeof(g_rtmpsink_elements) / sizeof(g_rtmpsink_elements[0]);
    }
    return g_rtmpsink_elements;
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
