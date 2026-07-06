/*=============================================================================
    gl_comp_sink.c — OpenGL compositor sink element

    Terminal video sink that accepts multiple raw-video sink pads (sink_%u),
    retains the latest frame on each pad, and composites visible inputs into
    a single X11/GLX window.  It gracefully falls back to null-sink mode when
    no display/GL context is available.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <GL/gl.h>
#include <GL/glx.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_gl_comp_sink.h"
#include "zst_buffer.h"
#include "zst_clock.h"
#include "zst_bus.h"
#include "zst_log.h"
#include "zst_pipeline.h"
#include "zst_queue.h"

#define GLCOMP_MAX_NAME 32

/* ─── Shader sources ─────────────────────────────────────────────────── */

static const char* g_comp_vertex_shader_src =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char* g_comp_frag_yuv420p_src =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "uniform mat3 color_matrix;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    float u = texture2D(u_tex, tc).r - 0.5;\n"
    "    float v = texture2D(v_tex, tc).r - 0.5;\n"
    "    vec3 rgb = color_matrix * vec3(y, u, v);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), alpha);\n"
    "}\n";

static const char* g_comp_frag_nv12_src =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D uv_tex;\n"
    "uniform mat3 color_matrix;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    vec2 uv = texture2D(uv_tex, tc).ra;\n"
    "    /* For GL_LUMINANCE_ALPHA, .r is luminance (Y/U), .a is alpha (V) */\n"
    "    vec3 rgb = color_matrix * vec3(y, uv.r - 0.5, uv.g - 0.5);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), alpha);\n"
    "}\n";

static const char* g_comp_frag_rgb_src =
    "#version 120\n"
    "uniform sampler2D rgb_tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    vec3 rgb = texture2D(rgb_tex, gl_TexCoord[0].st).rgb;\n"
    "    gl_FragColor = vec4(rgb, alpha);\n"
    "}\n";

typedef enum {
    GLCOMP_FMT_YUV420P,
    GLCOMP_FMT_NV12,
    GLCOMP_FMT_RGB,
    GLCOMP_FMT_UNKNOWN
} glcomp_fmt_t;

typedef struct gl_comp_sink gl_comp_sink_t;

typedef struct {
    gl_comp_sink_t* parent;
    zst_pad_t* pad;
    char name[GLCOMP_MAX_NAME];

    int x;
    int y;
    uint32_t width;
    uint32_t height;
    int z_order;
    double alpha;
    int visible;
    char scaling[16];
    uint32_t border_width;
    float border_color[4];

    zst_queue_t* queue;
    zst_buffer_t* latest;
    uint64_t frame_count;
    int eos;

    GLuint tex_y;
    GLuint tex_u;
    GLuint tex_v;
    GLuint tex_uv;
    GLuint tex_rgb;
    uint32_t tex_width;
    uint32_t tex_height;
    bool textures_valid;
} gl_comp_input_t;

typedef enum {
    CAPTURE_IDLE,
    CAPTURE_REQUESTED,
    CAPTURE_DONE
} capture_state_t;

struct gl_comp_sink {
    char window_title[256];
    uint32_t canvas_width;
    uint32_t canvas_height;
    float bg[4];
    int fullscreen;
    int vsync;
    int null_mode;
    int window_open;

    int64_t max_lateness;
    double display_rate;

    Display* x_display;
    Window x_window;
    Atom wm_delete_message;
    int x_screen;
    GLXContext gl_context;
    Bool double_buffered;
    XVisualInfo* visual_info;
    Colormap colormap;

    GLuint program_yuv420p;
    GLuint program_nv12;
    GLuint program_rgb;
    GLint uniform_y_tex;
    GLint uniform_u_tex;
    GLint uniform_v_tex;
    GLint uniform_uv_tex;
    GLint uniform_rgb_tex;
    GLint uniform_color_matrix;
    GLint uniform_alpha;

    gl_comp_input_t** inputs;
    uint32_t nb_inputs;
    uint32_t next_pad_index;
    uint64_t composite_count;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t worker_thread;
    bool running;

    /* GL readiness flags: the GL context is created on the worker thread. */
    bool gl_ready;
    bool gl_init_error;

    /* Capture request: the worker thread reads pixels on our behalf
     * since the GL context is bound to that thread. */
    uint8_t* capture_buf;
    uint32_t capture_w;
    uint32_t capture_h;
    volatile capture_state_t capture_state;
    pthread_cond_t capture_cond;
};

/* ─── GL helpers ─────────────────────────────────────────────────────── */

