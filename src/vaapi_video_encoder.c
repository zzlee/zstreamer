/*=============================================================================
    vaapi_video_encoder.c — Linux VA-API video encoder implementation

    Uses FFmpeg's VA-API hardware encoder wrappers (h264_vaapi/hevc_vaapi) and
    AVHWFramesContext upload from CPU I420 input to VAAPI NV12 surfaces.  The
    element is opt-in via ENABLE_VAAPI_ENCODER and skips gracefully when no DRM
    render node or VA-API encode-capable driver is available.
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_vaapi_video_encoder.h"

#ifndef AV_INPUT_BUFFER_PADDING_SIZE
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#endif

#define VAAPI_ENC_DEFAULT_DEVICE  "/dev/dri/renderD128"
#define VAAPI_ENC_DEFAULT_BITRATE 4000000
#define VAAPI_ENC_DEFAULT_GOP     30
#define VAAPI_ENC_DEFAULT_FPS     30

typedef struct vaapi_pending_packet {
    zst_buffer_t* buf;
    struct vaapi_pending_packet* next;
} vaapi_pending_packet_t;

typedef struct {
    AVCodecContext* codec_ctx;
    AVBufferRef*    hw_device_ctx;
    AVBufferRef*    hw_frames_ctx;
    int             initialized;
    int             draining;
    zst_buffer_pool_t* pool;
    size_t          packet_capacity;
    uint32_t        width;
    uint32_t        height;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            device[256];     /* DRM render node path */
    char            codec[32];       /* h264 or h265 */
    char            preset[32];      /* speed, balanced, quality */
    char            profile[32];
    char            level[16];
    char            rate_control[32];
    int64_t         bitrate;
    int             gop_size;
    int             fps_num;
    int             fps_den;
    zst_time_t      last_duration;
    int             force_keyframe;

    vaapi_pending_packet_t* pending_head;
    vaapi_pending_packet_t* pending_tail;
} vaapi_video_encoder_t;

static const char*
vaapi_encoder_name(const vaapi_video_encoder_t* s)
{
    return (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) ?
           "hevc_vaapi" : "h264_vaapi";
}

static const char*
vaapi_media_type(const vaapi_video_encoder_t* s)
{
    return (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) ?
           "video/x-h265" : "video/x-h264";
}

static int
vaapi_profile_id(const vaapi_video_encoder_t* s)
{
    int is_hevc = (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0);
    if (!s->profile[0]) return FF_PROFILE_UNKNOWN;

    if (is_hevc) {
        if (strcmp(s->profile, "main") == 0) return FF_PROFILE_HEVC_MAIN;
#ifdef FF_PROFILE_HEVC_MAIN_10
        if (strcmp(s->profile, "main10") == 0 || strcmp(s->profile, "main-10") == 0) return FF_PROFILE_HEVC_MAIN_10;
#endif
        return FF_PROFILE_UNKNOWN;
    }

    if (strcmp(s->profile, "baseline") == 0 || strcmp(s->profile, "constrained-baseline") == 0) return FF_PROFILE_H264_CONSTRAINED_BASELINE;
    if (strcmp(s->profile, "main") == 0) return FF_PROFILE_H264_MAIN;
    if (strcmp(s->profile, "high") == 0) return FF_PROFILE_H264_HIGH;
    return FF_PROFILE_UNKNOWN;
}

static int
vaapi_level_id(const vaapi_video_encoder_t* s)
{
    if (!s->level[0]) return FF_LEVEL_UNKNOWN;
    int major = 0;
    int minor = 0;
    if (sscanf(s->level, "%d.%d", &major, &minor) == 2 && major > 0) {
        return major * 10 + minor;
    }
    int numeric = atoi(s->level);
    return numeric > 0 ? numeric : FF_LEVEL_UNKNOWN;
}

static int
vaapi_quality_value(const vaapi_video_encoder_t* s)
{
    if (strcmp(s->preset, "speed") == 0 || strcmp(s->preset, "fast") == 0) return 7;
    if (strcmp(s->preset, "quality") == 0 || strcmp(s->preset, "slow") == 0) return 1;
    return 4;
}

