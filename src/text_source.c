/*=============================================================================
    text_source.c — Generates video frames with rendered text (no video input)
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "zst_element.h"
#include "zst_log.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_clock.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    char text[256];
    int font_size;
    char font_path[256];
    char bg_color_str[64];
    char text_color_str[64];
    char pixel_format[32];
    int num_buffers;
    bool loop;
    bool use_clock;
    int x;
    int y;

    // Parsed colors
    uint8_t bg_r, bg_g, bg_b;
    uint8_t text_r, text_g, text_b;

    uint64_t frame_count;
    zst_buffer_pool_t* pool;

    FT_Library ft;
    FT_Face face;
    int ft_initialized;
} text_source_t;

static void text_source_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}

static void parse_color(const char* hex, uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (!hex) return;
    if (hex[0] == '#') hex++;

    if (strcasecmp(hex, "black") == 0) {
        *r = 0; *g = 0; *b = 0;
    } else if (strcasecmp(hex, "white") == 0) {
        *r = 255; *g = 255; *b = 255;
    } else if (strcasecmp(hex, "red") == 0) {
        *r = 255; *g = 0; *b = 0;
    } else if (strcasecmp(hex, "green") == 0) {
        *r = 0; *g = 255; *b = 0;
    } else if (strcasecmp(hex, "blue") == 0) {
        *r = 0; *g = 0; *b = 255;
    } else if (strcasecmp(hex, "yellow") == 0) {
        *r = 255; *g = 255; *b = 0;
    } else if (strcasecmp(hex, "cyan") == 0) {
        *r = 0; *g = 255; *b = 255;
    } else if (strcasecmp(hex, "magenta") == 0) {
        *r = 255; *g = 0; *b = 255;
    } else if (strcasecmp(hex, "gray") == 0 || strcasecmp(hex, "grey") == 0) {
        *r = 128; *g = 128; *b = 128;
    } else {
        unsigned int ri = 0, gi = 0, bi = 0;
        if (sscanf(hex, "%02x%02x%02x", &ri, &gi, &bi) == 3) {
            *r = ri; *g = gi; *b = bi;
        }
    }
}

static void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t* y, uint8_t* u, uint8_t* v)
{
    *y = (uint8_t)( 0.299 * r + 0.587 * g + 0.114 * b);
    *u = (uint8_t)(-0.169 * r - 0.331 * g + 0.500 * b + 128);
    *v = (uint8_t)( 0.500 * r - 0.419 * g - 0.081 * b + 128);
}

static zst_result_t text_source_open(zst_element_t* el)
{
    text_source_t* s = el->priv;
    s->frame_count = 0;

    if (FT_Init_FreeType(&s->ft)) {
        ZST_LOG_ERROR("textsource", "Could not initialize FreeType library");
        return ZST_ERROR;
    }
    s->ft_initialized = 1;

    if (s->font_path[0] == '\0') {
        strncpy(s->font_path, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", sizeof(s->font_path) - 1);
        s->font_path[sizeof(s->font_path) - 1] = '\0';
    }

    if (FT_New_Face(s->ft, s->font_path, 0, &s->face)) {
        ZST_LOG_ERROR("textsource", "Could not load font: %s", s->font_path);
        FT_Done_FreeType(s->ft);
        s->ft_initialized = 0;
        return ZST_ERROR;
    }

    FT_Set_Pixel_Sizes(s->face, 0, s->font_size > 0 ? s->font_size : 48);

    zst_buffer_pool_config_t pool_cfg = {
        .min_buffers = 4,
        .max_buffers = 8,
        .buffer_size = s->width * s->height * 3 / 2,
        .buffer_type = ZST_BUFFER_VIDEO_FRAME
    };
    s->pool = zst_buffer_pool_create(NULL, &pool_cfg);
    if (!s->pool) {
        FT_Done_Face(s->face);
        s->face = NULL;
        FT_Done_FreeType(s->ft);
        s->ft_initialized = 0;
        return ZST_ERROR;
    }

    return ZST_OK;
}

static zst_result_t text_source_close(zst_element_t* el)
{
    text_source_t* s = el->priv;
    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }
    if (s->ft_initialized) {
        if (s->face) {
            FT_Done_Face(s->face);
            s->face = NULL;
        }
        FT_Done_FreeType(s->ft);
        s->ft_initialized = 0;
    }
    return ZST_OK;
}

static zst_result_t text_source_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    text_source_t* s = el->priv;

    if (s->num_buffers != -1 && s->frame_count >= (uint64_t)s->num_buffers) {
        if (s->loop) {
            s->frame_count = 0;
        } else {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_VIDEO_FRAME);
            if (eos_buf) eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            *out = eos_buf;
            return ZST_OK;
        }
    }

    zst_buffer_t* buf = zst_buffer_create_with_pool(s->pool);
    if (!buf) {
        return ZST_ERROR;
    }

    uint8_t* raw_data = buf->memory.data;

    zst_video_frame_t* frame = buf->payload;
    if (!frame) {
        frame = calloc(1, sizeof(*frame));
        if (!frame) {
            zst_buffer_unref(buf);
            return ZST_ERROR;
        }
        buf->payload = frame;
        buf->destroy = text_source_buf_free;
    }

    frame->width = s->width;
    frame->height = s->height;

    uint8_t bg_y, bg_u, bg_v;
    rgb_to_yuv(s->bg_r, s->bg_g, s->bg_b, &bg_y, &bg_u, &bg_v);

    if (strcmp(s->pixel_format, "YUV420P") == 0) {
        frame->format = 0; // YUV420P
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;

        memset(frame->plane[0], bg_y, s->width * s->height);
        memset(frame->plane[1], bg_u, (s->width * s->height) / 4);
        memset(frame->plane[2], bg_v, (s->width * s->height) / 4);
    } else if (strcmp(s->pixel_format, "NV12") == 0) {
        frame->format = 1; // NV12
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = NULL;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width;
        frame->stride[2] = 0;

        memset(frame->plane[0], bg_y, s->width * s->height);

        uint8_t* uv = frame->plane[1];
        uint32_t uv_size = s->width * s->height / 2;
        for (uint32_t i = 0; i < uv_size; i += 2) {
            uv[i] = bg_u;
            uv[i+1] = bg_v;
        }
    } else {
        // Fallback to YUV420P
        frame->format = 0;
        frame->plane[0] = raw_data;
        frame->plane[1] = raw_data + s->width * s->height;
        frame->plane[2] = raw_data + s->width * s->height + (s->width * s->height) / 4;
        frame->stride[0] = s->width;
        frame->stride[1] = s->width / 2;
        frame->stride[2] = s->width / 2;

        memset(frame->plane[0], bg_y, s->width * s->height);
        memset(frame->plane[1], bg_u, (s->width * s->height) / 4);
        memset(frame->plane[2], bg_v, (s->width * s->height) / 4);
    }

    uint8_t text_y, text_u, text_v;
    rgb_to_yuv(s->text_r, s->text_g, s->text_b, &text_y, &text_u, &text_v);

    int start_x = s->x;
    int start_y = s->y;
    int x = start_x;
    int y = start_y;
    int line_height = s->face->size->metrics.height >> 6;
    if (line_height == 0) {
        line_height = s->font_size > 0 ? s->font_size * 1.2 : 50;
    }

    int max_x = frame->width - 10;

    int i = 0;
    while (s->text[i] != '\0') {
        if (s->text[i] == '\n') {
            x = start_x;
            y += line_height;
            i++;
            continue;
        }

        int word_width = 0;
        int j = i;
        while (s->text[j] != '\0' && s->text[j] != ' ' && s->text[j] != '\n') {
            if (FT_Load_Char(s->face, s->text[j], FT_LOAD_DEFAULT) == 0) {
                word_width += (s->face->glyph->advance.x >> 6);
            }
            j++;
        }

        if (x + word_width > max_x && x > start_x) {
            x = start_x;
            y += line_height;
        }

        while (i < j || (s->text[i] == ' ' && s->text[i] != '\0')) {
            if (s->text[i] == '\n') break;

            if (s->text[i] == ' ') {
                if (FT_Load_Char(s->face, ' ', FT_LOAD_DEFAULT) == 0) {
                    x += (s->face->glyph->advance.x >> 6);
                } else {
                    x += s->font_size / 2;
                }
                i++;
                continue;
            }

            if (FT_Load_Char(s->face, s->text[i], FT_LOAD_RENDER) == 0) {
                FT_Bitmap* bmp = &s->face->glyph->bitmap;
                int draw_x = x + s->face->glyph->bitmap_left;
                int draw_y = y - s->face->glyph->bitmap_top;

                for (unsigned int r = 0; r < bmp->rows; r++) {
                    for (unsigned int c = 0; c < bmp->width; c++) {
                        int px = draw_x + c;
                        int py = draw_y + r;

                        if (px >= 0 && px < (int)frame->width && py >= 0 && py < (int)frame->height) {
                            uint8_t alpha = bmp->buffer[r * bmp->width + c];
                            if (alpha > 0) {
                                if (strcmp(s->pixel_format, "YUV420P") == 0 || frame->format == 0) {
                                    uint8_t* y_ptr = frame->plane[0] + py * frame->stride[0] + px;
                                    *y_ptr = (alpha * text_y + (255 - alpha) * (*y_ptr)) / 255;

                                    uint8_t* u_ptr = frame->plane[1] + (py / 2) * frame->stride[1] + (px / 2);
                                    uint8_t* v_ptr = frame->plane[2] + (py / 2) * frame->stride[2] + (px / 2);
                                    *u_ptr = (alpha * text_u + (255 - alpha) * (*u_ptr)) / 255;
                                    *v_ptr = (alpha * text_v + (255 - alpha) * (*v_ptr)) / 255;
                                } else if (strcmp(s->pixel_format, "NV12") == 0 || frame->format == 1) {
                                    uint8_t* y_ptr = frame->plane[0] + py * frame->stride[0] + px;
                                    *y_ptr = (alpha * text_y + (255 - alpha) * (*y_ptr)) / 255;

                                    uint8_t* uv_ptr = frame->plane[1] + (py / 2) * frame->stride[1] + (px & ~1);
                                    uv_ptr[0] = (alpha * text_u + (255 - alpha) * uv_ptr[0]) / 255;
                                    uv_ptr[1] = (alpha * text_v + (255 - alpha) * uv_ptr[1]) / 255;
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

    uint64_t dur_ns = 1000000000ULL / s->fps;
    if (s->use_clock && el->clock) {
        buf->pts = zst_clock_get_time(el->clock);
    } else {
        buf->pts = s->frame_count * dur_ns;
    }
    buf->dts = buf->pts;
    buf->duration = dur_ns;

    s->frame_count++;
    *out = buf;

    return ZST_OK;
}

static zst_caps_t* text_source_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    (void)pad;
    text_source_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;
    zst_caps_append(caps, zst_caps_struct_create_video("video/x-raw", s->width, s->height, (double)s->fps, s->pixel_format));
    return caps;
}

static zst_result_t text_source_set_property(zst_element_t* el, const char* name, const char* value)
{
    text_source_t* s = el->priv;
    if (strcmp(name, "width") == 0) {
        s->width = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        s->height = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        s->fps = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "text") == 0 || strcmp(name, "text-content") == 0) {
        strncpy(s->text, value, sizeof(s->text) - 1);
        s->text[sizeof(s->text) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "font-size") == 0 || strcmp(name, "font_size") == 0) {
        s->font_size = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "font-path") == 0 || strcmp(name, "font_path") == 0) {
        strncpy(s->font_path, value, sizeof(s->font_path) - 1);
        s->font_path[sizeof(s->font_path) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "bg-color") == 0 || strcmp(name, "background-color") == 0) {
        strncpy(s->bg_color_str, value, sizeof(s->bg_color_str) - 1);
        s->bg_color_str[sizeof(s->bg_color_str) - 1] = '\0';
        parse_color(s->bg_color_str, &s->bg_r, &s->bg_g, &s->bg_b);
        return ZST_OK;
    } else if (strcmp(name, "text-color") == 0 || strcmp(name, "color") == 0 || strcmp(name, "text_color") == 0) {
        strncpy(s->text_color_str, value, sizeof(s->text_color_str) - 1);
        s->text_color_str[sizeof(s->text_color_str) - 1] = '\0';
        parse_color(s->text_color_str, &s->text_r, &s->text_g, &s->text_b);
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0 || strcmp(name, "pixel_format") == 0) {
        strncpy(s->pixel_format, value, sizeof(s->pixel_format) - 1);
        s->pixel_format[sizeof(s->pixel_format) - 1] = '\0';
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0 || strcmp(name, "num_buffers") == 0) {
        s->num_buffers = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        s->loop = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        s->use_clock = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
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

static zst_result_t text_source_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    text_source_t* s = el->priv;
    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->width);
        return ZST_OK;
    } else if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->height);
        return ZST_OK;
    } else if (strcmp(name, "fps") == 0) {
        snprintf(value_out, max_len, "%u", s->fps);
        return ZST_OK;
    } else if (strcmp(name, "text") == 0 || strcmp(name, "text-content") == 0) {
        strncpy(value_out, s->text, max_len);
        return ZST_OK;
    } else if (strcmp(name, "font-size") == 0 || strcmp(name, "font_size") == 0) {
        snprintf(value_out, max_len, "%d", s->font_size);
        return ZST_OK;
    } else if (strcmp(name, "font-path") == 0 || strcmp(name, "font_path") == 0) {
        strncpy(value_out, s->font_path, max_len);
        return ZST_OK;
    } else if (strcmp(name, "bg-color") == 0 || strcmp(name, "background-color") == 0) {
        strncpy(value_out, s->bg_color_str, max_len);
        return ZST_OK;
    } else if (strcmp(name, "text-color") == 0 || strcmp(name, "color") == 0 || strcmp(name, "text_color") == 0) {
        strncpy(value_out, s->text_color_str, max_len);
        return ZST_OK;
    } else if (strcmp(name, "pixel-format") == 0 || strcmp(name, "pixel_format") == 0) {
        strncpy(value_out, s->pixel_format, max_len);
        return ZST_OK;
    } else if (strcmp(name, "num-buffers") == 0 || strcmp(name, "num_buffers") == 0) {
        snprintf(value_out, max_len, "%d", s->num_buffers);
        return ZST_OK;
    } else if (strcmp(name, "loop") == 0) {
        strncpy(value_out, s->loop ? "true" : "false", max_len);
        return ZST_OK;
    } else if (strcmp(name, "use-clock") == 0 || strcmp(name, "do-timestamp") == 0) {
        strncpy(value_out, s->use_clock ? "true" : "false", max_len);
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


static zst_buffer_pool_t*
element_get_pool(zst_element_t* el)
{
    text_source_t* s = el->priv;
    return s->pool;
}

static zst_element_ops_t g_ops = {
    .name = "textsource",
    .open = text_source_open,
    .close = text_source_close,
    .process = text_source_process,
    .get_caps = text_source_get_caps,
    .set_property = text_source_set_property,
    .get_property = text_source_get_property,
    .get_pool = element_get_pool
};

zst_element_t* zst_text_source_create(void)
{
    text_source_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    priv->width = 640;
    priv->height = 480;
    priv->fps = 30;
    strncpy(priv->text, "Hello ZStreamer", sizeof(priv->text) - 1);
    priv->text[sizeof(priv->text) - 1] = '\0';
    priv->font_size = 48;
    strncpy(priv->font_path, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", sizeof(priv->font_path) - 1);
    priv->font_path[sizeof(priv->font_path) - 1] = '\0';
    strncpy(priv->bg_color_str, "#000000", sizeof(priv->bg_color_str) - 1);
    priv->bg_color_str[sizeof(priv->bg_color_str) - 1] = '\0';
    strncpy(priv->text_color_str, "#FFFFFF", sizeof(priv->text_color_str) - 1);
    priv->text_color_str[sizeof(priv->text_color_str) - 1] = '\0';
    strncpy(priv->pixel_format, "YUV420P", sizeof(priv->pixel_format) - 1);
    priv->pixel_format[sizeof(priv->pixel_format) - 1] = '\0';
    priv->num_buffers = -1;
    priv->loop = false;
    priv->use_clock = false;
    priv->x = 10;
    priv->y = 50;
    priv->frame_count = 0;

    priv->bg_r = 0; priv->bg_g = 0; priv->bg_b = 0;
    priv->text_r = 255; priv->text_g = 255; priv->text_b = 255;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    if (strcmp(name, "textsource") == 0) {
        return zst_text_source_create();
    }
    return NULL;
}

static const zst_property_spec_t g_textsource_properties[] = {
    { "width", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Video width" },
    { "height", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Video height" },
    { "fps", ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30", "Video frame rate" },
    { "text", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "Hello", "Text content to display" },
    { "text-content", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "Hello", "Alias for text" },
    { "font-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Font size in pixels" },
    { "font_size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "24", "Alias for font-size" },
    { "font-path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Path to TrueType font file" },
    { "font_path", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Alias for font-path" },
    { "bg-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "black", "Background color name or #RRGGBB hex" },
    { "background-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "black", "Alias for bg-color" },
    { "text-color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Text color name or #RRGGBB hex" },
    { "color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Alias for text-color" },
    { "text_color", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "white", "Alias for text-color" },
    { "pixel-format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUV420P", "Pixel format" },
    { "pixel_format", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "YUV420P", "Alias for pixel-format" },
    { "num-buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Number of buffers to output before EOF" },
    { "num_buffers", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Alias for num-buffers" },
    { "loop", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Loop the source input" },
    { "use-clock", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Use pipeline clock" },
    { "do-timestamp", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Alias for use-clock" },
    { "x", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "X coordinate offset" },
    { "y", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "10", "Y coordinate offset" }
};

static const zst_pad_template_t g_textsource_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_textsource_elements[] = {
    {
        .name = "textsource",
        .long_name = "Text Source",
        .category = "Source/Video",
        .description = "Generates video frames containing text",
        .author = "zstreamer",
        .properties = g_textsource_properties,
        .nb_properties = sizeof(g_textsource_properties) / sizeof(g_textsource_properties[0]),
        .pads = g_textsource_pads,
        .nb_pads = sizeof(g_textsource_pads) / sizeof(g_textsource_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "textsource_plugin",
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
        *nb_elements_out = sizeof(g_textsource_elements) / sizeof(g_textsource_elements[0]);
    }
    return g_textsource_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
