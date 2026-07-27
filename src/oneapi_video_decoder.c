/*=============================================================================
    oneapi_video_decoder.c — Intel oneAPI/oneVPL video decoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zstreamer/elements/zst_oneapi_video_decoder.h"



typedef struct oneapi_pending_frame {
    zst_buffer_t* buf;
    struct oneapi_pending_frame* next;
} oneapi_pending_frame_t;

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

    int             drm_fd;
    VADisplay       va_dpy;
    int             is_sw;

    oneapi_pending_frame_t* pending_head;
    oneapi_pending_frame_t* pending_tail;
} oneapi_video_decoder_t;

static mfxU32
oneapi_codec_id(const oneapi_video_decoder_t* s)
{
    return (strcmp(s->codec, "h265") == 0 || strcmp(s->codec, "hevc") == 0) ?
           MFX_CODEC_HEVC : MFX_CODEC_AVC;
}

static void
oneapi_pending_clear(oneapi_video_decoder_t* s)
{
    oneapi_pending_frame_t* p = s->pending_head;
    while (p) {
        oneapi_pending_frame_t* next = p->next;
        zst_buffer_unref(p->buf);
        free(p);
        p = next;
    }
    s->pending_head = NULL;
    s->pending_tail = NULL;
}

static zst_result_t
oneapi_pending_push(oneapi_video_decoder_t* s, zst_buffer_t* buf)
{
    oneapi_pending_frame_t* node = calloc(1, sizeof(*node));
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
oneapi_pending_pop(oneapi_video_decoder_t* s)
{
    oneapi_pending_frame_t* node = s->pending_head;
    if (!node) return NULL;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    zst_buffer_t* buf = node->buf;
    free(node);
    return buf;
}

static void
oneapi_dec_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
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
oneapi_create_session(oneapi_video_decoder_t* s, int force_sw)
{
    s->loader = MFXLoad();
    if (!s->loader) {
        ZST_LOG_WARN("oneapidec", "oneVPL loader unavailable");
        return ZST_ERROR;
    }

    mfxU32 impl_type = force_sw ? MFX_IMPL_TYPE_SOFTWARE : MFX_IMPL_TYPE_HARDWARE;

    if (oneapi_config_u32(s->loader, "mfxImplDescription.Impl", impl_type) != ZST_OK ||
        oneapi_config_u32(s->loader,
                          "mfxImplDescription.mfxDecoderDescription.decoder.CodecID",
                          oneapi_codec_id(s)) != ZST_OK) {
        ZST_LOG_WARN("oneapidec", "oneVPL dispatcher does not accept decoder filters");
        MFXUnload(s->loader);
        s->loader = NULL;
        return ZST_ERROR;
    }

    mfxStatus sts = MFXCreateSession(s->loader, 0, &s->session);
    if (sts != MFX_ERR_NONE) {
        if (!force_sw) {
            ZST_LOG_INFO("oneapidec", "no Intel oneVPL hardware decoder session available, trying software fallback...");
            MFXUnload(s->loader);
            s->loader = NULL;
            s->session = NULL;
            return oneapi_create_session(s, 1);
        }
        ZST_LOG_WARN("oneapidec", "no Intel oneVPL decoder session available (status=%d)", (int)sts);
        MFXUnload(s->loader);
        s->loader = NULL;
        s->session = NULL;
        return ZST_ERROR;
    }



    s->session_ready = 1;
    s->is_sw = force_sw;
    return ZST_OK;
}

static zst_result_t
oneapi_video_decoder_open(zst_element_t* el)
{
    oneapi_video_decoder_t* s = el->priv;
    s->initialized = 0;
    s->draining = 0;
    s->session_ready = 0;
    s->pool = NULL;
    s->width = 0;
    s->height = 0;
    s->drm_fd = -1;
    s->va_dpy = NULL;
    s->is_sw = 0;
    oneapi_pending_clear(s);
    memset(&s->param, 0, sizeof(s->param));
    memset(&s->bitstream, 0, sizeof(s->bitstream));

    return oneapi_create_session(s, 0);
}

static zst_result_t
oneapi_video_decoder_close(zst_element_t* el)
{
    oneapi_video_decoder_t* s = el->priv;
    oneapi_pending_clear(s);

    if (s->initialized && s->session) {
        MFXVideoDECODE_Close(s->session);
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

    if (s->va_dpy) {
        vaTerminate(s->va_dpy);
        s->va_dpy = NULL;
    }
    if (s->drm_fd >= 0) {
        close(s->drm_fd);
        s->drm_fd = -1;
    }

    return ZST_OK;
}

static zst_result_t
oneapi_init_decoder(oneapi_video_decoder_t* s, mfxBitstream* bs)
{
    if (!s->session_ready || !s->session) {
        fprintf(stderr, "oneapi_init_decoder: session not ready\n");
        return ZST_ERROR;
    }

    memset(&s->param, 0, sizeof(s->param));
    s->param.mfx.CodecId = oneapi_codec_id(s);
    s->param.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    mfxStatus sts = MFXVideoDECODE_DecodeHeader(s->session, bs, &s->param);
    if (sts == MFX_ERR_MORE_DATA) {
        fprintf(stderr, "oneapi_init_decoder: DecodeHeader returned MFX_ERR_MORE_DATA\n");
        return ZST_AGAIN;
    }
    if (sts < MFX_ERR_NONE) {
        ZST_LOG_WARN("oneapidec", "oneVPL DecodeHeader failed (status=%d)", (int)sts);
        fprintf(stderr, "oneapi_init_decoder: DecodeHeader failed with sts=%d\n", (int)sts);
        return ZST_ERROR;
    }



    sts = MFXVideoDECODE_Init(s->session, &s->param);
    if (sts < MFX_ERR_NONE) {
        ZST_LOG_WARN("oneapidec", "oneVPL decoder init failed (status=%d)", (int)sts);
        fprintf(stderr, "oneapi_init_decoder: MFXVideoDECODE_Init failed with sts=%d\n", (int)sts);
        return ZST_ERROR;
    }

    uint32_t width = s->param.mfx.FrameInfo.CropW;
    uint32_t height = s->param.mfx.FrameInfo.CropH;
    if (width == 0) width = s->param.mfx.FrameInfo.Width;
    if (height == 0) height = s->param.mfx.FrameInfo.Height;

    s->width = width;
    s->height = height;

    size_t frame_size = (size_t)width * height * 3 / 2;
    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = frame_size,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        MFXVideoDECODE_Close(s->session);
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
oneapi_copy_surface_to_buffer(mfxFrameSurface1* surface, zst_buffer_t* vbuf)
{
    if (!surface || !vbuf) return ZST_ERROR;

    if (surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus sts = surface->FrameInterface->Map(surface, MFX_MAP_READ);
        if (sts < MFX_ERR_NONE) return ZST_ERROR;
    }

    uint8_t* src_y = surface->Data.Y;
    uint8_t* src_uv = surface->Data.UV ? surface->Data.UV : surface->Data.U;
    mfxU16 pitch = surface->Data.Pitch;

    if (!src_y || !src_uv || pitch == 0) {
        if (surface->FrameInterface && surface->FrameInterface->Unmap) surface->FrameInterface->Unmap(surface);
        return ZST_ERROR;
    }

    zst_video_frame_t* v_frame = vbuf->payload;
    if (!v_frame) {
        v_frame = calloc(1, sizeof(*v_frame));
        if (!v_frame) {
            if (surface->FrameInterface && surface->FrameInterface->Unmap) surface->FrameInterface->Unmap(surface);
            return ZST_ERROR;
        }
        vbuf->payload = v_frame;
        vbuf->destroy = oneapi_dec_buf_free;
    }

    uint32_t width = surface->Info.CropW;
    uint32_t height = surface->Info.CropH;
    if (width == 0) width = surface->Info.Width;
    if (height == 0) height = surface->Info.Height;

    v_frame->width = width;
    v_frame->height = height;
    v_frame->format = 1; /* NV12 */

    v_frame->stride[0] = width;
    v_frame->stride[1] = width;
    v_frame->stride[2] = 0;
    v_frame->stride[3] = 0;

    v_frame->plane[0] = vbuf->memory.data;
    v_frame->plane[1] = vbuf->memory.data + (size_t)width * height;
    v_frame->plane[2] = NULL;
    v_frame->plane[3] = NULL;

    for (uint32_t row = 0; row < height; row++) {
        memcpy(v_frame->plane[0] + (size_t)row * width,
               src_y + (size_t)row * pitch,
               width);
    }

    for (uint32_t row = 0; row < height / 2; row++) {
        memcpy(v_frame->plane[1] + (size_t)row * width,
               src_uv + (size_t)row * pitch,
               width);
    }

    vbuf->memory.size = (size_t)width * height * 3 / 2;

    if (surface->FrameInterface && surface->FrameInterface->Unmap) {
        surface->FrameInterface->Unmap(surface);
    }
    return ZST_OK;
}