static void
vaapi_pending_clear(vaapi_video_encoder_t* s)
{
    vaapi_pending_packet_t* p = s->pending_head;
    while (p) {
        vaapi_pending_packet_t* next = p->next;
        zst_buffer_unref(p->buf);
        free(p);
        p = next;
    }
    s->pending_head = NULL;
    s->pending_tail = NULL;
}

static zst_result_t
vaapi_pending_push(vaapi_video_encoder_t* s, zst_buffer_t* buf)
{
    vaapi_pending_packet_t* node = calloc(1, sizeof(*node));
    if (!node) return ZST_ERROR;
    node->buf = buf;
    if (s->pending_tail) {
        s->pending_tail->next = node;
    } else {
        s->pending_head = node;
    }
    s->pending_tail = node;
    return ZST_OK;
}

static zst_buffer_t*
vaapi_pending_pop(vaapi_video_encoder_t* s)
{
    vaapi_pending_packet_t* node = s->pending_head;
    if (!node) return NULL;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    zst_buffer_t* buf = node->buf;
    free(node);
    return buf;
}

static int
vaapi_is_config_property(const char* name)
{
    if (!name) return 0;
    return strcmp(name, "device") == 0 ||
           strcmp(name, "codec") == 0 ||
           strcmp(name, "preset") == 0 ||
           strcmp(name, "bitrate") == 0 ||
           strcmp(name, "gop-size") == 0 ||
           strcmp(name, "gop") == 0 ||
           strcmp(name, "keyint") == 0 ||
           strcmp(name, "keyframe-interval") == 0 ||
           strcmp(name, "profile") == 0 ||
           strcmp(name, "level") == 0 ||
           strcmp(name, "fps") == 0 ||
           strcmp(name, "rate-control") == 0;
}

static zst_result_t
vaapi_open(zst_element_t* el)
{
    vaapi_video_encoder_t* s = el->priv;
    s->initialized = 0;
    s->draining = 0;
    s->codec_ctx = NULL;
    s->hw_device_ctx = NULL;
    s->hw_frames_ctx = NULL;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->last_duration = 0;
    vaapi_pending_clear(s);

    const char* device = s->device[0] ? s->device : NULL;
    int ret = av_hwdevice_ctx_create(&s->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "VA-API device unavailable (%s): %s", device ? device : "default", err);
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t
vaapi_close(zst_element_t* el)
{
    vaapi_video_encoder_t* s = el->priv;
    vaapi_pending_clear(s);

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->codec_ctx) {
        avcodec_free_context(&s->codec_ctx);
        s->codec_ctx = NULL;
    }
    if (s->hw_frames_ctx) {
        av_buffer_unref(&s->hw_frames_ctx);
    }
    if (s->hw_device_ctx) {
        av_buffer_unref(&s->hw_device_ctx);
    }
    s->initialized = 0;
    s->draining = 0;
    return ZST_OK;
}

static zst_result_t
vaapi_setup_hw_frames(vaapi_video_encoder_t* s)
{
    s->hw_frames_ctx = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx) return ZST_ERROR;

    AVHWFramesContext* frames = (AVHWFramesContext*)s->hw_frames_ctx->data;
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = (int)s->width;
    frames->height = (int)s->height;
    frames->initial_pool_size = 8;

    int ret = av_hwframe_ctx_init(s->hw_frames_ctx);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "failed to initialize VA-API frame pool: %s", err);
        return ZST_ERROR;
    }

    s->codec_ctx->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx);
    return s->codec_ctx->hw_frames_ctx ? ZST_OK : ZST_ERROR;
}