static GLuint
comp_compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        ZST_LOG_ERROR("glcompsink", "shader compilation error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint
comp_link_program(GLuint vs, GLuint fs)
{
    GLuint prog = glCreateProgram();
    if (!prog) return 0;
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        ZST_LOG_ERROR("glcompsink", "program link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static int
comp_build_shaders(gl_comp_sink_t* s)
{
    GLuint vs = comp_compile_shader(GL_VERTEX_SHADER, g_comp_vertex_shader_src);
    if (!vs) return -1;

    GLuint fs = comp_compile_shader(GL_FRAGMENT_SHADER, g_comp_frag_yuv420p_src);
    if (fs) { s->program_yuv420p = comp_link_program(vs, fs); glDeleteShader(fs); }
    fs = comp_compile_shader(GL_FRAGMENT_SHADER, g_comp_frag_nv12_src);
    if (fs) { s->program_nv12 = comp_link_program(vs, fs); glDeleteShader(fs); }
    fs = comp_compile_shader(GL_FRAGMENT_SHADER, g_comp_frag_rgb_src);
    if (fs) { s->program_rgb = comp_link_program(vs, fs); glDeleteShader(fs); }
    glDeleteShader(vs);

    return (s->program_yuv420p || s->program_nv12 || s->program_rgb) ? 0 : -1;
}

static void
comp_get_uniforms(gl_comp_sink_t* s, GLuint prog)
{
    s->uniform_y_tex = glGetUniformLocation(prog, "y_tex");
    s->uniform_u_tex = glGetUniformLocation(prog, "u_tex");
    s->uniform_v_tex = glGetUniformLocation(prog, "v_tex");
    s->uniform_uv_tex = glGetUniformLocation(prog, "uv_tex");
    s->uniform_rgb_tex = glGetUniformLocation(prog, "rgb_tex");
    s->uniform_color_matrix = glGetUniformLocation(prog, "color_matrix");
    s->uniform_alpha = glGetUniformLocation(prog, "alpha");
}

static void
comp_set_common_uniforms(gl_comp_sink_t* s, float alpha)
{
    /* Column-major BT.601 full-range YUV→RGB matrix for GLSL mat3.
     * GLSL mat3 is column-major: mat[col][row].
     * [ 1.0   0.0      1.402 ] [ Y ]
     * [ 1.0  -0.34414 -0.7141] [ U ]
     * [ 1.0   1.772    0.0    ] [ V ]
     */
    const float mat[9] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.344f, 1.772f,
        1.402f, -0.714f, 0.0f
    };
    if (s->uniform_color_matrix >= 0) glUniformMatrix3fv(s->uniform_color_matrix, 1, GL_FALSE, mat);
    if (s->uniform_alpha >= 0) glUniform1f(s->uniform_alpha, alpha);
}

static void
comp_delete_input_textures(gl_comp_input_t* in)
{
    if (!in || !in->textures_valid) return;
    glDeleteTextures(1, &in->tex_y);
    glDeleteTextures(1, &in->tex_u);
    glDeleteTextures(1, &in->tex_v);
    glDeleteTextures(1, &in->tex_uv);
    glDeleteTextures(1, &in->tex_rgb);
    memset(&in->tex_y, 0, sizeof(GLuint) * 5);
    in->textures_valid = false;
}

static int
comp_create_input_textures(gl_comp_input_t* in, uint32_t width, uint32_t height)
{
    if (in->textures_valid && in->tex_width == width && in->tex_height == height) return 0;
    comp_delete_input_textures(in);

    in->tex_width = width;
    in->tex_height = height;
    glGenTextures(1, &in->tex_y);
    glGenTextures(1, &in->tex_u);
    glGenTextures(1, &in->tex_v);
    glGenTextures(1, &in->tex_uv);
    glGenTextures(1, &in->tex_rgb);

#define INIT_TEX(tex, internal, w, h, fmt) do { \
    glBindTexture(GL_TEXTURE_2D, (tex)); \
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); \
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); \
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); \
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); \
    glTexImage2D(GL_TEXTURE_2D, 0, (internal), (GLsizei)(w), (GLsizei)(h), 0, (fmt), GL_UNSIGNED_BYTE, NULL); \
} while (0)

    INIT_TEX(in->tex_y, GL_LUMINANCE, width, height, GL_LUMINANCE);
    INIT_TEX(in->tex_u, GL_LUMINANCE, width / 2, height / 2, GL_LUMINANCE);
    INIT_TEX(in->tex_v, GL_LUMINANCE, width / 2, height / 2, GL_LUMINANCE);
    INIT_TEX(in->tex_uv, GL_LUMINANCE_ALPHA, width / 2, height / 2, GL_LUMINANCE_ALPHA);
    INIT_TEX(in->tex_rgb, GL_RGB, width, height, GL_RGB);
#undef INIT_TEX

    in->textures_valid = true;
    return 0;
}

static glcomp_fmt_t
comp_detect_format(const zst_video_frame_t* frame)
{
    if (!frame || !frame->plane[0]) return GLCOMP_FMT_UNKNOWN;
    if (frame->format == 0 && frame->plane[1] && frame->plane[2] && !frame->plane[3]) return GLCOMP_FMT_YUV420P;
    if ((frame->format == 1 || frame->format == 12) && frame->plane[1] && !frame->plane[2]) return GLCOMP_FMT_NV12;
    if ((frame->format == 2 || frame->format == 24) && !frame->plane[1]) return GLCOMP_FMT_RGB;
    if (frame->plane[1] && frame->plane[2] && !frame->plane[3]) return GLCOMP_FMT_YUV420P;
    if (frame->plane[1] && !frame->plane[2]) return GLCOMP_FMT_NV12;
    if (!frame->plane[1]) return GLCOMP_FMT_RGB;
    return GLCOMP_FMT_UNKNOWN;
}

static void
comp_pixel_rect_to_ndc(uint32_t canvas_w, uint32_t canvas_h,
                       float x, float y, float w, float h,
                       float* l, float* r, float* b, float* t)
{
    *l = -1.0f + 2.0f * x / (float)canvas_w;
    *r = -1.0f + 2.0f * (x + w) / (float)canvas_w;
    *t =  1.0f - 2.0f * y / (float)canvas_h;
    *b =  1.0f - 2.0f * (y + h) / (float)canvas_h;
}

static void
comp_draw_quad(gl_comp_sink_t* s, const gl_comp_input_t* in,
               const zst_video_frame_t* frame)
{
    float x = (float)in->x;
    float y = (float)in->y;
    float w = (float)(in->width ? in->width : s->canvas_width);
    float h = (float)(in->height ? in->height : s->canvas_height);
    float tx0 = 0.0f, ty0 = 0.0f, tx1 = 1.0f, ty1 = 1.0f;

    if (frame->width > 0 && frame->height > 0 && w > 0 && h > 0) {
        float img_aspect = (float)frame->width / (float)frame->height;
        float dst_aspect = w / h;
        if (strcmp(in->scaling, "fit") == 0) {
            if (img_aspect > dst_aspect) {
                float nh = w / img_aspect;
                y += (h - nh) * 0.5f;
                h = nh;
            } else {
                float nw = h * img_aspect;
                x += (w - nw) * 0.5f;
                w = nw;
            }
        } else if (strcmp(in->scaling, "crop") == 0) {
            if (img_aspect > dst_aspect) {
                float vis = dst_aspect / img_aspect;
                tx0 = (1.0f - vis) * 0.5f;
                tx1 = 1.0f - tx0;
            } else {
                float vis = img_aspect / dst_aspect;
                ty0 = (1.0f - vis) * 0.5f;
                ty1 = 1.0f - ty0;
            }
        }
    }

    float l, r, b, t;
    comp_pixel_rect_to_ndc(s->canvas_width, s->canvas_height, x, y, w, h, &l, &r, &b, &t);

    if (in->border_width > 0) {
        glUseProgram(0);
        glColor4fv(in->border_color);
        float bl, br, bb, bt;
        float bw = (float)in->border_width;
        comp_pixel_rect_to_ndc(s->canvas_width, s->canvas_height, x - bw, y - bw, w + 2*bw, h + 2*bw, &bl, &br, &bb, &bt);
        glBegin(GL_QUADS);
        glVertex2f(bl, bb); glVertex2f(br, bb); glVertex2f(br, bt); glVertex2f(bl, bt);
        glEnd();
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }

    /* Restore program if we were using one (caller should re-glUseProgram) */
    glcomp_fmt_t fmt = comp_detect_format(frame);
    if (fmt == GLCOMP_FMT_YUV420P && s->program_yuv420p) glUseProgram(s->program_yuv420p);
    else if (fmt == GLCOMP_FMT_NV12 && s->program_nv12) glUseProgram(s->program_nv12);
    else if (fmt == GLCOMP_FMT_RGB && s->program_rgb) glUseProgram(s->program_rgb);

    glBegin(GL_QUADS);
    glTexCoord2f(tx0, ty1); glVertex2f(l, b);
    glTexCoord2f(tx1, ty1); glVertex2f(r, b);
    glTexCoord2f(tx1, ty0); glVertex2f(r, t);
    glTexCoord2f(tx0, ty0); glVertex2f(l, t);
    glEnd();
}

