/*=============================================================================
    text_overlay.c — Text rendering overlay element
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_text_overlay.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_log.h"

typedef struct {
    char text[256];
    char font_path[256];
    int font_size;
    int timecode;
    int x;
    int y;

    zst_pad_t* sinkpad;
    zst_pad_t* srcpad;
    zst_pad_t* textpad;

    pthread_mutex_t text_lock;

    /* Active subtitle text and expiry (ns) */
    char subtitle_text[512];
    zst_time_t subtitle_end_ns;

    FT_Library ft;
    FT_Face face;
    int ft_initialized;

    uint64_t frame_count;
} text_overlay_t;

static zst_result_t
text_overlay_open(zst_element_t* el)
{
    text_overlay_t* s = el->priv;
    if (FT_Init_FreeType(&s->ft)) {
        ZST_LOG_ERROR("textoverlay", "Could not initialize FreeType library");
        return ZST_ERROR;
    }
    s->ft_initialized = 1;

    if (s->font_path[0] == '\0') {
        /* Default to a likely system font */
        strcpy(s->font_path, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    }

    if (FT_New_Face(s->ft, s->font_path, 0, &s->face)) {
        ZST_LOG_ERROR("textoverlay", "Could not load font: %s", s->font_path);
        FT_Done_FreeType(s->ft);
        s->ft_initialized = 0;
        return ZST_ERROR;
    }

    FT_Set_Pixel_Sizes(s->face, 0, s->font_size > 0 ? s->font_size : 48);

    pthread_mutex_init(&s->text_lock, NULL);
    s->subtitle_text[0] = '\0';
    s->subtitle_end_ns = 0;
    s->frame_count = 0;

    return ZST_OK;
}

static zst_result_t
text_overlay_close(zst_element_t* el)
{
    text_overlay_t* s = el->priv;
    if (s->ft_initialized) {
        if (s->face) {
            FT_Done_Face(s->face);
            s->face = NULL;
        }
        FT_Done_FreeType(s->ft);
        s->ft_initialized = 0;
    }
    pthread_mutex_destroy(&s->text_lock);
    return ZST_OK;
}

static zst_result_t
text_overlay_text_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf) return ZST_ERROR;
    zst_element_t* el = pad->parent;
    if (!el) return ZST_ERROR;
    text_overlay_t* s = el->priv;

    /* Expect text in memory.data as NUL-terminated string; use pts/duration for timing */
    const char* str = NULL;
    if (buf->memory.data && buf->memory.size > 0) {
        str = (const char*)buf->memory.data;
    } else if (buf->payload) {
        str = (const char*)buf->payload;
    }

    if (str) {
        pthread_mutex_lock(&s->text_lock);
        strncpy(s->subtitle_text, str, sizeof(s->subtitle_text) - 1);
        s->subtitle_text[sizeof(s->subtitle_text) - 1] = '\0';
        s->subtitle_end_ns = buf->pts + buf->duration;
        pthread_mutex_unlock(&s->text_lock);
    }

    zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t