static zst_result_t
vaapi_init_encoder(vaapi_video_encoder_t* s, uint32_t width, uint32_t height)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(vaapi_encoder_name(s));
    if (!codec) {
        ZST_LOG_WARN("vaapienc", "FFmpeg encoder %s is unavailable", vaapi_encoder_name(s));
        return ZST_ERROR;
    }

    s->codec_ctx = avcodec_alloc_context3(codec);
    if (!s->codec_ctx) return ZST_ERROR;

    s->width = width;
    s->height = height;
    s->codec_ctx->width = (int)width;
    s->codec_ctx->height = (int)height;
    s->codec_ctx->time_base = (AVRational){1, 1000000000};
    s->codec_ctx->framerate = (AVRational){s->fps_num > 0 ? s->fps_num : VAAPI_ENC_DEFAULT_FPS,
                                           s->fps_den > 0 ? s->fps_den : 1};
    s->codec_ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    s->codec_ctx->bit_rate = s->bitrate > 0 ? s->bitrate : VAAPI_ENC_DEFAULT_BITRATE;
    s->codec_ctx->gop_size = s->gop_size > 0 ? s->gop_size : VAAPI_ENC_DEFAULT_GOP;
    s->codec_ctx->max_b_frames = 0;
    s->codec_ctx->profile = vaapi_profile_id(s);
    s->codec_ctx->level = vaapi_level_id(s);

    if (vaapi_setup_hw_frames(s) != ZST_OK) {
        avcodec_free_context(&s->codec_ctx);
        return ZST_ERROR;
    }

    if (s->codec_ctx->priv_data) {
        if (s->profile[0]) (void)av_opt_set(s->codec_ctx->priv_data, "profile", s->profile, 0);
        if (s->rate_control[0]) (void)av_opt_set(s->codec_ctx->priv_data, "rc_mode", s->rate_control, 0);
        (void)av_opt_set_int(s->codec_ctx->priv_data, "quality", vaapi_quality_value(s), 0);
    }

    int ret = avcodec_open2(s->codec_ctx, codec, NULL);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "failed to open %s: %s", vaapi_encoder_name(s), err);
        avcodec_free_context(&s->codec_ctx);
        return ZST_ERROR;
    }

    size_t packet_size = (size_t)width * height * 4u + 1024u * 1024u + AV_INPUT_BUFFER_PADDING_SIZE;
    s->packet_capacity = packet_size;
    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = packet_size,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        avcodec_free_context(&s->codec_ctx);
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
vaapi_copy_i420_to_nv12(AVFrame* sw, const zst_video_frame_t* frame)
{
    if (!sw || !frame || !frame->plane[0] || !frame->plane[1] || !frame->plane[2]) return ZST_ERROR;

    for (uint32_t row = 0; row < frame->height; row++) {
        memcpy(sw->data[0] + (size_t)row * sw->linesize[0],
               frame->plane[0] + (size_t)row * frame->stride[0],
               frame->width);
    }

    for (uint32_t row = 0; row < frame->height / 2; row++) {
        uint8_t* uv = sw->data[1] + (size_t)row * sw->linesize[1];
        const uint8_t* u = frame->plane[1] + (size_t)row * frame->stride[1];
        const uint8_t* v = frame->plane[2] + (size_t)row * frame->stride[2];
        for (uint32_t col = 0; col < frame->width / 2; col++) {
            uv[col * 2u + 0u] = u[col];
            uv[col * 2u + 1u] = v[col];
        }
    }

    return ZST_OK;
}

static void free_drm_descriptor(void* opaque, uint8_t* data)
{
    (void)opaque;
    if (data) {
        AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)data;
        for (int i = 0; i < desc->nb_objects; i++) {
            if (desc->objects[i].fd >= 0) {
                close(desc->objects[i].fd);
            }
        }
        av_free(desc);
    }
}

static uint32_t
av_to_drm_format(uint32_t av_fmt)
{
    switch (av_fmt) {
        case AV_PIX_FMT_YUV420P: return DRM_FORMAT_YUV420;
        case AV_PIX_FMT_YUYV422: return DRM_FORMAT_YUYV;
        case AV_PIX_FMT_NV12:    return DRM_FORMAT_NV12;
        default:                 return 0;
    }
}

