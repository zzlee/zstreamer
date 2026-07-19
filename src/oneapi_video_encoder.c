/*=============================================================================
    oneapi_video_encoder.c — Intel oneAPI/oneVPL video encoder implementation

    The element is opt-in (ENABLE_ONEAPI_ENCODER) and uses oneVPL for hardware
    H.264/H.265 encoding.  It currently feeds oneVPL with system-memory NV12
    surfaces; CPU and DMA-BUF sources are uploaded into those surfaces, while
    oneAPI USM buffers are accepted only when the upstream frame metadata also
    exposes CPU-readable plane pointers.
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_oneapi_video_encoder.h"

#ifndef MFX_TARGETUSAGE_BALANCED
#define MFX_TARGETUSAGE_BALANCED 4
#endif

#ifndef MFX_RATECONTROL_VBR
#define MFX_RATECONTROL_VBR 2
#endif

#ifndef MFX_PICSTRUCT_PROGRESSIVE
#define MFX_PICSTRUCT_PROGRESSIVE 1
#endif

#ifndef MFX_IOPATTERN_IN_SYSTEM_MEMORY
#define MFX_IOPATTERN_IN_SYSTEM_MEMORY 0x01
#endif

#ifndef MFX_WRN_DEVICE_BUSY
#define MFX_WRN_DEVICE_BUSY 2
#endif

#ifndef MFX_WRN_IN_EXECUTION
#define MFX_WRN_IN_EXECUTION 1
#endif

#ifndef MFX_INFINITE
#define MFX_INFINITE 0xFFFFFFFF
#endif

#ifndef MFX_MAP_WRITE
#define MFX_MAP_WRITE 2
#endif

#define ONEAPI_ENC_DEFAULT_BITRATE 4000000
#define ONEAPI_ENC_DEFAULT_GOP     30
#define ONEAPI_ENC_DEFAULT_FPS     30

typedef struct oneapi_pending_packet {
    zst_buffer_t* buf;
    struct oneapi_pending_packet* next;
} oneapi_pending_packet_t;

typedef struct {
    mfxLoader       loader;
    mfxSession      session;
    mfxVideoParam   param;
    mfxBitstream    bitstream;

    int             session_ready;
    int             initialized;
    int             draining;
    uint32_t        width;
    uint32_t        height;
    zst_buffer_pool_t* pool;
    zst_pad_t*      sinkpad;
    zst_pad_t*      srcpad;

    char            codec[32];      /* h264 or h265 */
    char            preset[32];     /* speed, balanced, quality */
    char            profile[32];
    char            level[16];
    int64_t         bitrate;
    int             gop_size;
    int             fps_num;
    int             fps_den;
    zst_time_t      last_duration;
    int             force_keyframe;

    oneapi_pending_packet_t* pending_head;
    oneapi_pending_packet_t* pending_tail;
} oneapi_video_encoder_t;

static uint16_t
align16_u16(uint32_t v)
{
    return (uint16_t)((v + 15u) & ~15u);
}

static mfxU32
oneapi_codec_id(const oneapi_video_encoder_t* s)
{
    return (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) ?
           MFX_CODEC_HEVC : MFX_CODEC_AVC;
}

static mfxU16
oneapi_target_usage(const oneapi_video_encoder_t* s)
{
    if (strcmp(s->preset, "speed") == 0 || strcmp(s->preset, "fast") == 0) return MFX_TARGETUSAGE_BEST_SPEED;
    if (strcmp(s->preset, "quality") == 0 || strcmp(s->preset, "slow") == 0) return MFX_TARGETUSAGE_BEST_QUALITY;
    return MFX_TARGETUSAGE_BALANCED;
}

static mfxU16
oneapi_profile_id(const oneapi_video_encoder_t* s)
{
    mfxU32 codec = oneapi_codec_id(s);
    if (!s->profile[0]) return 0;

    if (codec == MFX_CODEC_HEVC) {
        if (strcmp(s->profile, "main") == 0) return MFX_PROFILE_HEVC_MAIN;
#ifdef MFX_PROFILE_HEVC_MAIN10
        if (strcmp(s->profile, "main10") == 0 || strcmp(s->profile, "main-10") == 0) return MFX_PROFILE_HEVC_MAIN10;
#endif
        return 0;
    }

    if (strcmp(s->profile, "baseline") == 0) return MFX_PROFILE_AVC_BASELINE;
    if (strcmp(s->profile, "main") == 0) return MFX_PROFILE_AVC_MAIN;
    if (strcmp(s->profile, "high") == 0) return MFX_PROFILE_AVC_HIGH;
    return 0;
}