text_overlay_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    text_overlay_t* s = el->priv;
    if (!in || !out) return ZST_ERROR;

    /* Passthrough EOS */
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        *out = zst_buffer_ref(in);
        return ZST_OK;
    }

    if (in->type != ZST_BUFFER_VIDEO_FRAME) {
        /* Passthrough non-video buffers */
        *out = zst_buffer_ref(in);
        return ZST_OK;
    }

    /* In-place transform. Return a reference so direct process() callers own
       the output; the default pad path drops this extra ref after pushing. */
    zst_buffer_t* out_buf = zst_buffer_ref(in);

    /* Draw text */
    /* Wait, we need to know pixel format to draw */
    /* Only supporting YUV420P and NV12 for now */
    zst_video_frame_t* vf = (zst_video_frame_t*)out_buf->payload;
    if (!vf) {
        *out = out_buf;
        return ZST_OK;
    }

    zst_caps_t* caps = s->sinkpad->caps;
    const char* pixel_format = "";
    if (caps && caps->structs && caps->structs->type == ZST_CAPS_VIDEO) {
        pixel_format = caps->structs->video.pixel_format;
    }

    if (strcmp(pixel_format, "YUV420P") != 0 && strcmp(pixel_format, "NV12") != 0) {
        /* Unsupported format, just passthrough */
        *out = out_buf;
        return ZST_OK;
    }

    /* Word wrapping and text rendering loop */
    int start_x = s->x;
    int start_y = s->y; /* Baseline */
    int x = start_x;
    int y = start_y;
    int line_height = s->face->size->metrics.height >> 6;
    if (line_height == 0) line_height = s->font_size > 0 ? s->font_size * 1.2 : 50;

    int max_x = vf->width - 10;

    /* Decide which text to render: generated timecode, active subtitle, or static text */
    char timecode_text[128];
    const char* render_text = s->text;
    if (s->timecode) {
        uint64_t total_ms = in->pts / 1000000ULL;
        unsigned hh = (unsigned)(total_ms / 3600000ULL);
        unsigned mm = (unsigned)((total_ms / 60000ULL) % 60ULL);
        unsigned ss = (unsigned)((total_ms / 1000ULL) % 60ULL);
        unsigned ms = (unsigned)(total_ms % 1000ULL);
        snprintf(timecode_text, sizeof(timecode_text), "Frame: %llu  Time: %02u:%02u:%02u.%03u", (unsigned long long)s->frame_count, hh, mm, ss, ms);
        render_text = timecode_text;
    }
    s->frame_count++;
    pthread_mutex_lock(&s->text_lock);
    if (!s->timecode && s->subtitle_text[0] != '\0' && in->pts <= s->subtitle_end_ns) {
        render_text = s->subtitle_text;
    }
    /* If subtitle expired, clear it */
    if (s->subtitle_text[0] != '\0' && in->pts > s->subtitle_end_ns) {
        s->subtitle_text[0] = '\0';
        s->subtitle_end_ns = 0;
    }
    pthread_mutex_unlock(&s->text_lock);

    int i = 0;
    while (render_text[i] != '\0') {
        /* Check for explicit newline */
        if (render_text[i] == '\n') {
            x = start_x;
            y += line_height;
            i++;
            continue;
        }

        /* Measure next word */
        int word_width = 0;
        int j = i;
        while (render_text[j] != '\0' && render_text[j] != ' ' && render_text[j] != '\n') {
            if (FT_Load_Char(s->face, render_text[j], FT_LOAD_DEFAULT) == 0) {
                word_width += (s->face->glyph->advance.x >> 6);
            }
            j++;
        }

        /* If word exceeds remaining width (and it's not the start of the line), wrap */
        if (x + word_width > max_x && x > start_x) {
            x = start_x;
            y += line_height;
        }

        /* Render word and spaces */
        while (i < j || (render_text[i] == ' ' && render_text[i] != '\0')) {
            if (render_text[i] == '\n') break;

            if (render_text[i] == ' ') {
                if (FT_Load_Char(s->face, ' ', FT_LOAD_DEFAULT) == 0) {
                    x += (s->face->glyph->advance.x >> 6);
                } else {
                    x += s->font_size / 2;
                }
                i++;
                continue;
            }

            if (FT_Load_Char(s->face, render_text[i], FT_LOAD_RENDER) == 0) {
                FT_Bitmap* bmp = &s->face->glyph->bitmap;
                int draw_x = x + s->face->glyph->bitmap_left;
                int draw_y = y - s->face->glyph->bitmap_top;

                for (unsigned int r = 0; r < bmp->rows; r++) {
                    for (unsigned int c = 0; c < bmp->width; c++) {
                        int px = draw_x + c;
                        int py = draw_y + r;

                        if (px >= 0 && px < (int)vf->width && py >= 0 && py < (int)vf->height) {
                            uint8_t alpha = bmp->buffer[r * bmp->width + c];
                            if (alpha > 0) {
                                uint8_t* y_ptr = vf->plane[0] + py * vf->stride[0] + px;
                                *y_ptr = (alpha * 255 + (255 - alpha) * (*y_ptr)) / 255;

                                if (strcmp(pixel_format, "YUV420P") == 0) {
                                    uint8_t* u_ptr = vf->plane[1] + (py / 2) * vf->stride[1] + (px / 2);
                                    uint8_t* v_ptr = vf->plane[2] + (py / 2) * vf->stride[2] + (px / 2);
                                    *u_ptr = (alpha * 128 + (255 - alpha) * (*u_ptr)) / 255;
                                    *v_ptr = (alpha * 128 + (255 - alpha) * (*v_ptr)) / 255;
                                } else if (strcmp(pixel_format, "NV12") == 0) {
                                    uint8_t* uv_ptr = vf->plane[1] + (py / 2) * vf->stride[1] + (px & ~1);
                                    uv_ptr[0] = (alpha * 128 + (255 - alpha) * uv_ptr[0]) / 255;
                                    uv_ptr[1] = (alpha * 128 + (255 - alpha) * uv_ptr[1]) / 255;
                                }
                            }
                        }
                    }
                }
                x += (s->face->glyph->advance.x >> 6);
            }
            i++;
        }
    }

    *out = out_buf;
    return ZST_OK;
}

static zst_caps_t*
text_overlay_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    text_overlay_t* s = el->priv;

    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    if (pad == s->sinkpad || pad == s->srcpad) {
        /* Accept YUV420P and NV12 raw video */
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "YUV420P"));
        zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", 0, 0, 0.0, "NV12"));

        /* Alternatively, try to negotiate same as peer if one exists */
        if (pad->peer && pad->peer->caps) {
             /* A real implementation might union or intersect here */
        }
    }
    return caps;
}