static zst_result_t
vaapi_make_hw_frame(vaapi_video_encoder_t* s, const zst_buffer_t* in, AVFrame** hw_out)
{
    zst_video_frame_t* frame = in ? (zst_video_frame_t*)in->payload : NULL;
    if (!frame) return ZST_ERROR;

    if (in->memory.type == ZST_MEMORY_DMABUF && in->memory.priv) {
        int dmabuf_fd = *(int*)in->memory.priv;
        if (dmabuf_fd >= 0) {
            AVFrame* drm_frame = av_frame_alloc();
            if (!drm_frame) return ZST_ERROR;

            drm_frame->format = AV_PIX_FMT_DRM_PRIME;
            drm_frame->width = frame->width;
            drm_frame->height = frame->height;
            drm_frame->pts = (int64_t)in->pts;

            AVDRMFrameDescriptor* desc = av_mallocz(sizeof(*desc));
            if (!desc) {
                av_frame_free(&drm_frame);
                return ZST_ERROR;
            }

            desc->nb_objects = 1;
            desc->objects[0].fd = dup(dmabuf_fd);
            if (desc->objects[0].fd < 0) {
                av_free(desc);
                av_frame_free(&drm_frame);
                return ZST_ERROR;
            }
            desc->objects[0].size = in->memory.size;
            desc->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;

            uint32_t drm_fmt = av_to_drm_format(frame->format);
            if (drm_fmt == 0) {
                if (frame->format == 0) drm_fmt = DRM_FORMAT_YUV420;
                else if (frame->format == 1) drm_fmt = DRM_FORMAT_YUYV;
                else drm_fmt = DRM_FORMAT_NV12;
            }

            desc->nb_layers = 1;
            desc->layers[0].format = drm_fmt;

            int nb_planes = 1;
            if (drm_fmt == DRM_FORMAT_NV12) {
                nb_planes = 2;
            } else if (drm_fmt == DRM_FORMAT_YUV420) {
                nb_planes = 3;
            }
            desc->layers[0].nb_planes = nb_planes;

            for (int i = 0; i < nb_planes; i++) {
                desc->layers[0].planes[i].object_index = 0;
                if (frame->plane[i] && frame->plane[0]) {
                    desc->layers[0].planes[i].offset = (ptrdiff_t)(frame->plane[i] - frame->plane[0]);
                } else {
                    desc->layers[0].planes[i].offset = 0;
                }
                desc->layers[0].planes[i].pitch = frame->stride[i];
            }

            drm_frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc), free_drm_descriptor, NULL, 0);
            if (!drm_frame->buf[0]) {
                close(desc->objects[0].fd);
                av_free(desc);
                av_frame_free(&drm_frame);
                return ZST_ERROR;
            }
            drm_frame->data[0] = (uint8_t*)desc;

            AVFrame* hw = av_frame_alloc();
            if (!hw) {
                av_frame_free(&drm_frame);
                return ZST_ERROR;
            }

            hw->hw_frames_ctx = av_buffer_ref(s->codec_ctx->hw_frames_ctx);
            if (!hw->hw_frames_ctx) {
                av_frame_free(&drm_frame);
                av_frame_free(&hw);
                return ZST_ERROR;
            }

            int ret = av_hwframe_map(hw, drm_frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE);
            av_frame_free(&drm_frame);
            if (ret < 0) {
                char err[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err, sizeof(err));
                ZST_LOG_WARN("vaapienc", "failed to map DRM_PRIME frame to VA-API: %s", err);
                av_frame_free(&hw);
                return ZST_ERROR;
            }
            hw->pts = (int64_t)in->pts;
            *hw_out = hw;
            return ZST_OK;
        }
    }

    AVFrame* sw = av_frame_alloc();
    AVFrame* hw = av_frame_alloc();
    if (!sw || !hw) {
        av_frame_free(&sw);
        av_frame_free(&hw);
        return ZST_ERROR;
    }

    sw->format = AV_PIX_FMT_NV12;
    sw->width = (int)frame->width;
    sw->height = (int)frame->height;
    sw->pts = (int64_t)in->pts;

    int ret = av_frame_get_buffer(sw, 32);
    if (ret < 0 || vaapi_copy_i420_to_nv12(sw, frame) != ZST_OK) {
        av_frame_free(&sw);
        av_frame_free(&hw);
        return ZST_ERROR;
    }

    ret = av_hwframe_get_buffer(s->codec_ctx->hw_frames_ctx, hw, 0);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "failed to allocate VA-API surface: %s", err);
        av_frame_free(&sw);
        av_frame_free(&hw);
        return ZST_ERROR;
    }
    hw->pts = sw->pts;

    ret = av_hwframe_transfer_data(hw, sw, 0);
    av_frame_free(&sw);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "failed to upload frame to VA-API surface: %s", err);
        av_frame_free(&hw);
        return ZST_ERROR;
    }

    *hw_out = hw;
    return ZST_OK;
}