static void
comp_upload_and_draw(gl_comp_sink_t* s, gl_comp_input_t* in)
{
    if (!in->latest || !in->visible) return;
    zst_video_frame_t* frame = (zst_video_frame_t*)in->latest->payload;
    if (!frame || frame->width == 0 || frame->height == 0) return;
    if (comp_create_input_textures(in, frame->width, frame->height) != 0) return;

    glcomp_fmt_t fmt = comp_detect_format(frame);
    switch (fmt) {
        case GLCOMP_FMT_YUV420P:
            if (!s->program_yuv420p) return;
            glUseProgram(s->program_yuv420p);
            comp_get_uniforms(s, s->program_yuv420p);
            comp_set_common_uniforms(s, (float)in->alpha);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, in->tex_y);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width, in->tex_height,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, in->tex_u);
            if (frame->stride[1] > 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width / 2, in->tex_height / 2,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, in->tex_v);
            if (frame->stride[2] > 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[2]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width / 2, in->tex_height / 2,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[2]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            if (s->uniform_y_tex >= 0) glUniform1i(s->uniform_y_tex, 0);
            if (s->uniform_u_tex >= 0) glUniform1i(s->uniform_u_tex, 1);
            if (s->uniform_v_tex >= 0) glUniform1i(s->uniform_v_tex, 2);
            break;

        case GLCOMP_FMT_NV12:
            if (!s->program_nv12) return;
            glUseProgram(s->program_nv12);
            comp_get_uniforms(s, s->program_nv12);
            comp_set_common_uniforms(s, (float)in->alpha);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, in->tex_y);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width, in->tex_height,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, in->tex_uv);
            if (frame->stride[1] > 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[1] / 2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width / 2, in->tex_height / 2,
                            GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, frame->plane[1]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            if (s->uniform_y_tex >= 0) glUniform1i(s->uniform_y_tex, 0);
            if (s->uniform_uv_tex >= 0) glUniform1i(s->uniform_uv_tex, 1);
            break;

        case GLCOMP_FMT_RGB:
            if (!s->program_rgb) return;
            glUseProgram(s->program_rgb);
            comp_get_uniforms(s, s->program_rgb);
            if (s->uniform_alpha >= 0) glUniform1f(s->uniform_alpha, (float)in->alpha);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, in->tex_rgb);
            if (frame->stride[0] > 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0] / 3);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->tex_width, in->tex_height,
                            GL_RGB, GL_UNSIGNED_BYTE, frame->plane[0]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (s->uniform_rgb_tex >= 0) glUniform1i(s->uniform_rgb_tex, 0);
            break;

        default:
            ZST_LOG_WARN("glcompsink", "unsupported frame format on %s", in->name);
            return;
    }

    comp_draw_quad(s, in, frame);
}

static int
comp_input_cmp(const void* a, const void* b)
{
    const gl_comp_input_t* ia = *(const gl_comp_input_t* const*)a;
    const gl_comp_input_t* ib = *(const gl_comp_input_t* const*)b;
    if (ia->z_order != ib->z_order) return ia->z_order - ib->z_order;
    return strcmp(ia->name, ib->name);
}

static void
comp_apply_fullscreen(gl_comp_sink_t* s, int fullscreen)
{
    if (!s->x_display || !s->x_window) return;
    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type = ClientMessage;
    xev.xclient.window = s->x_window;
    xev.xclient.message_type = XInternAtom(s->x_display, "_NET_WM_STATE", False);
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = fullscreen ? 1 : 0; /* 1=_NET_WM_STATE_ADD, 0=_NET_WM_STATE_REMOVE */
    xev.xclient.data.l[1] = XInternAtom(s->x_display, "_NET_WM_STATE_FULLSCREEN", False);
    xev.xclient.data.l[2] = 0;
    XSendEvent(s->x_display, DefaultRootWindow(s->x_display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
}

static int
comp_check_events(gl_comp_sink_t* s, zst_element_t* el)
{
    if (!s->x_display || !s->window_open) return 0;
    while (XPending(s->x_display)) {
        XEvent ev;
        XNextEvent(s->x_display, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == s->wm_delete_message) {
            return 1;
        }
        if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);

            if (el && el->bus) {
                char key_str[16] = {0};
                XLookupString(&ev.xkey, key_str, sizeof(key_str) - 1, NULL, NULL);
                zst_event_t* kp_ev = zst_event_new_key_press(el, (uint32_t)ks, (uint32_t)ev.xkey.keycode, key_str);
                if (kp_ev) {
                    zst_bus_post(el->bus, kp_ev);
                }
            }
        }
        if (ev.type == ButtonPress || ev.type == ButtonRelease) {
            if (el && el->bus) {
                zst_event_t* m_ev = zst_event_new_mouse_button(
                    el,
                    (uint32_t)ev.xbutton.button,
                    (ev.type == ButtonPress) ? 1 : 0,
                    ev.xbutton.x,
                    ev.xbutton.y
                );
                if (m_ev) {
                    zst_bus_post(el->bus, m_ev);
                }
            }
        }
        if (ev.type == MotionNotify) {
            if (el && el->bus) {
                zst_event_t* m_ev = zst_event_new_mouse_motion(
                    el,
                    ev.xmotion.x,
                    ev.xmotion.y
                );
                if (m_ev) {
                    zst_bus_post(el->bus, m_ev);
                }
            }
        }
        if (ev.type == ConfigureNotify) {
            s->canvas_width = (uint32_t)ev.xconfigure.width;
            s->canvas_height = (uint32_t)ev.xconfigure.height;
        }
    }
    return 0;
}

