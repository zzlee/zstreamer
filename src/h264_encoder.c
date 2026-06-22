/*=============================================================================
    h264_encoder.c — x264 H.264 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x264.h>

#include "zst_element.h"
#include "zstreamer/elements/zst_h264_encoder.h"
#include "zst_element_factory.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct {
    x264_t*         x264;
    x264_param_t    param;
    x264_picture_t  pic_in;
    x264_picture_t  pic_out;
    uint32_t        width;
    uint32_t        height;
    int             initialized;
    zst_buffer_pool_t* pool;
} h264_encoder_t;

static zst_result_t
h264_open(zst_element_t* el)
{
    h264_encoder_t* s = el->priv;
    s->initialized = 0;
    s->x264 = NULL;
    s->pool = NULL;
    return ZST_OK;
}

static zst_result_t
h264_close(zst_element_t* el)
{
    h264_encoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->x264) {
        x264_encoder_close(s->x264);
        x264_picture_clean(&s->pic_in);
        s->x264 = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
h264_init_encoder(h264_encoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    /* Preset ultrafast, tune zerolatency */
    if (x264_param_default_preset(&s->param, "ultrafast", "zerolatency") < 0) {
        return ZST_ERROR;
    }

    s->param.i_csp = X264_CSP_I420;
    s->param.i_width = width;
    s->param.i_height = height;
    s->param.b_vfr_input = 0;
    s->param.b_annexb = 1;
    s->param.b_repeat_headers = 1;
    s->param.i_fps_num = 30;
    s->param.i_fps_den = 1;
    
    /* Rate control: CRF 23 */
    s->param.rc.i_rc_method = X264_RC_CRF;
    s->param.rc.f_rf_constant = 23.0;

    /* Apply profile high */
    if (x264_param_apply_profile(&s->param, "high") < 0) {
        return ZST_ERROR;
    }

    s->x264 = x264_encoder_open(&s->param);
    if (!s->x264) {
        return ZST_ERROR;
    }

    if (x264_picture_alloc(&s->pic_in, s->param.i_csp, s->param.i_width, s->param.i_height) < 0) {
        x264_encoder_close(s->x264);
        s->x264 = NULL;
        return ZST_ERROR;
    }

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = width * height * 3 / 2, // Safe upper bound for packet
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        x264_picture_clean(&s->pic_in);
        x264_encoder_close(s->x264);
        s->x264 = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
h264_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    h264_encoder_t* s = el->priv;
    if (!in) return ZST_ERROR;

    /* If we get EOS, pass it downstream */
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
        return ZST_ERROR;
    }

    /* Deduce width and height from the incoming frame payload */
    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;

    if (!s->initialized) {
        if (h264_init_encoder(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    /* Copy raw YUV planes into x264_picture_t */
    int y_size = s->width * s->height;
    int uv_size = y_size / 4;
    memcpy(s->pic_in.img.plane[0], frame->plane[0], y_size);
    memcpy(s->pic_in.img.plane[1], frame->plane[1], uv_size);
    memcpy(s->pic_in.img.plane[2], frame->plane[2], uv_size);

    s->pic_in.i_pts = in->pts;

    x264_nal_t* nals = NULL;
    int i_nals = 0;
    int frame_size = x264_encoder_encode(s->x264, &nals, &i_nals, &s->pic_in, &s->pic_out);
    if (frame_size < 0) {
        return ZST_ERROR;
    }

    if (frame_size > 0 && nals) {
        zst_buffer_t* pkt = NULL;
        if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
            return ZST_ERROR;
        }

        uint8_t* enc_data = pkt->memory.data;

        /* Concatenate NAL units as Annex-B.  x264_nal_t payloads may be raw
           NAL payloads depending on build/API settings, so add a start code
           when one is not already present. */
        uint8_t* ptr = enc_data;
        for (int i = 0; i < i_nals; i++) {
            uint8_t* payload = nals[i].p_payload;
            int payload_size = nals[i].i_payload;
            int has_start_code = 0;
            if (payload_size >= 4 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 && payload[3] == 1) {
                has_start_code = 1;
            } else if (payload_size >= 3 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1) {
                has_start_code = 1;
            }
            if (!has_start_code) {
                ptr[0] = 0;
                ptr[1] = 0;
                ptr[2] = 0;
                ptr[3] = 1;
                ptr += 4;
            }
            memcpy(ptr, payload, payload_size);
            ptr += payload_size;
        }

        pkt->memory.size = (size_t)(ptr - enc_data);
        pkt->pts = s->pic_out.i_pts;
        pkt->dts = s->pic_out.i_dts;
        pkt->duration = in->duration;

        *out = pkt;
    } else {
        *out = NULL;
    }

    return ZST_OK;
}


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    h264_encoder_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name    = "h264enc",
    .open    = h264_open,
    .close   = h264_close,
    .process = h264_process,
    .get_pool = element_get_pool
};

zst_element_t*
zst_h264_encoder_create(void)
{
    zst_element_t* el;
    h264_encoder_t* priv;
    zst_pad_t* sink;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);

    sink = zst_pad_create("sink", ZST_PAD_SINK);
    src  = zst_pad_create("src",  ZST_PAD_SRC);

    zst_element_add_pad(el, sink);
    zst_element_add_pad(el, src);

    return el;
}



zst_element_t*
zst_h264_encoder_create_with_config(const zst_h264_encoder_config_t* config)
{
    (void)config;
    return zst_element_factory_make("h264enc");
}
#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "h264enc") == 0) {
        return zst_h264_encoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_h264enc_pads[] = {
    { "sink", ZST_PAD_SINK, "video/x-raw" },
    { "src", ZST_PAD_SRC, "video/x-h264" }
};

static const zst_element_desc_t g_h264enc_elements[] = {
    {
        .name = "h264enc",
        .long_name = "H.264 Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw video to H.264",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_h264enc_pads,
        .nb_pads = sizeof(g_h264enc_pads) / sizeof(g_h264enc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "h264encoder_plugin",
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
        *nb_elements_out = sizeof(g_h264enc_elements) / sizeof(g_h264enc_elements[0]);
    }
    return g_h264enc_elements;
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