static mfxU16
oneapi_level_id(const oneapi_video_encoder_t* s)
{
    if (!s->level[0]) return 0;
    int major = 0;
    int minor = 0;
    if (sscanf(s->level, "%d.%d", &major, &minor) == 2 && major > 0) {
        return (mfxU16)(major * 10 + minor);
    }
    int numeric = atoi(s->level);
    return numeric > 0 ? (mfxU16)numeric : 0;
}

static void
oneapi_pending_clear(oneapi_video_encoder_t* s)
{
    oneapi_pending_packet_t* p = s->pending_head;
    while (p) {
        oneapi_pending_packet_t* next = p->next;
        zst_buffer_unref(p->buf);
        free(p);
        p = next;
    }
    s->pending_head = NULL;
    s->pending_tail = NULL;
}

static zst_result_t
oneapi_pending_push(oneapi_video_encoder_t* s, zst_buffer_t* buf)
{
    oneapi_pending_packet_t* node = calloc(1, sizeof(*node));
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
oneapi_pending_pop(oneapi_video_encoder_t* s)
{
    oneapi_pending_packet_t* node = s->pending_head;
    if (!node) return NULL;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    zst_buffer_t* buf = node->buf;
    free(node);
    return buf;
}

static int
oneapi_is_config_property(const char* name)
{
    if (!name) return 0;
    return strcmp(name, "codec") == 0 ||
           strcmp(name, "preset") == 0 ||
           strcmp(name, "bitrate") == 0 ||
           strcmp(name, "gop-size") == 0 ||
           strcmp(name, "gop") == 0 ||
           strcmp(name, "keyint") == 0 ||
           strcmp(name, "keyframe-interval") == 0 ||
           strcmp(name, "profile") == 0 ||
           strcmp(name, "level") == 0 ||
           strcmp(name, "fps") == 0;
}

static zst_result_t
oneapi_config_u32(mfxLoader loader, const char* name, mfxU32 value)
{
    mfxConfig cfg = MFXCreateConfig(loader);
    if (!cfg) return ZST_ERROR;

    mfxVariant v;
    memset(&v, 0, sizeof(v));
    v.Type = MFX_VARIANT_TYPE_U32;
    v.Data.U32 = value;

    return (MFXSetConfigFilterProperty(cfg, (const mfxU8*)name, v) == MFX_ERR_NONE) ? ZST_OK : ZST_ERROR;
}

static zst_result_t
oneapi_create_session(oneapi_video_encoder_t* s)
{
    s->loader = MFXLoad();
    if (!s->loader) {
        ZST_LOG_WARN("oneapienc", "oneVPL loader unavailable");
        return ZST_ERROR;
    }

    if (oneapi_config_u32(s->loader, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE) != ZST_OK ||
        oneapi_config_u32(s->loader,
                          "mfxImplDescription.mfxEncoderDescription.encoder.CodecID",
                          oneapi_codec_id(s)) != ZST_OK) {
        ZST_LOG_WARN("oneapienc", "oneVPL dispatcher does not accept encoder filters");
        MFXUnload(s->loader);
        s->loader = NULL;
        return ZST_ERROR;
    }

    mfxStatus sts = MFXCreateSession(s->loader, 0, &s->session);
    if (sts != MFX_ERR_NONE) {
        ZST_LOG_WARN("oneapienc", "no Intel oneVPL hardware encoder session available (status=%d)", (int)sts);
        MFXUnload(s->loader);
        s->loader = NULL;
        s->session = NULL;
        return ZST_ERROR;
    }

    s->session_ready = 1;
    return ZST_OK;
}

static zst_result_t
oneapi_video_encoder_open(zst_element_t* el)
{
    oneapi_video_encoder_t* s = el->priv;
    s->initialized = 0;
    s->draining = 0;
    s->session_ready = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->last_duration = 0;
    oneapi_pending_clear(s);
    memset(&s->param, 0, sizeof(s->param));
    memset(&s->bitstream, 0, sizeof(s->bitstream));

    return oneapi_create_session(s);
}

static zst_result_t
oneapi_video_encoder_close(zst_element_t* el)
{
    oneapi_video_encoder_t* s = el->priv;
    oneapi_pending_clear(s);

    if (s->initialized && s->session) {
        MFXVideoENCODE_Close(s->session);
    }
    s->initialized = 0;
    s->draining = 0;

    if (s->bitstream.Data) {
        free(s->bitstream.Data);
        memset(&s->bitstream, 0, sizeof(s->bitstream));
    }

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    if (s->session) {
        MFXClose(s->session);
        s->session = NULL;
    }
    if (s->loader) {
        MFXUnload(s->loader);
        s->loader = NULL;
    }
    s->session_ready = 0;
    return ZST_OK;
}

static zst_result_t
oneapi_init_encoder(oneapi_video_encoder_t* s, uint32_t width, uint32_t height)
{
    if (!s->session_ready || !s->session) return ZST_ERROR;

    memset(&s->param, 0, sizeof(s->param));
    s->param.mfx.CodecId = oneapi_codec_id(s);
    s->param.mfx.TargetUsage = oneapi_target_usage(s);
    s->param.mfx.TargetKbps = (mfxU16)((s->bitrate > 0 ? s->bitrate : ONEAPI_ENC_DEFAULT_BITRATE) / 1000);
    if (s->param.mfx.TargetKbps == 0) s->param.mfx.TargetKbps = 1;
    s->param.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    s->param.mfx.CodecProfile = oneapi_profile_id(s);
    s->param.mfx.CodecLevel = oneapi_level_id(s);
    s->param.mfx.GopPicSize = (mfxU16)(s->gop_size > 0 ? s->gop_size : ONEAPI_ENC_DEFAULT_GOP);
    s->param.mfx.GopRefDist = 1;
    s->param.mfx.IdrInterval = 1;
    s->param.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    s->param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    s->param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    s->param.mfx.FrameInfo.CropX = 0;
    s->param.mfx.FrameInfo.CropY = 0;
    s->param.mfx.FrameInfo.CropW = (mfxU16)width;
    s->param.mfx.FrameInfo.CropH = (mfxU16)height;
    s->param.mfx.FrameInfo.Width = align16_u16(width);
    s->param.mfx.FrameInfo.Height = align16_u16(height);
    s->param.mfx.FrameInfo.FrameRateExtN = (mfxU32)(s->fps_num > 0 ? s->fps_num : ONEAPI_ENC_DEFAULT_FPS);
    s->param.mfx.FrameInfo.FrameRateExtD = (mfxU32)(s->fps_den > 0 ? s->fps_den : 1);
    s->param.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    s->param.AsyncDepth = 1;

    mfxStatus sts = MFXVideoENCODE_Query(s->session, &s->param, &s->param);
    if (sts < MFX_ERR_NONE) {
        ZST_LOG_WARN("oneapienc", "oneVPL encoder query failed (status=%d)", (int)sts);
        return ZST_ERROR;
    }

    sts = MFXVideoENCODE_Init(s->session, &s->param);
    if (sts < MFX_ERR_NONE) {
        ZST_LOG_WARN("oneapienc", "oneVPL encoder init failed (status=%d)", (int)sts);
        return ZST_ERROR;
    }

    (void)MFXVideoENCODE_GetVideoParam(s->session, &s->param);
    size_t bs_size = (size_t)s->param.mfx.BufferSizeInKB * 1024u;
    if (bs_size < (size_t)width * height) bs_size = (size_t)width * height * 4u + 1024u * 1024u;

    s->bitstream.MaxLength = (mfxU32)bs_size;
    s->bitstream.Data = (mfxU8*)calloc(1, bs_size);
    if (!s->bitstream.Data) {
        MFXVideoENCODE_Close(s->session);
        return ZST_ERROR;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = bs_size,
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        free(s->bitstream.Data);
        memset(&s->bitstream, 0, sizeof(s->bitstream));
        MFXVideoENCODE_Close(s->session);
        return ZST_ERROR;
    }

    s->width = width;
    s->height = height;
    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
oneapi_copy_i420_to_surface(mfxFrameSurface1* surface, const zst_video_frame_t* frame)
{
    if (!surface || !frame || !frame->plane[0] || !frame->plane[1] || !frame->plane[2]) return ZST_ERROR;

    if (surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus sts = surface->FrameInterface->Map(surface, MFX_MAP_WRITE);
        if (sts < MFX_ERR_NONE) return ZST_ERROR;
    }

    uint8_t* dst_y = surface->Data.Y;
    uint8_t* dst_uv = surface->Data.UV ? surface->Data.UV : surface->Data.U;
    mfxU16 pitch = surface->Data.Pitch;
    if (!dst_y || !dst_uv || pitch == 0) {
        if (surface->FrameInterface && surface->FrameInterface->Unmap) surface->FrameInterface->Unmap(surface);
        return ZST_ERROR;
    }

    uint32_t width = frame->width;
    uint32_t height = frame->height;
    for (uint32_t row = 0; row < height; row++) {
        memcpy(dst_y + (size_t)row * pitch,
               frame->plane[0] + (size_t)row * frame->stride[0],
               width);
    }

    for (uint32_t row = 0; row < height / 2; row++) {
        uint8_t* uv = dst_uv + (size_t)row * pitch;
        const uint8_t* u = frame->plane[1] + (size_t)row * frame->stride[1];
        const uint8_t* v = frame->plane[2] + (size_t)row * frame->stride[2];
        for (uint32_t col = 0; col < width / 2; col++) {
            uv[col * 2u + 0u] = u[col];
            uv[col * 2u + 1u] = v[col];
        }
    }

    if (surface->FrameInterface && surface->FrameInterface->Unmap) {
        mfxStatus sts = surface->FrameInterface->Unmap(surface);
        if (sts < MFX_ERR_NONE) return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
oneapi_queue_packet(oneapi_video_encoder_t* s, const zst_buffer_t* in)
{
    zst_buffer_t* pkt = NULL;
    if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) return ZST_ERROR;

    if (s->bitstream.DataLength > pkt->memory.size) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }

    memcpy(pkt->memory.data,
           s->bitstream.Data + s->bitstream.DataOffset,
           s->bitstream.DataLength);
    pkt->memory.size = s->bitstream.DataLength;
    if (in) {
        pkt->pts = in->pts;
        pkt->dts = in->dts ? in->dts : in->pts;
        pkt->duration = in->duration;
        s->last_duration = in->duration;
    } else {
        pkt->duration = s->last_duration;
    }

    if (oneapi_pending_push(s, pkt) != ZST_OK) {
        zst_buffer_unref(pkt);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static mfxStatus
oneapi_encode_async(oneapi_video_encoder_t* s, mfxFrameSurface1* surface, mfxSyncPoint* syncp)
{
    s->bitstream.DataOffset = 0;
    s->bitstream.DataLength = 0;
    *syncp = NULL;

    mfxEncodeCtrl ctrl = {0};
    mfxEncodeCtrl* pctrl = NULL;

    if (s->force_keyframe && surface) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        pctrl = &ctrl;
        s->force_keyframe = 0;
    }

    mfxStatus sts;
    for (int i = 0; i < 50; i++) {
        sts = MFXVideoENCODE_EncodeFrameAsync(s->session, pctrl, surface, &s->bitstream, syncp);
        if (sts != MFX_WRN_DEVICE_BUSY) return sts;
        (void)MFXVideoCORE_SyncOperation(s->session, *syncp, 1);
    }
    return MFX_WRN_DEVICE_BUSY;
}

static zst_result_t
oneapi_send_surface(oneapi_video_encoder_t* s, mfxFrameSurface1* surface, const zst_buffer_t* in)
{
    mfxSyncPoint syncp = NULL;
    mfxStatus sts = oneapi_encode_async(s, surface, &syncp);

    if (surface && surface->FrameInterface && surface->FrameInterface->Release) {
        surface->FrameInterface->Release(surface);
    }

    if (sts == MFX_ERR_MORE_DATA || sts == MFX_ERR_MORE_SURFACE) return ZST_OK;
    if (sts < MFX_ERR_NONE && sts != MFX_WRN_IN_EXECUTION) {
        ZST_LOG_WARN("oneapienc", "oneVPL encode failed (status=%d)", (int)sts);
        return ZST_ERROR;
    }

    if (syncp) {
        sts = MFXVideoCORE_SyncOperation(s->session, syncp, MFX_INFINITE);
        if (sts < MFX_ERR_NONE) return ZST_ERROR;
    }

    if (s->bitstream.DataLength > 0) {
        return oneapi_queue_packet(s, in);
    }
    return ZST_OK;
}

static zst_result_t
oneapi_queue_eos(oneapi_video_encoder_t* s)
{
    zst_buffer_t* eos = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!eos) return ZST_ERROR;
    eos->flags |= ZST_BUFFER_FLAG_EOS;
    if (oneapi_pending_push(s, eos) != ZST_OK) {
        zst_buffer_unref(eos);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
oneapi_drain(oneapi_video_encoder_t* s)
{
    while (1) {
        mfxSyncPoint syncp = NULL;
        mfxStatus sts = oneapi_encode_async(s, NULL, &syncp);
        if (sts == MFX_ERR_MORE_DATA) return ZST_OK;
        if (sts < MFX_ERR_NONE && sts != MFX_WRN_IN_EXECUTION) return ZST_ERROR;
        if (syncp) {
            sts = MFXVideoCORE_SyncOperation(s->session, syncp, MFX_INFINITE);
            if (sts < MFX_ERR_NONE) return ZST_ERROR;
        }
        if (s->bitstream.DataLength > 0 && oneapi_queue_packet(s, NULL) != ZST_OK) return ZST_ERROR;
    }
}

static zst_result_t
oneapi_video_encoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    oneapi_video_encoder_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;

    zst_buffer_t* pending = oneapi_pending_pop(s);
    if (pending) {
        *out = pending;
        return ZST_OK;
    }

    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->initialized && !s->draining) {
            s->draining = 1;
            if (oneapi_drain(s) != ZST_OK) return ZST_ERROR;
        }
        if (oneapi_queue_eos(s) != ZST_OK) return ZST_ERROR;
        *out = oneapi_pending_pop(s);
        return ZST_OK;
    }

    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;
    if (!s->initialized) {
        if (oneapi_init_encoder(s, frame->width, frame->height) != ZST_OK) return ZST_ERROR;
    }

    mfxFrameSurface1* surface = NULL;
    mfxStatus sts = MFXMemory_GetSurfaceForEncode(s->session, &surface);
    if (sts < MFX_ERR_NONE || !surface) {
        ZST_LOG_WARN("oneapienc", "failed to obtain oneVPL input surface (status=%d)", (int)sts);
        return ZST_ERROR;
    }

    if (oneapi_copy_i420_to_surface(surface, frame) != ZST_OK) {
        if (surface->FrameInterface && surface->FrameInterface->Release) surface->FrameInterface->Release(surface);
        return ZST_ERROR;
    }

    if (oneapi_send_surface(s, surface, in) != ZST_OK) return ZST_ERROR;
    *out = oneapi_pending_pop(s);
    return ZST_OK;
}

static zst_caps_t*
oneapi_video_encoder_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    oneapi_video_encoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "I420"));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video(oneapi_codec_id(s) == MFX_CODEC_HEVC ? "video/x-h265" : "video/x-h264",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           (double)(s->fps_num > 0 ? s->fps_num : ONEAPI_ENC_DEFAULT_FPS),
                                                           s->profile[0] ? s->profile : "main"));
    }
    return caps;
}