static zst_result_t
vaapi_queue_packet(vaapi_video_encoder_t* s, const AVPacket* av_pkt, const zst_buffer_t* in)
{
    zst_buffer_t* pkt = NULL;
    if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
        ZST_LOG_WARN("vaapienc", "failed to acquire encoded packet buffer");
        return ZST_ERROR;
    }

    if ((size_t)av_pkt->size > s->packet_capacity) {
        ZST_LOG_WARN("vaapienc", "encoded packet too large (%d > %zu)", av_pkt->size, s->packet_capacity);
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }

    memcpy(pkt->memory.data, av_pkt->data, av_pkt->size);
    pkt->memory.size = (size_t)av_pkt->size;
    if (av_pkt->pts != AV_NOPTS_VALUE) pkt->pts = (zst_time_t)av_pkt->pts;
    else if (in) pkt->pts = in->pts;
    if (av_pkt->dts != AV_NOPTS_VALUE) pkt->dts = (zst_time_t)av_pkt->dts;
    else pkt->dts = pkt->pts;
    if (av_pkt->duration > 0) pkt->duration = (zst_time_t)av_pkt->duration;
    else if (in) pkt->duration = in->duration;
    else pkt->duration = s->last_duration;
    if (in) s->last_duration = in->duration;

    if (vaapi_pending_push(s, pkt) != ZST_OK) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
vaapi_receive_packets(vaapi_video_encoder_t* s, const zst_buffer_t* in)
{
    AVPacket* av_pkt = av_packet_alloc();
    if (!av_pkt) return ZST_ERROR;

    while (1) {
        int ret = avcodec_receive_packet(s->codec_ctx, av_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&av_pkt);
            return ZST_OK;
        }
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            ZST_LOG_WARN("vaapienc", "failed to receive VA-API packet: %s", err);
            av_packet_free(&av_pkt);
            return ZST_ERROR;
        }

        zst_result_t qret = vaapi_queue_packet(s, av_pkt, in);
        av_packet_unref(av_pkt);
        if (qret != ZST_OK) {
            ZST_LOG_WARN("vaapienc", "failed to queue encoded VA-API packet");
            av_packet_free(&av_pkt);
            return qret;
        }
    }
}