static zst_result_t
oneapi_decode_all_available(zst_element_t* el, oneapi_video_decoder_t* s, zst_time_t pts, zst_time_t duration)
{
    (void)el;
    while (s->bitstream.DataLength > 0) {
        mfxFrameSurface1* surface_decoded = NULL;
        mfxSyncPoint syncp = NULL;

        mfxStatus sts = MFXVideoDECODE_DecodeFrameAsync(s->session, &s->bitstream, NULL, &surface_decoded, &syncp);

        if (sts == MFX_ERR_MORE_DATA) {
            return ZST_OK;
        }
        if (sts == MFX_WRN_VIDEO_PARAM_CHANGED) {
            continue;
        }
        if (sts < MFX_ERR_NONE) {
            ZST_LOG_WARN("oneapidec", "DecodeFrameAsync failed (status=%d)", (int)sts);
            return ZST_ERROR;
        }

        if (syncp) {
            sts = MFXVideoCORE_SyncOperation(s->session, syncp, MFX_INFINITE);
            if (sts < MFX_ERR_NONE) {
                ZST_LOG_WARN("oneapidec", "SyncOperation failed (status=%d)", (int)sts);
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            zst_buffer_t* vbuf = NULL;
            if (zst_buffer_pool_acquire(s->pool, &vbuf, 0, 0) != ZST_OK) {
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            vbuf->pts = pts;
            vbuf->dts = pts;
            vbuf->duration = duration;

            if (oneapi_copy_surface_to_buffer(surface_decoded, vbuf) != ZST_OK) {
                zst_buffer_unref(vbuf);
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                surface_decoded->FrameInterface->Release(surface_decoded);
            }

            if (oneapi_pending_push(s, vbuf) != ZST_OK) {
                zst_buffer_unref(vbuf);
                return ZST_ERROR;
            }
        }
    }
    return ZST_OK;
}

static zst_result_t
oneapi_drain_decoder(zst_element_t* el, oneapi_video_decoder_t* s)
{
    (void)el;
    while (1) {
        mfxFrameSurface1* surface_decoded = NULL;
        mfxSyncPoint syncp = NULL;

        mfxStatus sts = MFXVideoDECODE_DecodeFrameAsync(s->session, NULL, NULL, &surface_decoded, &syncp);

        if (sts == MFX_ERR_MORE_DATA) {
            return ZST_OK;
        }
        if (sts < MFX_ERR_NONE) {
            return ZST_ERROR;
        }

        if (syncp) {
            sts = MFXVideoCORE_SyncOperation(s->session, syncp, MFX_INFINITE);
            if (sts < MFX_ERR_NONE) {
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            zst_buffer_t* vbuf = NULL;
            if (zst_buffer_pool_acquire(s->pool, &vbuf, 0, 0) != ZST_OK) {
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            if (oneapi_copy_surface_to_buffer(surface_decoded, vbuf) != ZST_OK) {
                zst_buffer_unref(vbuf);
                if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                    surface_decoded->FrameInterface->Release(surface_decoded);
                }
                return ZST_ERROR;
            }

            if (surface_decoded && surface_decoded->FrameInterface && surface_decoded->FrameInterface->Release) {
                surface_decoded->FrameInterface->Release(surface_decoded);
            }

            if (oneapi_pending_push(s, vbuf) != ZST_OK) {
                zst_buffer_unref(vbuf);
                return ZST_ERROR;
            }
        }
    }
}

static zst_result_t
oneapi_video_decoder_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    oneapi_video_decoder_t* s = el->priv;
    if (out) *out = NULL;

    zst_buffer_t* pending = oneapi_pending_pop(s);
    if (pending) {
        if (out) *out = pending;
        return ZST_OK;
    }

    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->initialized && !s->draining) {
            s->draining = 1;
            if (oneapi_drain_decoder(el, s) != ZST_OK) return ZST_ERROR;
        }

        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
        if (!eos_buf) return ZST_ERROR;
        eos_buf->flags |= ZST_BUFFER_FLAG_EOS;

        if (oneapi_pending_push(s, eos_buf) != ZST_OK) {
            zst_buffer_unref(eos_buf);
            return ZST_ERROR;
        }

        if (out) *out = oneapi_pending_pop(s);
        return ZST_OK;
    }

    const uint8_t* in_data = in->memory.data;
    size_t in_size = in->memory.size;
    if (in_size > 0 && in_data) {
        if (!s->bitstream.Data || s->bitstream.MaxLength < s->bitstream.DataLength + in_size) {
            uint32_t new_max = s->bitstream.DataLength + in_size + 4096;
            uint8_t* new_data = realloc(s->bitstream.Data, new_max);
            if (!new_data) return ZST_ERROR;
            s->bitstream.Data = new_data;
            s->bitstream.MaxLength = new_max;
        }
        if (s->bitstream.DataLength > 0 && s->bitstream.DataOffset > 0) {
            memmove(s->bitstream.Data, s->bitstream.Data + s->bitstream.DataOffset, s->bitstream.DataLength);
        }
        s->bitstream.DataOffset = 0;
        memcpy(s->bitstream.Data + s->bitstream.DataLength, in_data, in_size);
        s->bitstream.DataLength += in_size;
    }

    if (!s->initialized) {
        zst_result_t init_res = oneapi_init_decoder(s, &s->bitstream);
        if (init_res == ZST_AGAIN) {
            return ZST_OK; 
        }
        if (init_res != ZST_OK) return ZST_ERROR;
    }

    zst_result_t res = oneapi_decode_all_available(el, s, in->pts, in->duration);
    if (res != ZST_OK && res != ZST_AGAIN) return res;

    if (out) *out = oneapi_pending_pop(s);
    return ZST_OK;
}

