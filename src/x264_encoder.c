/*=============================================================================
    x264_encoder.c — x264 H.264 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <x264.h>

#include "zst_element.h"
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

    char            preset[32];
    char            tune[32];
    char            profile[32];
    char            level[16];
    double          crf;
    int64_t         bitrate;
    int             gop_size;
    int             keyint_min;
    int             fps_num;
    int             fps_den;
    int             force_keyframe;
} x264_encoder_t;

static zst_result_t
x264_open(zst_element_t* el)
{
    x264_encoder_t* s = el->priv;
    s->initialized = 0;
    s->x264 = NULL;
    s->pool = NULL;
    return ZST_OK;
}

static int
x264_is_config_property(const char* name)
{
    if (!name) return 0;
    return strcmp(name, "preset") == 0 ||
           strcmp(name, "tune") == 0 ||
           strcmp(name, "crf") == 0 ||
           strcmp(name, "bitrate") == 0 ||
           strcmp(name, "gop-size") == 0 ||
           strcmp(name, "gop") == 0 ||
           strcmp(name, "keyint") == 0 ||
           strcmp(name, "keyframe-interval") == 0 ||
           strcmp(name, "keyint-min") == 0 ||
           strcmp(name, "profile") == 0 ||
           strcmp(name, "level") == 0 ||
           strcmp(name, "fps") == 0;
}

static zst_result_t
x264_close(zst_element_t* el)
{
    x264_encoder_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->x264) {
        x264_encoder_close(s->x264);
        s->x264 = NULL;
    }
    s->initialized = 0;
    return ZST_OK;
}

static zst_result_t
x264_init_encoder(x264_encoder_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    /* Use user-configured preset and tune, fall back to defaults */
    const char* preset = s->preset[0] ? s->preset : "ultrafast";
    const char* tune   = s->tune[0]   ? s->tune   : "zerolatency";
    if (x264_param_default_preset(&s->param, preset, tune) < 0) {
        return ZST_ERROR;
    }

    s->param.i_csp = X264_CSP_I420;
    s->param.i_width = width;
    s->param.i_height = height;
    s->param.b_vfr_input = 0;
    s->param.b_annexb = 1;
    s->param.b_repeat_headers = 1;

    /* Use configured framerate, default 30/1 */
    s->param.i_fps_num = s->fps_num > 0 ? s->fps_num : 30;
    s->param.i_fps_den = s->fps_den > 0 ? s->fps_den : 1;
    
    /* Rate control: prefer CRF if bitrate not set, else ABR */
    if (s->bitrate > 0) {
        s->param.rc.i_rc_method = X264_RC_ABR;
        s->param.rc.i_bitrate = (int)(s->bitrate / 1000);
    } else {
        s->param.rc.i_rc_method = X264_RC_CRF;
        s->param.rc.f_rf_constant = (s->crf > 0.0) ? s->crf : 23.0;
    }

    /* GOP / keyint configuration */
    if (s->gop_size > 0) {
        s->param.i_keyint_max = s->gop_size;
    }
    if (s->keyint_min > 0) {
        s->param.i_keyint_min = s->keyint_min;
    }

    /* Apply configured profile, default "high" */
    const char* profile = s->profile[0] ? s->profile : "high";
    if (x264_param_apply_profile(&s->param, profile) < 0) {
        return ZST_ERROR;
    }

    s->x264 = x264_encoder_open(&s->param);
    if (!s->x264) {
        return ZST_ERROR;
    }

    /* Don't use x264_picture_alloc — we map external plane pointers for zero-copy.
     * Just zero-init the picture struct and set the color space. */
    memset(&s->pic_in, 0, sizeof(s->pic_in));
    s->pic_in.img.i_csp = s->param.i_csp;

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 2,
        .max_buffers = 8,
        .buffer_size = width * height * 3 / 2, // Safe upper bound for packet
        .buffer_type = ZST_BUFFER_VIDEO_PACKET
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        x264_encoder_close(s->x264);
        s->x264 = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    return ZST_OK;
}

static zst_result_t
x264_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    x264_encoder_t* s = el->priv;
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
        if (x264_init_encoder(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    /* Zero-copy: map raw YUV plane pointers and strides into x264_picture_t */
    s->pic_in.img.i_plane = 3;
    s->pic_in.img.plane[0] = frame->plane[0];
    s->pic_in.img.plane[1] = frame->plane[1];
    s->pic_in.img.plane[2] = frame->plane[2];
    s->pic_in.img.i_stride[0] = frame->stride[0];
    s->pic_in.img.i_stride[1] = frame->stride[1];
    s->pic_in.img.i_stride[2] = frame->stride[2];

    s->pic_in.i_pts = in->pts;

    if (s->force_keyframe) {
        s->pic_in.i_type = X264_TYPE_IDR;
        s->force_keyframe = 0;
    } else {
        s->pic_in.i_type = X264_TYPE_AUTO;
    }

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


static zst_result_t
x264_set_property(zst_element_t* el, const char* name, const char* value)
{
    x264_encoder_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;
    if (s->initialized && x264_is_config_property(name)) return ZST_ERROR;

    if (strcmp(name, "preset") == 0) {
        snprintf(s->preset, sizeof(s->preset), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "tune") == 0) {
        snprintf(s->tune, sizeof(s->tune), "%s", value);
        return ZST_OK;
    } else if (strcmp(name, "crf") == 0) {
        s->crf = atof(value);
        if (s->crf < 0.0) s->crf = 0.0;
        if (s->crf > 51.0) s->crf = 51.0;
        return ZST_OK;
    } else if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
        if (s->bitrate < 0) s->bitrate = 0;
        return ZST_OK;
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        s->gop_size = atoi(value);
        if (s->gop_size < 1) s->gop_size = 1;
        return ZST_OK;
    } else if (strcmp(name, "keyint-min") == 0) {
        s->keyint_min = atoi(value);
        if (s->keyint_min < 1) s->keyint_min = 1;
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
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
x264_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    x264_encoder_t* s = el->priv;
    if (!name || !value_out || max_len == 0) return ZST_ERROR;

    if (strcmp(name, "preset") == 0) {
        snprintf(value_out, max_len, "%s", s->preset);
    } else if (strcmp(name, "tune") == 0) {
        snprintf(value_out, max_len, "%s", s->tune);
    } else if (strcmp(name, "crf") == 0) {
        snprintf(value_out, max_len, "%.3f", s->crf);
    } else if (strcmp(name, "bitrate") == 0) {
        snprintf(value_out, max_len, "%" PRId64, s->bitrate);
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        snprintf(value_out, max_len, "%d", s->gop_size);
    } else if (strcmp(name, "keyint-min") == 0) {
        snprintf(value_out, max_len, "%d", s->keyint_min);
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
element_get_pool(zst_element_t* el)
{
    x264_encoder_t* s = el->priv;
    return s->pool;
}

static zst_result_t
x264_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    x264_encoder_t* s = el->priv;
    (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_FORCE_KEYFRAME) {
        s->force_keyframe = 1;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name    = "x264enc",
    .open    = x264_open,
    .close   = x264_close,
    .process = x264_process,
    .event   = x264_event,
    .set_property = x264_set_property,
    .get_property = x264_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_x264_encoder_create(void)
{
    zst_element_t* el;
    x264_encoder_t* priv;
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

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "x264enc") == 0) {
        return zst_x264_encoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_x264enc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h264" }
};

static const zst_element_desc_t g_x264enc_elements[] = {
    {
        .name = "x264enc",
        .long_name = "H.264 Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw video to H.264",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_x264enc_pads,
        .nb_pads = sizeof(g_x264enc_pads) / sizeof(g_x264enc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "x264encoder_plugin",
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
        *nb_elements_out = sizeof(g_x264enc_elements) / sizeof(g_x264enc_elements[0]);
    }
    return g_x264enc_elements;
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