static zst_result_t
vaapi_queue_eos(vaapi_video_encoder_t* s)
{
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!eos_buf) return ZST_ERROR;
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
    if (vaapi_pending_push(s, eos_buf) != ZST_OK) {
        zst_buffer_unref(eos_buf);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
vaapi_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    vaapi_video_encoder_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;

    zst_buffer_t* pending = vaapi_pending_pop(s);
    if (pending) {
        *out = pending;
        return ZST_OK;
    }

    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->initialized && !s->draining) {
            s->draining = 1;
            int ret = avcodec_send_frame(s->codec_ctx, NULL);
            if (ret < 0 && ret != AVERROR_EOF) return ZST_ERROR;
            if (vaapi_receive_packets(s, NULL) != ZST_OK) return ZST_ERROR;
        }
        if (vaapi_queue_eos(s) != ZST_OK) return ZST_ERROR;
        *out = vaapi_pending_pop(s);
        return ZST_OK;
    }

    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;
    if (!s->initialized) {
        if (vaapi_init_encoder(s, frame->width, frame->height) != ZST_OK) return ZST_ERROR;
    }

    AVFrame* hw = NULL;
    if (vaapi_make_hw_frame(s, in, &hw) != ZST_OK) return ZST_ERROR;

    if (s->force_keyframe) {
        hw->pict_type = AV_PICTURE_TYPE_I;
        s->force_keyframe = 0;
    }

    int ret = avcodec_send_frame(s->codec_ctx, hw);
    if (ret == AVERROR(EAGAIN)) {
        if (vaapi_receive_packets(s, in) != ZST_OK) {
            av_frame_free(&hw);
            return ZST_ERROR;
        }
        ret = avcodec_send_frame(s->codec_ctx, hw);
    }
    av_frame_free(&hw);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        ZST_LOG_WARN("vaapienc", "failed to send frame to VA-API encoder: %s", err);
        return ZST_ERROR;
    }

    if (vaapi_receive_packets(s, in) != ZST_OK) return ZST_ERROR;
    *out = vaapi_pending_pop(s);
    return ZST_OK;
}

static zst_caps_t*
vaapi_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    vaapi_video_encoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "I420"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "NV12"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUYV"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUY2"));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video(vaapi_media_type(s),
                                                           (int)s->width,
                                                           (int)s->height,
                                                           (double)(s->fps_num > 0 ? s->fps_num : VAAPI_ENC_DEFAULT_FPS),
                                                           s->profile[0] ? s->profile : "main"));
    }
    return caps;
}

static zst_result_t
vaapi_set_property(zst_element_t* el, const char* name, const char* value)
{
    vaapi_video_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized && vaapi_is_config_property(name)) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(s->device, sizeof(s->device), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "codec") == 0) {
        if (strcmp(value, "h264") != 0 && strcmp(value, "avc") != 0 &&
            strcmp(value, "h265") != 0 && strcmp(value, "hevc") != 0) return ZST_ERROR;
        snprintf(s->codec, sizeof(s->codec), "%s", (strcmp(value, "avc") == 0) ? "h264" : (strcmp(value, "hevc") == 0 ? "h265" : value));
        return ZST_OK;
    } else if (strcmp(name, "preset") == 0) {
        snprintf(s->preset, sizeof(s->preset), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
        if (s->bitrate < 1) s->bitrate = 1;
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        s->gop_size = atoi(value);
        if (s->gop_size < 1) s->gop_size = 1;
        return ZST_OK;
    } else if (strcmp(name, "profile") == 0) {
        snprintf(s->profile, sizeof(s->profile), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "level") == 0) {
        snprintf(s->level, sizeof(s->level), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        int num = 0, den = 1;
        if (sscanf(value, "%d/%d", &num, &den) >= 1 && num > 0) {
            s->fps_num = num;
            s->fps_den = den > 0 ? den : 1;
        } else {
            s->fps_num = atoi(value);
            s->fps_den = 1;
        }
        return s->fps_num > 0 ? ZST_OK : ZST_ERROR;
    } else if (strcmp(name, "rate-control") == 0) {
        snprintf(s->rate_control, sizeof(s->rate_control), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
vaapi_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    vaapi_video_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "device") == 0) {
        snprintf(value_out, max_len, "%s", s->device);
    } else if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->codec);
    } else if (strcmp(name, "preset") == 0) {
        snprintf(value_out, max_len, "%s", s->preset);
    } else if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%" PRId64, s->bitrate);
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        snprintf(value_out, max_len, "%d", s->gop_size);
    } else if (strcmp(name, "profile") == 0) {
        snprintf(value_out, max_len, "%s", s->profile);
    } else if (strcmp(name, "level") == 0) {
        snprintf(value_out, max_len, "%s", s->level);
    } else if (strcmp(name, "fps") == 0) {
        snprintf(value_out, max_len, "%d/%d", s->fps_num, s->fps_den);
    } else if (strcmp(name, "rate-control") == 0) {
        snprintf(value_out, max_len, "%s", s->rate_control);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
vaapi_get_pool(zst_element_t* el)
{
    vaapi_video_encoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
vaapi_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    vaapi_video_encoder_t* s = pad->parent->priv;
    zst_buffer_t* out = NULL;
    zst_result_t ret = vaapi_process(pad->parent, buf, &out);

    while (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
            if (push_ret != ZST_OK) return ret;
        } else {
            zst_buffer_unref(out);
        }
        out = vaapi_pending_pop(s);
    }

    return ret;
}