static zst_caps_t*
oneapi_video_decoder_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    oneapi_video_decoder_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad) {
        zst_caps_append(caps, zst_caps_struct_create_video(oneapi_codec_id(s) == MFX_CODEC_HEVC ? "video/x-h265" : "video/x-h264", 0, 0, 0.0, ""));
    } else if (pad == s->srcpad) {
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw",
                                                           (int)s->width,
                                                           (int)s->height,
                                                           0.0,
                                                           "NV12"));
    }
    return caps;
}

static zst_result_t
oneapi_video_decoder_set_property(zst_element_t* el, const char* name, const char* value)
{
    oneapi_video_decoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        if (strcmp(value, "h264") != 0 && strcmp(value, "avc") != 0 &&
            strcmp(value, "h265") != 0 && strcmp(value, "hevc") != 0) return ZST_ERROR;
        snprintf(s->codec, sizeof(s->codec), "%s", (strcmp(value, "avc") == 0) ? "h264" : (strcmp(value, "hevc") == 0 ? "h265" : value));
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
oneapi_video_decoder_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    oneapi_video_decoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "codec") == 0) {
        snprintf(value_out, max_len, "%s", s->codec);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_buffer_pool_t*
oneapi_video_decoder_get_pool(zst_element_t* el)
{
    oneapi_video_decoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
oneapi_video_decoder_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent || !buf) return ZST_ERROR;
    oneapi_video_decoder_t* s = pad->parent->priv;
    zst_buffer_t* out = NULL;
    zst_result_t ret = oneapi_video_decoder_process(pad->parent, buf, &out);

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

static zst_element_ops_t g_oneapi_video_decoder_ops = {
    .name    = "oneapidec",
    .open    = oneapi_video_decoder_open,
    .close   = oneapi_video_decoder_close,
    .process = oneapi_video_decoder_process,
    .get_caps = oneapi_video_decoder_get_caps,
    .set_property = oneapi_video_decoder_set_property,
    .get_property = oneapi_video_decoder_get_property,
    .get_pool = oneapi_video_decoder_get_pool
};

zst_element_t*
zst_oneapi_video_decoder_create(void)
{
    oneapi_video_decoder_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->codec, sizeof(priv->codec), "h264");

    zst_element_t* el = zst_element_create(&g_oneapi_video_decoder_ops, priv);
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
    priv->sinkpad->push = oneapi_video_decoder_sink_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->srcpad);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "oneapidec") == 0 || strcmp(name, "oneapi_video_decoder") == 0) {
        return zst_oneapi_video_decoder_create();
    }
    return NULL;
}