static zst_result_t
text_overlay_set_property(zst_element_t* el, const char* name, const char* value)
{
    text_overlay_t* s = el->priv;
    if (strcmp(name, "text") == 0 || strcmp(name, "text-content") == 0) {
        strncpy(s->text, value, sizeof(s->text) - 1);
        s->text[sizeof(s->text) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "timecode") == 0 || strcmp(name, "show-timecode") == 0) {
        s->timecode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    } else if (strcmp(name, "font-size") == 0 || strcmp(name, "font_size") == 0) {
        s->font_size = atoi(value);
        if (s->face) FT_Set_Pixel_Sizes(s->face, 0, s->font_size > 0 ? s->font_size : 48);
        return ZST_OK;
    } else if (strcmp(name, "font-path") == 0 || strcmp(name, "font_path") == 0) {
        strncpy(s->font_path, value, sizeof(s->font_path) - 1);
        s->font_path[sizeof(s->font_path) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "x") == 0) {
        s->x = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "y") == 0) {
        s->y = atoi(value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
text_overlay_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    text_overlay_t* s = el->priv;
    if (strcmp(name, "text") == 0 || strcmp(name, "text-content") == 0) {
        snprintf(value_out, max_len, "%s", s->text);
        return ZST_OK;
    } else if (strcmp(name, "timecode") == 0 || strcmp(name, "show-timecode") == 0) {
        snprintf(value_out, max_len, "%s", s->timecode ? "true" : "false");
        return ZST_OK;
    } else if (strcmp(name, "font-size") == 0 || strcmp(name, "font_size") == 0) {
        snprintf(value_out, max_len, "%d", s->font_size);
        return ZST_OK;
    } else if (strcmp(name, "font-path") == 0 || strcmp(name, "font_path") == 0) {
        snprintf(value_out, max_len, "%s", s->font_path);
        return ZST_OK;
    } else if (strcmp(name, "x") == 0) {
        snprintf(value_out, max_len, "%d", s->x);
        return ZST_OK;
    } else if (strcmp(name, "y") == 0) {
        snprintf(value_out, max_len, "%d", s->y);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static const zst_element_ops_t g_ops = {
    .name = "textoverlay",
    .open = text_overlay_open,
    .close = text_overlay_close,
    .start = NULL,
    .stop = NULL,
    .process = text_overlay_process,
    .get_caps = text_overlay_get_caps,
    .provide_clock = NULL,
    .set_property = text_overlay_set_property,
    .get_property = text_overlay_get_property
};

zst_element_t*
zst_text_overlay_create(const char* text)
{
    text_overlay_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    if (text) {
        strncpy(priv->text, text, sizeof(priv->text) - 1);
    } else {
        strcpy(priv->text, "Hello ZStreamer");
    }
    priv->font_size = 48;
    priv->timecode = 0;
    priv->x = 10;
    priv->y = 50;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    priv->sinkpad = zst_pad_create("sink", ZST_PAD_SINK);
    priv->srcpad  = zst_pad_create("src",  ZST_PAD_SRC);
    priv->textpad = zst_pad_create("text", ZST_PAD_SINK);
    /* Override default push callback to handle subtitle text buffers */
    priv->textpad->push = text_overlay_text_push;

    zst_element_add_pad(el, priv->sinkpad);
    zst_element_add_pad(el, priv->textpad);
    zst_element_add_pad(el, priv->srcpad);

    return el;
}

zst_element_t*
zst_text_overlay_create_with_config(const zst_text_overlay_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_text_overlay_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("textoverlay");
    if (!el) return NULL;

    if (config->text) {
        zst_element_set_property_string(el, "text", config->text);
    }
    zst_element_set_property_bool(el, "timecode", config->timecode);
    if (config->font_size > 0) {
        zst_element_set_property_int(el, "font-size", config->font_size);
    }
    if (config->font_path) {
        zst_element_set_property_string(el, "font-path", config->font_path);
    }
    zst_element_set_property_int(el, "x", config->x);
    zst_element_set_property_int(el, "y", config->y);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "textoverlay") == 0) {
        return zst_text_overlay_create(NULL);
    }
    return NULL;
}

static const zst_property_spec_t g_textoverlay_properties[] = {
    { "text", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Text to overlay" },
    { "timecode", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Show timecode" },
    { "font-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Font size" },
    { "font-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Path to TTF font file" },
    { "x", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "X coordinate" },
    { "y", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "Y coordinate" }
};

static const zst_pad_template_t g_textoverlay_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" },
    { "text", ZST_PAD_SINK, ZST_PAD_ALWAYS, "text/x-raw" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_textoverlay_elements[] = {
    {
        .name = "textoverlay",
        .long_name = "Text Overlay",
        .category = "Filter/Video",
        .description = "Overlays text on video frames",
        .author = "zstreamer",
        .properties = g_textoverlay_properties,
        .nb_properties = sizeof(g_textoverlay_properties) / sizeof(g_textoverlay_properties[0]),
        .pads = g_textoverlay_pads,
        .nb_pads = sizeof(g_textoverlay_pads) / sizeof(g_textoverlay_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "textoverlay_plugin",
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
        *nb_elements_out = sizeof(g_textoverlay_elements) / sizeof(g_textoverlay_elements[0]);
    }
    return g_textoverlay_elements;
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