static zst_result_t
comp_render_locked(gl_comp_sink_t* s, zst_element_t* el)
{
    if (s->null_mode || !s->gl_context) {
        s->composite_count++;
        return ZST_OK;
    }
    if (comp_check_events(s, el)) return ZST_EOF;

    glXMakeCurrent(s->x_display, s->x_window, s->gl_context);
    glViewport(0, 0, (GLsizei)s->canvas_width, (GLsizei)s->canvas_height);
    glClearColor(s->bg[0], s->bg[1], s->bg[2], s->bg[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    if (s->nb_inputs > 0) {
        gl_comp_input_t** sorted = calloc(s->nb_inputs, sizeof(*sorted));
        if (sorted) {
            memcpy(sorted, s->inputs, s->nb_inputs * sizeof(*sorted));
            qsort(sorted, s->nb_inputs, sizeof(*sorted), comp_input_cmp);

            zst_time_t now = 0;
            zst_element_t* el_ptr = sorted[0]->pad->parent;
            if (el_ptr && el_ptr->clock) now = zst_clock_get_time(el_ptr->clock);

            for (uint32_t i = 0; i < s->nb_inputs; i++) {
                gl_comp_input_t* in = sorted[i];

        /* If input was released, it's already removed from the list but check for safety */
        if (!in) continue;

                zst_buffer_t* next = NULL;
                while (zst_queue_pop(in->queue, &next, 0) == ZST_OK) {
                    /* If we have a clock, we should ideally check if 'next' is too early.
                     * But zst_queue doesn't support peeking.
                     * For now, we just use it as 'latest'.
                     */
                    if (in->latest) zst_buffer_unref(in->latest);
                    in->latest = next;
                }
                comp_upload_and_draw(s, in);
            }
            free(sorted);
        }
    }

    if (s->double_buffered) glXSwapBuffers(s->x_display, s->x_window);
    else glFlush();
    s->composite_count++;
    if (s->composite_count % 30 == 0) {
        ZST_LOG_INFO("glcompsink", "rendered %llu compositor frames", (unsigned long long)s->composite_count);
    }
    return comp_check_events(s, el) ? ZST_EOF : ZST_OK;
}

static int
comp_worker_init_gl(gl_comp_sink_t* s)
{
    /* Create the GL context on the worker thread so it stays thread-local */
    s->gl_context = glXCreateContext(s->x_display, s->visual_info, None, True);
    if (!s->gl_context) {
        ZST_LOG_ERROR("glcompsink", "glXCreateContext failed on worker thread");
        glXMakeCurrent(s->x_display, None, NULL);
        return -1;
    }
    if (!glXMakeCurrent(s->x_display, s->x_window, s->gl_context)) {
        ZST_LOG_ERROR("glcompsink", "glXMakeCurrent failed on worker thread");
        glXDestroyContext(s->x_display, s->gl_context);
        s->gl_context = NULL;
        return -1;
    }

    int dbuf = 0;
    glXGetConfig(s->x_display, s->visual_info, GLX_DOUBLEBUFFER, &dbuf);
    s->double_buffered = dbuf ? True : False;

    if (comp_build_shaders(s) != 0) {
        ZST_LOG_ERROR("glcompsink", "shader compilation failed on worker thread");
        glXDestroyContext(s->x_display, s->gl_context);
        s->gl_context = NULL;
        return -1;
    }

    if (s->vsync) {
        typedef void (*SwapIntervalFunc)(Display*, GLXDrawable, int);
        SwapIntervalFunc swapInterval = (SwapIntervalFunc)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
        if (swapInterval) swapInterval(s->x_display, glXGetCurrentDrawable(), 1);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    return 0;
}

static void*
glcomp_worker_thread(void* arg)
{
    zst_element_t* el = arg;
    gl_comp_sink_t* s = el->priv;

    ZST_LOG_INFO("glcompsink", "starting compositor worker thread (rate=%.2f fps)", s->display_rate);

    pthread_mutex_lock(&s->lock);

    /* Initialize GL on this thread */
    if (s->x_display && s->x_window && !s->null_mode) {
        if (comp_worker_init_gl(s) != 0) {
            ZST_LOG_ERROR("glcompsink", "worker thread GL init failed — falling back to null mode");
            s->null_mode = 1;
            s->gl_init_error = true;
            pthread_cond_broadcast(&s->cond);
        } else {
            s->gl_ready = true;
            pthread_cond_broadcast(&s->cond);
        }
    } else {
        s->gl_ready = true;
        pthread_cond_broadcast(&s->cond);
    }

    struct timespec next_render;
    clock_gettime(CLOCK_REALTIME, &next_render);

    while (s->running) {
        /* Compute the next render time */
        double interval = 1.0 / s->display_rate;
        long long nsec = next_render.tv_nsec + (long long)(interval * 1000000000.0);
        next_render.tv_sec += nsec / 1000000000LL;
        next_render.tv_nsec = nsec % 1000000000LL;

        /* Wait until it's time to render the next frame, or until signaled.
         * If a capture is pending, skip the timed wait and render immediately. */
        if (s->capture_state != CAPTURE_REQUESTED) {
            while (s->running) {
                int res = pthread_cond_timedwait(&s->cond, &s->lock, &next_render);
                if (res == ETIMEDOUT) break;
                if (res != 0) {
                    ZST_LOG_ERROR("glcompsink", "pthread_cond_timedwait returned %d (next_render=%ld.%09ld)",
                                  res, (long)next_render.tv_sec, next_render.tv_nsec);
                    break;
                }
            }
        }

        if (!s->running) break;

        if (comp_render_locked(s, el) == ZST_EOF) {
            ZST_LOG_INFO("glcompsink", "worker thread detected window close");
            s->null_mode = 1;
            if (el->bus) {
                zst_bus_post(el->bus, zst_event_new_eos(el));
            }
        }

        /* Handle capture request: read back pixels and signal caller */
        if (s->capture_state == CAPTURE_REQUESTED && s->gl_context && !s->null_mode) {
            glReadPixels(0, 0, (GLsizei)s->capture_w, (GLsizei)s->capture_h,
                         GL_RGBA, GL_UNSIGNED_BYTE, s->capture_buf);
            s->capture_state = CAPTURE_DONE;
            pthread_cond_signal(&s->capture_cond);
        } else if (s->capture_state == CAPTURE_REQUESTED) {
            /* Null mode — can't capture, signal done with NULL result */
            s->capture_state = CAPTURE_DONE;
            pthread_cond_signal(&s->capture_cond);
        }
    }

    /* Clean up GL resources on the worker thread before exiting */
    if (s->gl_context && s->x_display) {
        for (uint32_t i = 0; i < s->nb_inputs; i++) comp_delete_input_textures(s->inputs[i]);
        if (s->program_yuv420p) glDeleteProgram(s->program_yuv420p);
        if (s->program_nv12) glDeleteProgram(s->program_nv12);
        if (s->program_rgb) glDeleteProgram(s->program_rgb);
        s->program_yuv420p = s->program_nv12 = s->program_rgb = 0;
        glXMakeCurrent(s->x_display, None, NULL);
        glXDestroyContext(s->x_display, s->gl_context);
        s->gl_context = NULL;
    }

    pthread_mutex_unlock(&s->lock);

    ZST_LOG_INFO("glcompsink", "compositor worker thread exiting");
    return NULL;
}

/* ─── Public capture API ──────────────────────────────────────────────── */

zst_result_t
zst_gl_comp_sink_capture(zst_element_t* el, uint32_t width, uint32_t height,
                          uint8_t* rgba_out)
{
    if (!el || !el->priv || !rgba_out) return ZST_ERROR;
    gl_comp_sink_t* s = el->priv;

    pthread_mutex_lock(&s->lock);

    if (s->null_mode || !s->gl_context || !s->x_display) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    /* Request a capture from the worker thread (which owns the GL context) */
    s->capture_buf = rgba_out;
    s->capture_w = width;
    s->capture_h = height;
    s->capture_state = CAPTURE_REQUESTED;
    pthread_cond_signal(&s->cond);

    /* Wait for the worker thread to complete the capture */
    while (s->capture_state != CAPTURE_DONE) {
        pthread_cond_wait(&s->capture_cond, &s->lock);
    }
    s->capture_state = CAPTURE_IDLE;

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

/* ─── Pad and property helpers ───────────────────────────────────────── */

static gl_comp_input_t*
comp_find_input_by_name(gl_comp_sink_t* s, const char* name)
{
    if (!s || !name) return NULL;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (strcmp(s->inputs[i]->name, name) == 0) return s->inputs[i];
    }
    return NULL;
}

static zst_result_t comp_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf);

static gl_comp_input_t*
comp_add_input_locked(zst_element_t* el, const char* requested_name)
{
    gl_comp_sink_t* s = el->priv;
    char name[GLCOMP_MAX_NAME];
    if (requested_name && requested_name[0]) {
        snprintf(name, sizeof(name), "%s", requested_name);
        if (strncmp(name, "sink_", 5) != 0) return NULL;
        if (comp_find_input_by_name(s, name)) return comp_find_input_by_name(s, name);
    } else {
        do {
            snprintf(name, sizeof(name), "sink_%u", s->next_pad_index++);
        } while (comp_find_input_by_name(s, name));
    }

    gl_comp_input_t* in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    in->parent = s;
    snprintf(in->name, sizeof(in->name), "%s", name);
    in->width = s->canvas_width;
    in->height = s->canvas_height;
    in->alpha = 1.0;
    in->visible = 1;
    snprintf(in->scaling, sizeof(in->scaling), "fit");
    in->border_width = 0;
    in->border_color[0] = 1.0f; in->border_color[1] = 1.0f; in->border_color[2] = 1.0f; in->border_color[3] = 1.0f;

    zst_queue_config_t qcfg = {
        .mode = ZST_QUEUE_SYNC,
        .max_buffers = 10
    };
    in->queue = zst_queue_create(&qcfg);
    if (!in->queue) { free(in); return NULL; }

    zst_pad_t* pad = zst_pad_create(in->name, ZST_PAD_SINK);
    if (!pad) { free(in); return NULL; }
    pad->push = comp_sink_pad_push;
    pad->priv = in;
    in->pad = pad;

    gl_comp_input_t** inputs = realloc(s->inputs, (s->nb_inputs + 1) * sizeof(*s->inputs));
    if (!inputs) {
        zst_pad_destroy(pad);
        free(in);
        return NULL;
    }
    s->inputs = inputs;
    s->inputs[s->nb_inputs++] = in;

    if (zst_element_add_pad(el, pad) != ZST_OK) {
        s->nb_inputs--;
        zst_queue_destroy(in->queue);
        zst_pad_destroy(pad);
        free(in);
        return NULL;
    }
    return in;
}

static void
comp_input_free(gl_comp_input_t* in)
{
    if (!in) return;
    /* textures should be deleted in the worker thread context if possible,
     * but for now they are leaked if we don't handle thread context here.
     * Since pad removal is rare, we might need a better strategy.
     * However, the worker thread is usually running. */
    if (in->latest) zst_buffer_unref(in->latest);
    if (in->queue) zst_queue_destroy(in->queue);
    free(in);
}

zst_result_t
zst_gl_comp_sink_release_pad(zst_element_t* el, zst_pad_t* pad)
{
    if (!el || !pad || !el->priv) return ZST_ERROR;
    gl_comp_sink_t* s = el->priv;
    gl_comp_input_t* in = pad->priv;

    pthread_mutex_lock(&s->lock);

    /* Find input in array */
    int idx = -1;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i] == in) {
            idx = (int)i;
            break;
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&s->lock);
        return ZST_ERROR;
    }

    /* Remove from element and array */
    zst_element_remove_pad(el, pad);
    memmove(&s->inputs[idx], &s->inputs[idx + 1], (s->nb_inputs - idx - 1) * sizeof(*s->inputs));
    s->nb_inputs--;

    comp_input_free(in);

    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