static zst_result_t
vaapi_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    vaapi_video_encoder_t* s = el->priv;
    (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_FORCE_KEYFRAME) {
        s->force_keyframe = 1;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_vaapi_ops = {
    .name = "vaapienc",
    .open = vaapi_open,
    .close = vaapi_close,
    .process = vaapi_process,
    .event = vaapi_event,
    .get_caps = vaapi_get_caps,
    .set_property = vaapi_set_property,
    .get_property = vaapi_get_property,
    .get_pool = vaapi_get_pool
};

zst_element_t*
zst_vaapi_video_encoder_create(void)
{
    vaapi_video_encoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->device, sizeof(priv->device), "%s", VAAPI_ENC_DEFAULT_DEVICE);
    snprintf(priv->codec, sizeof(priv->codec), "h264");
    snprintf(priv->preset, sizeof(priv->preset), "balanced");
    snprintf(priv->profile, sizeof(priv->profile), "main");
    priv->rate_control[0] = '\0';
    priv->level[0] = '\0';
    priv->bitrate = VAAPI_ENC_DEFAULT_BITRATE;
    priv->gop_size = VAAPI_ENC_DEFAULT_GOP;
    priv->fps_num = VAAPI_ENC_DEFAULT_FPS;
    priv->fps_den = 1;

    zst_element_t* el = zst_element_create(&g_vaapi_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    if (!priv->sinkpad || !priv->srcpad) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sinkpad->push = vaapi_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "vaapienc") == 0 || strcmp(name, "vaapi_video_encoder") == 0) {
        return zst_vaapi_video_encoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_vaapienc_properties[] = {
    { "device", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, VAAPI_ENC_DEFAULT_DEVICE, "DRM render node path" },
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "balanced", "speed, balanced, quality" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4000000", "Target bitrate in bits/sec" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "Codec profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Codec level" },
    { "fps", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30/1", "Frame rate as integer or num/den" },
    { "rate-control", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "VAAPI rate control mode (empty = backend default)" }
};

static const zst_pad_template_t g_vaapienc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};

static const zst_element_desc_t g_vaapienc_elements[] = {
    {
        .name = "vaapienc",
        .long_name = "VA-API Video Encoder",
        .category = "Codec/Encoder",
        .description = "Hardware H.264/H.265 video encoder using Linux VA-API",
        .author = "zstreamer",
        .properties = g_vaapienc_properties,
        .nb_properties = sizeof(g_vaapienc_properties) / sizeof(g_vaapienc_properties[0]),
        .pads = g_vaapienc_pads,
        .nb_pads = sizeof(g_vaapienc_pads) / sizeof(g_vaapienc_pads[0]),
        .create = NULL
    },
    {
        .name = "vaapi_video_encoder",
        .long_name = "VA-API Video Encoder",
        .category = "Codec/Encoder",
        .description = "Alias for vaapienc",
        .author = "zstreamer",
        .properties = g_vaapienc_properties,
        .nb_properties = sizeof(g_vaapienc_properties) / sizeof(g_vaapienc_properties[0]),
        .pads = g_vaapienc_pads,
        .nb_pads = sizeof(g_vaapienc_pads) / sizeof(g_vaapienc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "vaapienc_plugin",
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
    if (nb_elements_out) *nb_elements_out = sizeof(g_vaapienc_elements) / sizeof(g_vaapienc_elements[0]);
    return g_vaapienc_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif
