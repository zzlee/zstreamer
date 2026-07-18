/*=============================================================================
    x265_encoder.c — x265 H.265 video encoder implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <x265.h>

#include "zst_element.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"

typedef struct x265_pending_packet {
    zst_buffer_t* buf;
    struct x265_pending_packet* next;
} x265_pending_packet_t;

typedef struct {
    x265_encoder*   x265;
    x265_param*     param;
    x265_picture*   pic_in;
    x265_picture*   pic_out;
    uint32_t        width;
    uint32_t        height;
    int             initialized;
    int             draining;
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

    x265_pending_packet_t* pending_head;
    x265_pending_packet_t* pending_tail;
} x265_encoder_ctx_t;

static void
x265_pending_clear(x265_encoder_ctx_t* s)
{
    x265_pending_packet_t* p = s->pending_head;
    while (p) {
        x265_pending_packet_t* next = p->next;
        zst_buffer_unref(p->buf);
        free(p);
        p = next;
    }
    s->pending_head = NULL;
    s->pending_tail = NULL;
}

static zst_result_t
x265_pending_push(x265_encoder_ctx_t* s, zst_buffer_t* buf)
{
    x265_pending_packet_t* node = calloc(1, sizeof(*node));
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
x265_pending_pop(x265_encoder_ctx_t* s)
{
    x265_pending_packet_t* node = s->pending_head;
    if (!node) return NULL;
    s->pending_head = node->next;
    if (!s->pending_head) s->pending_tail = NULL;
    zst_buffer_t* buf = node->buf;
    free(node);
    return buf;
}

static zst_result_t
x265_open(zst_element_t* el)
{
    x265_encoder_ctx_t* s = el->priv;
    s->initialized = 0;
    s->draining = 0;
    s->x265 = NULL;
    s->pool = NULL;
    s->param = x265_param_alloc();
    s->pic_in = x265_picture_alloc();
    s->pic_out = x265_picture_alloc();
    x265_pending_clear(s);
    return ZST_OK;
}

static int
x265_is_config_property(const char* name)
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
x265_close(zst_element_t* el)
{
    x265_encoder_ctx_t* s = el->priv;
    x265_pending_clear(s);
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->x265) {
        x265_encoder_close(s->x265);
        s->x265 = NULL;
    }
    if (s->param) {
        x265_param_free(s->param);
        s->param = NULL;
    }
    if (s->pic_in) {
        /* NULL-out plane pointers before freeing — they reference external
         * zst_buffer memory (zero-copy). x265_picture_clean would otherwise
         * X265_FREE memory it doesn't own. */
        s->pic_in->planes[0] = NULL;
        s->pic_in->planes[1] = NULL;
        s->pic_in->planes[2] = NULL;
        x265_picture_free(s->pic_in);
        s->pic_in = NULL;
    }
    if (s->pic_out) {
        s->pic_out->planes[0] = NULL;
        s->pic_out->planes[1] = NULL;
        s->pic_out->planes[2] = NULL;
        x265_picture_free(s->pic_out);
        s->pic_out = NULL;
    }
    s->initialized = 0;
    s->draining = 0;
    return ZST_OK;
}

static zst_result_t
x265_init_encoder(x265_encoder_ctx_t* s, uint32_t width, uint32_t height)
{
    s->width = width;
    s->height = height;

    /* Use user-configured preset and tune, fall back to defaults */
    const char* preset = s->preset[0] ? s->preset : "ultrafast";
    const char* tune   = s->tune[0]   ? s->tune   : "zerolatency";

    if (x265_param_default_preset(s->param, preset, tune) < 0) {
        return ZST_ERROR;
    }

    s->param->internalCsp = X265_CSP_I420;
    s->param->sourceWidth = width;
    s->param->sourceHeight = height;
    s->param->bRepeatHeaders = 1;
    s->param->bAnnexB = 1;

    /* Use configured framerate, default 30/1 */
    s->param->fpsNum = s->fps_num > 0 ? s->fps_num : 30;
    s->param->fpsDenom = s->fps_den > 0 ? s->fps_den : 1;

    /* Rate control: prefer CRF if bitrate not set, else ABR */
    if (s->bitrate > 0) {
        s->param->rc.rateControlMode = X265_RC_ABR;
        s->param->rc.bitrate = (int)(s->bitrate / 1000);
    } else {
        s->param->rc.rateControlMode = X265_RC_CRF;
        s->param->rc.rfConstant = (s->crf > 0.0) ? s->crf : 23.0;
    }

    /* GOP / keyint configuration */
    if (s->gop_size > 0) {
        s->param->keyframeMax = s->gop_size;
    }
    if (s->keyint_min > 0) {
        s->param->keyframeMin = s->keyint_min;
    }

    /* Profile / Level */
    if (s->profile[0]) {
        x265_param_apply_profile(s->param, s->profile);
    }

    x265_picture_init(s->param, s->pic_in);
    x265_picture_init(s->param, s->pic_out);

    s->x265 = x265_encoder_open(s->param);
    if (!s->x265) {
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
        x265_encoder_close(s->x265);
        s->x265 = NULL;
        return ZST_ERROR;
    }

    s->initialized = 1;
    s->draining = 0;
    return ZST_OK;
}

static zst_result_t
x265_queue_eos(x265_encoder_ctx_t* s)
{
    zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    if (!eos_buf) return ZST_ERROR;
    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
    if (x265_pending_push(s, eos_buf) != ZST_OK) {
        zst_buffer_unref(eos_buf);
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
x265_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    x265_encoder_ctx_t* s = el->priv;
    if (!out) return ZST_ERROR;
    *out = NULL;

    zst_buffer_t* pending = x265_pending_pop(s);
    if (pending) {
        *out = pending;
        return ZST_OK;
    }

    if (!in) return ZST_ERROR;

    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        if (s->initialized && s->x265 && !s->draining) {
            s->draining = 1;
            x265_nal* nals = NULL;
            uint32_t i_nals = 0;
            while (x265_encoder_encode(s->x265, &nals, &i_nals, NULL, s->pic_out) > 0 && nals) {
                zst_buffer_t* pkt = NULL;
                if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) == ZST_OK) {
                    uint8_t* enc_data = pkt->memory.data;
                    uint8_t* ptr = enc_data;
                    for (uint32_t i = 0; i < i_nals; i++) {
                        memcpy(ptr, nals[i].payload, nals[i].sizeBytes);
                        ptr += nals[i].sizeBytes;
                    }
                    pkt->memory.size = (size_t)(ptr - enc_data);
                    pkt->pts = s->pic_out->pts;
                    pkt->dts = s->pic_out->dts;
                    x265_pending_push(s, pkt);
                }
            }
        }
        if (x265_queue_eos(s) != ZST_OK) return ZST_ERROR;
        *out = x265_pending_pop(s);
        return ZST_OK;
    }

    zst_video_frame_t* frame = in->payload;
    if (!frame) return ZST_ERROR;

    if (!s->initialized) {
        if (x265_init_encoder(s, frame->width, frame->height) != ZST_OK) {
            return ZST_ERROR;
        }
    }

    s->pic_in->colorSpace = X265_CSP_I420;
    s->pic_in->planes[0] = frame->plane[0];
    s->pic_in->planes[1] = frame->plane[1];
    s->pic_in->planes[2] = frame->plane[2];
    s->pic_in->stride[0] = frame->stride[0];
    s->pic_in->stride[1] = frame->stride[1];
    s->pic_in->stride[2] = frame->stride[2];

    s->pic_in->pts = in->pts;

    if (s->force_keyframe) {
        s->pic_in->sliceType = X265_TYPE_IDR;
        s->force_keyframe = 0;
    } else {
        s->pic_in->sliceType = X265_TYPE_AUTO;
    }

    x265_nal* nals = NULL;
    uint32_t i_nals = 0;
    int frame_size = x265_encoder_encode(s->x265, &nals, &i_nals, s->pic_in, s->pic_out);

    if (frame_size < 0) {
        return ZST_ERROR;
    }

    if (frame_size > 0 && nals) {
        zst_buffer_t* pkt = NULL;
        if (zst_buffer_pool_acquire(s->pool, &pkt, 0, 0) != ZST_OK) {
            return ZST_ERROR;
        }

        uint8_t* enc_data = pkt->memory.data;

        uint8_t* ptr = enc_data;
        for (uint32_t i = 0; i < i_nals; i++) {
            uint8_t* payload = nals[i].payload;
            int payload_size = nals[i].sizeBytes;
            memcpy(ptr, payload, payload_size);
            ptr += payload_size;
        }

        pkt->memory.size = (size_t)(ptr - enc_data);
        pkt->pts = s->pic_out->pts;
        pkt->dts = s->pic_out->dts;
        pkt->duration = in->duration;

        *out = pkt;
    } else {
        *out = NULL;
    }

    return ZST_OK;
}