static const zst_property_spec_t g_oneapidec_properties[] = {
    { "codec", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "h264", "h264 or h265" }
};

static const zst_pad_template_t g_oneapidec_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h264" },
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-h265" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_oneapidec_elements[] = {
    {
        .name = "oneapidec",
        .long_name = "Intel oneAPI Video Decoder",
        .category = "Codec/Decoder",
        .description = "Hardware H.264/H.265 video decoder using Intel oneVPL",
        .author = "zstreamer",
        .properties = g_oneapidec_properties,
        .nb_properties = sizeof(g_oneapidec_properties) / sizeof(g_oneapidec_properties[0]),
        .pads = g_oneapidec_pads,
        .nb_pads = sizeof(g_oneapidec_pads) / sizeof(g_oneapidec_pads[0]),
        .create = NULL
    },
    {
        .name = "oneapi_video_decoder",
        .long_name = "Intel oneAPI Video Decoder",
        .category = "Codec/Decoder",
        .description = "Alias for oneapidec",
        .author = "zstreamer",
        .properties = g_oneapidec_properties,
        .nb_properties = sizeof(g_oneapidec_properties) / sizeof(g_oneapidec_properties[0]),
        .pads = g_oneapidec_pads,
        .nb_pads = sizeof(g_oneapidec_pads) / sizeof(g_oneapidec_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "oneapidec_plugin",
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
        *nb_elements_out = sizeof(g_oneapidec_elements) / sizeof(g_oneapidec_elements[0]);
    }
    return g_oneapidec_elements;
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