static zst_result_t
oneapi_video_encoder_set_property(zst_element_t* el, const char* name, const char* value)
{
    oneapi_video_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized && oneapi_is_config_property(name)) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
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
    }
    return ZST_ERROR;
}

static zst_result_t
oneapi_video_encoder_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    oneapi_video_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
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
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
oneapi_video_encoder_get_pool(zst_element_t* el)
{
    oneapi_video_encoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
oneapi_video_encoder_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    oneapi_video_encoder_t* s = pad->parent->priv;
    zst_buffer_t* out = NULL;
    zst_result_t ret = oneapi_video_encoder_process(pad->parent, buf, &out);

    while (out) {
        if (pad->parent->nb_src_pads > 0 && pad->parent->src_pads[0]->peer) {
            zst_result_t push_ret = zst_pad_push(pad->parent->src_pads[0], out);
            zst_buffer_unref(out);
            if (ret == ZST_OK) ret = push_ret;
            if (push_ret != ZST_OK) return ret;
        } else {
            zst_buffer_unref(out);
        }
        out = oneapi_pending_pop(s);
    }

    return ret;
}

static zst_result_t
oneapi_video_encoder_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    oneapi_video_encoder_t* s = el->priv;
    (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_FORCE_KEYFRAME) {
        s->force_keyframe = 1;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_oneapi_video_encoder_ops = {
    .name    = "oneapienc",
    .open    = oneapi_video_encoder_open,
    .close   = oneapi_video_encoder_close,
    .process = oneapi_video_encoder_process,
    .event   = oneapi_video_encoder_event,
    .get_caps = oneapi_video_encoder_get_caps,
    .set_property = oneapi_video_encoder_set_property,
    .get_property = oneapi_video_encoder_get_property,
    .get_pool = oneapi_video_encoder_get_pool
};

zst_element_t*
zst_oneapi_video_encoder_create(void)
{
    oneapi_video_encoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->codec, sizeof(priv->codec), "h264");
    snprintf(priv->preset, sizeof(priv->preset), "balanced");
    snprintf(priv->profile, sizeof(priv->profile), "main");
    priv->level[0] = '\0';
    priv->bitrate = ONEAPI_ENC_DEFAULT_BITRATE;
    priv->gop_size = ONEAPI_ENC_DEFAULT_GOP;
    priv->fps_num = ONEAPI_ENC_DEFAULT_FPS;
    priv->fps_den = 1;

    zst_element_t* el = zst_element_create(&g_oneapi_video_encoder_ops, priv);
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
    priv->sinkpad->push = oneapi_video_encoder_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "oneapienc") == 0 || strcmp(name, "oneapi_video_encoder") == 0) {
        return zst_oneapi_video_encoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_oneapienc_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" },
    { "preset", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "balanced", "speed, balanced, quality" },
    { "bitrate", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "4000000", "Target bitrate in bits/sec" },
    { "gop-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "GOP/keyframe interval" },
    { "profile", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "main", "Codec profile" },
    { "level", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Codec level (reserved for backend support)" },
    { "fps", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30/1", "Frame rate as integer or num/den" }
};

static const zst_pad_template_t g_oneapienc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};

static const zst_element_desc_t g_oneapienc_elements[] = {
    {
        .name = "oneapienc",
        .long_name = "Intel oneAPI Video Encoder",
        .category = "Codec/Encoder",
        .description = "Hardware H.264/H.265 video encoder using Intel oneVPL",
        .author = "zstreamer",
        .properties = g_oneapienc_properties,
        .nb_properties = sizeof(g_oneapienc_properties) / sizeof(g_oneapienc_properties[0]),
        .pads = g_oneapienc_pads,
        .nb_pads = sizeof(g_oneapienc_pads) / sizeof(g_oneapienc_pads[0]),
        .create = NULL
    },
    {
        .name = "oneapi_video_encoder",
        .long_name = "Intel oneAPI Video Encoder",
        .category = "Codec/Encoder",
        .description = "Alias for oneapienc",
        .author = "zstreamer",
        .properties = g_oneapienc_properties,
        .nb_properties = sizeof(g_oneapienc_properties) / sizeof(g_oneapienc_properties[0]),
        .pads = g_oneapienc_pads,
        .nb_pads = sizeof(g_oneapienc_pads) / sizeof(g_oneapienc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "oneapienc_plugin",
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
    if (nb_elements_out) *nb_elements_out = sizeof(g_oneapienc_elements) / sizeof(g_oneapienc_elements[0]);
    return g_oneapienc_elements;
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