static zst_result_t
x265_set_property(zst_element_t* el, const char* name, const char* value)
{
    x265_encoder_ctx_t* s = el->priv;

    if (x265_is_config_property(name) && s->initialized) {
        return ZST_ERROR;
    }

    if (strcmp(name, "preset") == 0) {
        strncpy(s->preset, value, sizeof(s->preset) - 1);
    } else if (strcmp(name, "tune") == 0) {
        strncpy(s->tune, value, sizeof(s->tune) - 1);
    } else if (strcmp(name, "crf") == 0) {
        s->crf = atof(value);
    } else if (strcmp(name, "bitrate") == 0) {
        s->bitrate = atoll(value);
    } else if (strcmp(name, "gop-size") == 0 || strcmp(name, "gop") == 0 ||
               strcmp(name, "keyint") == 0 || strcmp(name, "keyframe-interval") == 0) {
        s->gop_size = atoi(value);
    } else if (strcmp(name, "keyint-min") == 0) {
        s->keyint_min = atoi(value);
    } else if (strcmp(name, "profile") == 0) {
        strncpy(s->profile, value, sizeof(s->profile) - 1);
    } else if (strcmp(name, "level") == 0) {
        strncpy(s->level, value, sizeof(s->level) - 1);
    } else if (strcmp(name, "fps") == 0) {
        if (sscanf(value, "%d/%d", &s->fps_num, &s->fps_den) != 2) {
            s->fps_num = atoi(value);
            s->fps_den = 1;
        }
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_result_t
x265_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    x265_encoder_ctx_t* s = el->priv;

    if (strcmp(name, "preset") == 0) {
        snprintf(value_out, max_len, "%s", s->preset);
    } else if (strcmp(name, "tune") == 0) {
        snprintf(value_out, max_len, "%s", s->tune);
    } else if (strcmp(name, "crf") == 0) {
        snprintf(value_out, max_len, "%.2f", s->crf);
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
    x265_encoder_ctx_t* s = el->priv;
    return s->pool;
}

static zst_result_t
x265_event(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event)
{
    x265_encoder_ctx_t* s = el->priv;
    (void)sink_pad;
    if (event->type == ZST_PAD_EVENT_FORCE_KEYFRAME) {
        s->force_keyframe = 1;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name    = "x265enc",
    .open    = x265_open,
    .close   = x265_close,
    .process = x265_process,
    .event   = x265_event,
    .set_property = x265_set_property,
    .get_property = x265_get_property,
    .get_pool = element_get_pool
};

zst_element_t*
zst_x265_encoder_create(void)
{
    zst_element_t* el;
    x265_encoder_ctx_t* priv;
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
    if (strcmp(name, "x265enc") == 0) {
        return zst_x265_encoder_create();
    }
    return NULL;
}

static const zst_pad_template_t g_x265enc_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-h265" }
};

static const zst_element_desc_t g_x265enc_elements[] = {
    {
        .name = "x265enc",
        .long_name = "H.265 Encoder",
        .category = "Codec/Encoder",
        .description = "Encodes raw video to H.265",
        .author = "zstreamer",
        .properties = NULL,
        .nb_properties = 0,
        .pads = g_x265enc_pads,
        .nb_pads = sizeof(g_x265enc_pads) / sizeof(g_x265enc_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "x265encoder_plugin",
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
        *nb_elements_out = sizeof(g_x265enc_elements) / sizeof(g_x265enc_elements[0]);
    }
    return g_x265enc_elements;
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