zst_pad_t*
zst_gl_comp_sink_request_pad(zst_element_t* el, const char* name)
{
    if (!el || !el->priv) return NULL;
    gl_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    gl_comp_input_t* in = comp_add_input_locked(el, name);
    pthread_mutex_unlock(&s->lock);
    return in ? in->pad : NULL;
}

static void
comp_auto_layout_locked(gl_comp_sink_t* s)
{
    if (!s || s->nb_inputs == 0) return;
    uint32_t cols = 1;
    while (cols * cols < s->nb_inputs) cols++;
    uint32_t rows = (s->nb_inputs + cols - 1) / cols;
    uint32_t cell_w = s->canvas_width / cols;
    uint32_t cell_h = s->canvas_height / rows;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        s->inputs[i]->x = (int)((i % cols) * cell_w);
        s->inputs[i]->y = (int)((i / cols) * cell_h);
        s->inputs[i]->width = cell_w;
        s->inputs[i]->height = cell_h;
        s->inputs[i]->z_order = (int)i;
    }
}

static int
parse_bool(const char* value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0;
}

static zst_result_t
parse_background(const char* value, float out[4])
{
    if (!value) return ZST_ERROR;
    if (value[0] == '#') {
        unsigned int r = 0, g = 0, b = 0, a = 255;
        if (strlen(value) == 7 && sscanf(value + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = 1.0f;
            return ZST_OK;
        }
        if (strlen(value) == 9 && sscanf(value + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = a / 255.0f;
            return ZST_OK;
        }
    }
    double r = 0, g = 0, b = 0, a = 1;
    int n = sscanf(value, "%lf,%lf,%lf,%lf", &r, &g, &b, &a);
    if (n == 3 || n == 4) {
        out[0] = (float)r; out[1] = (float)g; out[2] = (float)b; out[3] = (float)a;
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
comp_set_pad_property(gl_comp_input_t* in, const char* prop, const char* value)
{
    if (!in || !prop || !value) return ZST_ERROR;
    if (strcmp(prop, "x") == 0) {
        int val = atoi(value);
        if (val < 0) val = 0;
        if (val > (int)in->parent->canvas_width) val = (int)in->parent->canvas_width;
        in->x = val;
        return ZST_OK;
    }
    if (strcmp(prop, "y") == 0) {
        int val = atoi(value);
        if (val < 0) val = 0;
        if (val > (int)in->parent->canvas_height) val = (int)in->parent->canvas_height;
        in->y = val;
        return ZST_OK;
    }
    if (strcmp(prop, "width") == 0) {
        uint32_t val = (uint32_t)strtoul(value, NULL, 10);
        if (val > in->parent->canvas_width) val = in->parent->canvas_width;
        in->width = val;
        return ZST_OK;
    }
    if (strcmp(prop, "height") == 0) {
        uint32_t val = (uint32_t)strtoul(value, NULL, 10);
        if (val > in->parent->canvas_height) val = in->parent->canvas_height;
        in->height = val;
        return ZST_OK;
    }
    if (strcmp(prop, "z-order") == 0 || strcmp(prop, "z_order") == 0 || strcmp(prop, "z") == 0) {
        in->z_order = atoi(value); return ZST_OK;
    }
    if (strcmp(prop, "alpha") == 0) {
        in->alpha = atof(value);
        if (in->alpha < 0.0) in->alpha = 0.0;
        if (in->alpha > 1.0) in->alpha = 1.0;
        return ZST_OK;
    }
    if (strcmp(prop, "visible") == 0) { in->visible = parse_bool(value); return ZST_OK; }
    if (strcmp(prop, "scaling") == 0) {
        if (strcmp(value, "fit") && strcmp(value, "stretch") && strcmp(value, "crop")) return ZST_ERROR;
        snprintf(in->scaling, sizeof(in->scaling), "%s", value);
        return ZST_OK;
    }
    if (strcmp(prop, "border-width") == 0) {
        in->border_width = (uint32_t)strtoul(value, NULL, 10);
        return ZST_OK;
    }
    if (strcmp(prop, "border-color") == 0) {
        return parse_background(value, in->border_color);
    }
    return ZST_ERROR;
}

static zst_result_t
comp_get_pad_property(gl_comp_input_t* in, const char* prop, char* out, size_t max)
{
    if (!in || !prop || !out || max == 0) return ZST_ERROR;
    if (strcmp(prop, "x") == 0) { snprintf(out, max, "%d", in->x); return ZST_OK; }
    if (strcmp(prop, "y") == 0) { snprintf(out, max, "%d", in->y); return ZST_OK; }
    if (strcmp(prop, "width") == 0) { snprintf(out, max, "%u", in->width); return ZST_OK; }
    if (strcmp(prop, "height") == 0) { snprintf(out, max, "%u", in->height); return ZST_OK; }
    if (strcmp(prop, "z-order") == 0 || strcmp(prop, "z_order") == 0 || strcmp(prop, "z") == 0) { snprintf(out, max, "%d", in->z_order); return ZST_OK; }
    if (strcmp(prop, "alpha") == 0) { snprintf(out, max, "%.17g", in->alpha); return ZST_OK; }
    if (strcmp(prop, "visible") == 0) { snprintf(out, max, "%s", in->visible ? "true" : "false"); return ZST_OK; }
    if (strcmp(prop, "scaling") == 0) { snprintf(out, max, "%s", in->scaling); return ZST_OK; }
    if (strcmp(prop, "border-width") == 0) { snprintf(out, max, "%u", in->border_width); return ZST_OK; }
    if (strcmp(prop, "border-color") == 0) {
        snprintf(out, max, "#%02x%02x%02x%02x",
                 (unsigned)(in->border_color[0] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[1] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[2] * 255.0f + 0.5f),
                 (unsigned)(in->border_color[3] * 255.0f + 0.5f));
        return ZST_OK;
    }
    if (strcmp(prop, "frame-count") == 0 || strcmp(prop, "frame_count") == 0) { snprintf(out, max, "%llu", (unsigned long long)in->frame_count); return ZST_OK; }
    return ZST_ERROR;
}

static int
split_pad_prop(const char* name, char* pad, size_t pad_len, const char** prop_out)
{
    const char* sep = strstr(name, "::");
    if (!sep) sep = strchr(name, '.');
    if (!sep) return 0;
    size_t n = (size_t)(sep - name);
    if (n == 0 || n >= pad_len) return 0;
    memcpy(pad, name, n);
    pad[n] = '\0';
    *prop_out = sep + ((sep[0] == ':' && sep[1] == ':') ? 2 : 1);
    return 1;
}

static int
comp_all_inputs_eos(gl_comp_sink_t* s)
{
    if (!s || s->nb_inputs == 0) return 0;
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        if (!s->inputs[i]->eos) return 0;
    }
    return 1;
}

static zst_result_t
comp_sink_pad_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf || !pad->priv) return ZST_ERROR;
    gl_comp_input_t* in = pad->priv;
    gl_comp_sink_t* s = in->parent;
    zst_element_t* el = pad->parent;
    zst_result_t ret = ZST_OK;

    if (buf->flags & ZST_BUFFER_FLAG_DROP) {
        return ZST_OK;
    }

    if (el && el->clock && buf->pts > 0 && !(buf->flags & ZST_BUFFER_FLAG_EOS)) {
        zst_time_t current = zst_clock_get_time(el->clock);
        if (current > buf->pts && (current - buf->pts) > (zst_time_t)s->max_lateness) {
            if (current - buf->pts < 5000000000ULL) { /* 5s safeguard */
                buf->flags |= ZST_BUFFER_FLAG_DROP;
                if (el->bus) {
                    zst_event_t* qos_ev = zst_event_new_warning(el, ZST_ERROR, "QoS: Frame dropped (too late)");
                    zst_bus_post(el->bus, qos_ev);
                }
                return ZST_OK;
            }
        }
    }

    pthread_mutex_lock(&s->lock);
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        in->eos = 1;
        ret = comp_all_inputs_eos(s) ? ZST_EOF : ZST_OK;
        pthread_cond_signal(&s->cond);
    } else {
        if (zst_queue_push(in->queue, buf, 0) == ZST_OK) {
            in->frame_count++;
            pthread_cond_signal(&s->cond);
        }
        ret = ZST_OK;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/* ─── Element lifecycle ───────────────────────────────────────────────── */

static void
comp_cleanup_display(gl_comp_sink_t* s)
{
    /* GL context cleanup is handled by the worker thread on exit.
     * If the worker thread already cleaned up (e.g. gl_init_error before
     * the worker reached its cleanup block), s->gl_context is NULL and
     * no GL operations are performed here. */
    if (s->x_display && s->x_window) {
        XDestroyWindow(s->x_display, s->x_window);
        s->x_window = 0;
    }
    s->window_open = 0;
    if (s->colormap) {
        XFreeColormap(s->x_display, s->colormap);
        s->colormap = 0;
    }
    if (s->visual_info) {
        XFree(s->visual_info);
        s->visual_info = NULL;
    }
    if (s->x_display) {
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
    }
}

static zst_result_t
glcomp_open(zst_element_t* el)
{
    gl_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    s->null_mode = 0;
    s->window_open = 0;
    s->composite_count = 0;

    static pthread_mutex_t xinit_lock = PTHREAD_MUTEX_INITIALIZER;
    static int xinit_done = 0;
    pthread_mutex_lock(&xinit_lock);
    if (!xinit_done) { XInitThreads(); xinit_done = 1; }
    pthread_mutex_unlock(&xinit_lock);

    s->x_display = XOpenDisplay(NULL);
    if (!s->x_display) {
        ZST_LOG_WARN("glcompsink", "no X11 display available — running in null-sink mode");
        s->null_mode = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    s->x_screen = DefaultScreen(s->x_display);
    if (s->canvas_width == 0) s->canvas_width = 640;
    if (s->canvas_height == 0) s->canvas_height = 480;

    int glx_attrs[] = {
        GLX_RGBA, GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 16,
        None
    };
    s->visual_info = glXChooseVisual(s->x_display, s->x_screen, glx_attrs);
    if (!s->visual_info) {
        ZST_LOG_ERROR("glcompsink", "glXChooseVisual failed — falling back to null mode");
        comp_cleanup_display(s);
        s->null_mode = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    Window root = RootWindow(s->x_display, s->x_screen);
    s->colormap = XCreateColormap(s->x_display, root, s->visual_info->visual, AllocNone);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = s->colormap;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | ClientMessage;
    swa.background_pixel = BlackPixel(s->x_display, s->x_screen);
    swa.border_pixel = 0;

    s->x_window = XCreateWindow(s->x_display, root, 0, 0,
                                s->canvas_width, s->canvas_height, 0,
                                s->visual_info->depth, InputOutput,
                                s->visual_info->visual,
                                CWColormap | CWEventMask | CWBackPixel | CWBorderPixel,
                                &swa);
    if (!s->x_window) {
        ZST_LOG_ERROR("glcompsink", "failed to create X11 window — falling back to null mode");
        comp_cleanup_display(s);
        s->null_mode = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    XStoreName(s->x_display, s->x_window, s->window_title);
    XSetIconName(s->x_display, s->x_window, s->window_title);
    s->wm_delete_message = XInternAtom(s->x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->x_display, s->x_window, &s->wm_delete_message, 1);
    XMapWindow(s->x_display, s->x_window);
    if (s->fullscreen) comp_apply_fullscreen(s, 1);
    XFlush(s->x_display);

    /* GL context is created on the worker thread. Start the worker now. */
    s->running = true;
    pthread_create(&s->worker_thread, NULL, glcomp_worker_thread, el);

    /* Wait for the worker thread to finish GL init (or fail)
     * pthread_cond_timedwait requires an absolute time. */
    while (!s->gl_ready && !s->gl_init_error) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5; /* 5-second timeout */
        int res = pthread_cond_timedwait(&s->cond, &s->lock, &ts);
        if (res == ETIMEDOUT) {
            ZST_LOG_ERROR("glcompsink", "timed out (5s) waiting for worker thread GL init");
            break;
        }
    }

    if (s->gl_init_error || !s->gl_ready) {
        ZST_LOG_WARN("glcompsink", "GL initialization failed on worker thread — falling back to null mode");
        comp_cleanup_display(s);
        s->null_mode = 1;
        pthread_mutex_unlock(&s->lock);
        return ZST_OK;
    }

    s->window_open = 1;

    ZST_LOG_INFO("glcompsink", "opened compositor window '%s' (%ux%u, inputs=%u)",
                 s->window_title, s->canvas_width, s->canvas_height, s->nb_inputs);
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
glcomp_close(zst_element_t* el)
{
    gl_comp_sink_t* s = el->priv;
    pthread_mutex_lock(&s->lock);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (s->worker_thread) {
        pthread_join(s->worker_thread, NULL);
        s->worker_thread = 0;
    }

    pthread_mutex_lock(&s->lock);
    comp_cleanup_display(s);
    for (uint32_t i = 0; i < s->nb_inputs; i++) {
        comp_input_free(s->inputs[i]);
    }
    free(s->inputs);
    s->inputs = NULL;
    s->nb_inputs = 0;
    s->null_mode = 0;
    pthread_mutex_unlock(&s->lock);
    return ZST_OK;
}

static zst_result_t
glcomp_process(zst_element_t* el, zst_buffer_t* inbuf, zst_buffer_t** out)
{
    (void)out;
    if (!inbuf) return ZST_AGAIN;
    zst_pad_t* pad = zst_element_get_pad(el, "sink_0");
    if (!pad || !pad->push) return ZST_ERROR;
    return pad->push(pad, inbuf);
}

static zst_result_t
glcomp_set_property(zst_element_t* el, const char* name, const char* value)
{
    gl_comp_sink_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    char pad_name[GLCOMP_MAX_NAME];
    const char* prop = NULL;
    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        gl_comp_input_t* in = comp_find_input_by_name(s, pad_name);
        zst_result_t r = in ? comp_set_pad_property(in, prop, value) : ZST_ERROR;
        pthread_mutex_unlock(&s->lock);
        return r;
    }

    zst_result_t ret = ZST_OK;
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(s->window_title, sizeof(s->window_title), "%s", value);
        if (s->x_display && s->window_open) XStoreName(s->x_display, s->x_window, s->window_title);
    } else if (strcmp(name, "canvas-width") == 0 || strcmp(name, "width") == 0) {
        s->canvas_width = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(name, "canvas-height") == 0 || strcmp(name, "height") == 0) {
        s->canvas_height = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcmp(name, "background-color") == 0 || strcmp(name, "background") == 0) {
        ret = parse_background(value, s->bg);
    } else if (strcmp(name, "fullscreen") == 0) {
        s->fullscreen = parse_bool(value);
    } else if (strcmp(name, "vsync") == 0) {
        s->vsync = parse_bool(value);
    } else if (strcmp(name, "display-rate") == 0 || strcmp(name, "rate") == 0) {
        s->display_rate = atof(value);
        if (s->display_rate <= 0.0) s->display_rate = 30.0;
    } else if (strcmp(name, "max-lateness") == 0 || strcmp(name, "max_lateness") == 0) {
        s->max_lateness = (int64_t)atoll(value);
    } else if (strcmp(name, "input-count") == 0 || strcmp(name, "inputs") == 0) {
        uint32_t n = (uint32_t)strtoul(value, NULL, 10);
        while (ret == ZST_OK && s->nb_inputs < n) {
            if (!comp_add_input_locked(el, NULL)) ret = ZST_ERROR;
        }
        if (ret == ZST_OK) comp_auto_layout_locked(s);
    } else if (strcmp(name, "request-pad") == 0) {
        if (!comp_add_input_locked(el, (strcmp(value, "auto") == 0) ? NULL : value)) ret = ZST_ERROR;
    } else {
        ret = ZST_ERROR;
    }

    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_result_t
glcomp_get_property(zst_element_t* el, const char* name, char* out, size_t max)
{
    gl_comp_sink_t* s = el->priv;
    if (!name || !out || max == 0) return ZST_ERROR;

    pthread_mutex_lock(&s->lock);

    char pad_name[GLCOMP_MAX_NAME];
    const char* prop = NULL;
    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        gl_comp_input_t* in = comp_find_input_by_name(s, pad_name);
        zst_result_t r = in ? comp_get_pad_property(in, prop, out, max) : ZST_ERROR;
        pthread_mutex_unlock(&s->lock);
        return r;
    }

    zst_result_t ret = ZST_OK;
    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(out, max, "%s", s->window_title);
    } else if (strcmp(name, "canvas-width") == 0 || strcmp(name, "width") == 0) {
        snprintf(out, max, "%u", s->canvas_width);
    } else if (strcmp(name, "canvas-height") == 0 || strcmp(name, "height") == 0) {
        snprintf(out, max, "%u", s->canvas_height);
    } else if (strcmp(name, "background-color") == 0 || strcmp(name, "background") == 0) {
        snprintf(out, max, "#%02x%02x%02x%02x",
                 (unsigned)(s->bg[0] * 255.0f + 0.5f),
                 (unsigned)(s->bg[1] * 255.0f + 0.5f),
                 (unsigned)(s->bg[2] * 255.0f + 0.5f),
                 (unsigned)(s->bg[3] * 255.0f + 0.5f));
    } else if (strcmp(name, "fullscreen") == 0) {
        snprintf(out, max, "%s", s->fullscreen ? "true" : "false");
    } else if (strcmp(name, "vsync") == 0) {
        snprintf(out, max, "%s", s->vsync ? "true" : "false");
    } else if (strcmp(name, "display-rate") == 0 || strcmp(name, "rate") == 0) {
        snprintf(out, max, "%.17g", s->display_rate);
    } else if (strcmp(name, "max-lateness") == 0) {
        snprintf(out, max, "%lld", (long long)s->max_lateness);
    } else if (strcmp(name, "input-count") == 0 || strcmp(name, "inputs") == 0) {
        snprintf(out, max, "%u", s->nb_inputs);
    } else if (strcmp(name, "composite-count") == 0 || strcmp(name, "frame-count") == 0) {
        snprintf(out, max, "%llu", (unsigned long long)s->composite_count);
    } else if (strcmp(name, "null-mode") == 0) {
        snprintf(out, max, "%s", s->null_mode ? "true" : "false");
    } else {
        ret = ZST_ERROR;
    }

    pthread_mutex_unlock(&s->lock);
    return ret;
}

static zst_element_ops_t g_glcompsink_ops = {
    .name = "glcompsink",
    .open = glcomp_open,
    .close = glcomp_close,
    .process = glcomp_process,
    .set_property = glcomp_set_property,
    .get_property = glcomp_get_property,
};

zst_element_t*
zst_gl_comp_sink_create(void)
{
    gl_comp_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;
    snprintf(priv->window_title, sizeof(priv->window_title), "zstreamer GL Compositor Sink");
    priv->canvas_width = 640;
    priv->canvas_height = 480;
    priv->bg[0] = 0.0f; priv->bg[1] = 0.0f; priv->bg[2] = 0.0f; priv->bg[3] = 1.0f;
    priv->vsync = 1;
    priv->max_lateness = 20000000; /* 20ms */
    priv->display_rate = 30.0;
    pthread_mutex_init(&priv->lock, NULL);
    pthread_cond_init(&priv->cond, NULL);
    pthread_cond_init(&priv->capture_cond, NULL);

    zst_element_t* el = zst_element_create(&g_glcompsink_ops, priv);
    if (!el) {
        pthread_cond_destroy(&priv->capture_cond);
        pthread_cond_destroy(&priv->cond);
        pthread_mutex_destroy(&priv->lock);
        free(priv);
        return NULL;
    }

    pthread_mutex_lock(&priv->lock);
    comp_add_input_locked(el, "sink_0");
    pthread_mutex_unlock(&priv->lock);
    return el;
}

zst_element_t*
zst_gl_comp_sink_create_with_config(const zst_gl_comp_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(*config)) return NULL;
    zst_element_t* el = zst_element_factory_make("glcompsink");
    if (!el) return NULL;
    if (config->window_title) zst_element_set_property_string(el, "window-title", config->window_title);
    if (config->canvas_width > 0) zst_element_set_property_uint(el, "canvas-width", config->canvas_width);
    if (config->canvas_height > 0) zst_element_set_property_uint(el, "canvas-height", config->canvas_height);
    if (config->background_color) zst_element_set_property_string(el, "background-color", config->background_color);
    zst_element_set_property_bool(el, "fullscreen", config->fullscreen ? true : false);
    zst_element_set_property_bool(el, "vsync", config->vsync ? true : false);
    if (config->input_count > 0) zst_element_set_property_uint(el, "input-count", config->input_count);
    if (config->struct_size >= offsetof(zst_gl_comp_sink_config_t, max_lateness) + sizeof(int64_t)) {
        if (config->max_lateness > 0) {
            zst_element_set_property_int(el, "max-lateness", config->max_lateness);
        }
    }
    if (config->struct_size >= offsetof(zst_gl_comp_sink_config_t, display_rate) + sizeof(double)) {
        if (config->display_rate > 0) {
            zst_element_set_property_double(el, "display-rate", config->display_rate);
        }
    }
    return el;
}

/* ─── Dynamic plugin support ──────────────────────────────────────────── */

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "glcompsink") == 0 || strcmp(name, "glcomp") == 0) {
        return zst_gl_comp_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_glcompsink_properties[] = {
    { "window-title",    ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "zstreamer GL Compositor Sink", "Window title" },
    { "canvas-width",    ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "640", "Canvas/window width" },
    { "canvas-height",   ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "480", "Canvas/window height" },
    { "background-color",ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "#000000ff", "Canvas background color (#RRGGBB[AA] or r,g,b[,a])" },
    { "fullscreen",      ZST_PROPERTY_BOOL,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Fullscreen mode" },
    { "vsync",           ZST_PROPERTY_BOOL,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "true", "Enable VSync" },
    { "display-rate",    ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "30.0", "Composition frame rate" },
    { "max-lateness",    ZST_PROPERTY_INT,    ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "20000000", "Maximum frame lateness in nanoseconds before dropping" },
    { "input-count",     ZST_PROPERTY_UINT,   ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1", "Number of sink_%u input pads" }
};

static const zst_pad_template_t g_glcompsink_pads[] = {
    { "sink_%u", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_glcompsink_elements[] = {
    {
        .name = "glcompsink",
        .long_name = "OpenGL Compositor Sink",
        .category = "Sink/Video",
        .description = "Composites multiple raw video streams into one OpenGL window",
        .author = "zstreamer",
        .properties = g_glcompsink_properties,
        .nb_properties = sizeof(g_glcompsink_properties) / sizeof(g_glcompsink_properties[0]),
        .pads = g_glcompsink_pads,
        .nb_pads = sizeof(g_glcompsink_pads) / sizeof(g_glcompsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = { .name = "glcompsink_plugin", .author = "zstreamer", .version = "1.0.0", .init = NULL, .deinit = NULL },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) *nb_elements_out = sizeof(g_glcompsink_elements) / sizeof(g_glcompsink_elements[0]);
    return g_glcompsink_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) *p = g_plugin;
    return p;
}
#endif /* BUILDING_PLUGIN */
