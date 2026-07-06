/*=============================================================================
    gl_sink.c — OpenGL display sink element

    Renders raw video frames to an X11 window using OpenGL (GLX). Supports
    YUV420P, NV12, and RGB pixel formats with GPU-based YUV→RGB conversion
    via GLSL fragment shaders. Falls back to a null sink (discard) in
    headless environments when no display server is available.
=============================================================================*/

#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <GL/gl.h>
#include <GL/glx.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_gl_sink.h"
#include "zst_buffer.h"
#include "zst_log.h"
#include "zst_pipeline.h"
#include "zst_bus.h"

/* ─── Forward declarations ─────────────────────────────────────────────── */

/* ─── Private data ──────────────────────────────────────────────────────── */

typedef struct {
    /* Properties */
    char window_title[256];
    uint32_t width;
    uint32_t height;
    int fullscreen;
    int vsync;
    char scaling[16];       /* "fit", "stretch", "crop" */
    int64_t max_lateness;   /* nanoseconds */
    char color_matrix[16];  /* "bt601", "bt709" */
    double brightness;
    double contrast;
    double saturation;

    /* X11 state */
    Display* x_display;
    Window x_window;
    Atom wm_delete_message;
    int x_screen;
    int window_x;
    int window_y;
    bool window_open;
    bool null_mode;  /* true when no display available — acts as fakesink */

    /* GLX state */
    GLXContext gl_context;
    Bool double_buffered;

    /* Shaders */
    GLuint program_yuv420p;      /* 3-plane YUV420P */
    GLuint program_nv12;         /* 2-plane NV12 */
    GLuint program_rgb;          /* 1-plane RGB */
    GLuint active_program;

    /* Uniform locations (YUV420P / NV12) */
    GLint uniform_y_tex;
    GLint uniform_u_tex;
    GLint uniform_v_tex;
    GLint uniform_uv_tex;
    GLint uniform_color_matrix;
    GLint uniform_brightness;
    GLint uniform_contrast;
    GLint uniform_saturation;

    /* Texture objects */
    GLuint tex_y;
    GLuint tex_u;
    GLuint tex_v;
    GLuint tex_uv;
    GLuint tex_rgb;

    /* Texture dimensions (for reallocation on resize) */
    uint32_t tex_width;
    uint32_t tex_height;
    bool textures_valid;

    /* Frame timing */
    uint64_t frame_count;

    /* X11 error trapping */
    int (*old_x_error_handler)(Display*, XErrorEvent*);
} gl_sink_t;

/* ─── Vertex shader (common) ──────────────────────────────────────────── */

static const char* g_vertex_shader_src =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

/* ─── Fragment shader: YUV420P (3 planes) ─────────────────────────────── */

static const char* g_frag_yuv420p_src =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D v_tex;\n"
    "uniform mat3 color_matrix;\n"
    "uniform float brightness;\n"
    "uniform float contrast;\n"
    "uniform float saturation;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    float u = texture2D(u_tex, tc).r - 0.5;\n"
    "    float v = texture2D(v_tex, tc).r - 0.5;\n"
    "    vec3 yuv = vec3(y, u, v);\n"
    "    vec3 rgb = color_matrix * yuv;\n"
    "    rgb = (rgb - 0.5) * contrast + 0.5 + brightness;\n"
    "    float luma = dot(vec3(0.299, 0.587, 0.114), rgb);\n"
    "    rgb = mix(vec3(luma), rgb, saturation);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* ─── Fragment shader: NV12 (2 planes) ────────────────────────────────── */

static const char* g_frag_nv12_src =
    "#version 120\n"
    "uniform sampler2D y_tex;\n"
    "uniform sampler2D uv_tex;\n"
    "uniform mat3 color_matrix;\n"
    "uniform float brightness;\n"
    "uniform float contrast;\n"
    "uniform float saturation;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    float y = texture2D(y_tex, tc).r;\n"
    "    vec2 uv = texture2D(uv_tex, tc).rg;\n"
    "    float u = uv.r - 0.5;\n"
    "    float v = uv.g - 0.5;\n"
    "    vec3 yuv = vec3(y, u, v);\n"
    "    vec3 rgb = color_matrix * yuv;\n"
    "    rgb = (rgb - 0.5) * contrast + 0.5 + brightness;\n"
    "    float luma = dot(vec3(0.299, 0.587, 0.114), rgb);\n"
    "    rgb = mix(vec3(luma), rgb, saturation);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* ─── Fragment shader: RGB (1 plane) ──────────────────────────────────── */

static const char* g_frag_rgb_src =
    "#version 120\n"
    "uniform sampler2D rgb_tex;\n"
    "uniform float brightness;\n"
    "uniform float contrast;\n"
    "uniform float saturation;\n"
    "void main() {\n"
    "    vec2 tc = gl_TexCoord[0].st;\n"
    "    vec3 rgb = texture2D(rgb_tex, tc).rgb;\n"
    "    rgb = (rgb - 0.5) * contrast + 0.5 + brightness;\n"
    "    float luma = dot(vec3(0.299, 0.587, 0.114), rgb);\n"
    "    rgb = mix(vec3(luma), rgb, saturation);\n"
    "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* ─── Helper: compile a GLSL shader ──────────────────────────────────── */

static GLuint
compile_shader(GLenum type, const char* src)
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
        ZST_LOG_ERROR("glsink", "shader compilation error (type=%d): %s", (int)type, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* ─── Helper: link a GLSL program ─────────────────────────────────────── */

static GLuint
link_program(GLuint vs, GLuint fs)
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
        ZST_LOG_ERROR("glsink", "program link error: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ─── Helper: build a 3x3 colour matrix uniform ───────────────────────── */

static void
glsink_get_color_matrix(gl_sink_t* s, float mat[9])
{
    /* Identity default */
    memset(mat, 0, sizeof(float) * 9);

    if (strcmp(s->color_matrix, "bt709") == 0) {
        /* ITU-R BT.709 */
        mat[0] = 1.0f;     mat[1] = 0.0f;      mat[2] = 1.5748f;
        mat[3] = 1.0f;     mat[4] = -0.1873f;   mat[5] = -0.4681f;
        mat[6] = 1.0f;     mat[7] = 1.8556f;    mat[8] = 0.0f;
    } else {
        /* ITU-R BT.601 (default) */
        mat[0] = 1.0f;     mat[1] = 0.0f;      mat[2] = 1.402f;
        mat[3] = 1.0f;     mat[4] = -0.344f;    mat[5] = -0.714f;
        mat[6] = 1.0f;     mat[7] = 1.772f;     mat[8] = 0.0f;
    }
}

/* ─── Build all shaders ───────────────────────────────────────────────── */

static int
glsink_build_shaders(gl_sink_t* s)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, g_vertex_shader_src);
    if (!vs) return -1;

    /* YUV420P */
    GLuint fs_yuv = compile_shader(GL_FRAGMENT_SHADER, g_frag_yuv420p_src);
    if (fs_yuv) {
        s->program_yuv420p = link_program(vs, fs_yuv);
        glDeleteShader(fs_yuv);
    }
    if (!s->program_yuv420p) {
        ZST_LOG_ERROR("glsink", "failed to build YUV420P shader program");
    }

    /* NV12 */
    GLuint fs_nv12 = compile_shader(GL_FRAGMENT_SHADER, g_frag_nv12_src);
    if (fs_nv12) {
        s->program_nv12 = link_program(vs, fs_nv12);
        glDeleteShader(fs_nv12);
    }
    if (!s->program_nv12) {
        ZST_LOG_ERROR("glsink", "failed to build NV12 shader program");
    }

    /* RGB */
    GLuint fs_rgb = compile_shader(GL_FRAGMENT_SHADER, g_frag_rgb_src);
    if (fs_rgb) {
        s->program_rgb = link_program(vs, fs_rgb);
        glDeleteShader(fs_rgb);
    }
    if (!s->program_rgb) {
        ZST_LOG_ERROR("glsink", "failed to build RGB shader program");
    }

    glDeleteShader(vs);

    if (!s->program_yuv420p && !s->program_nv12 && !s->program_rgb) {
        return -1;
    }
    return 0;
}

/* ─── Helper: get uniform locations ────────────────────────────────────── */

static void
glsink_get_uniforms(gl_sink_t* s, GLuint prog)
{
    s->uniform_y_tex    = glGetUniformLocation(prog, "y_tex");
    s->uniform_u_tex    = glGetUniformLocation(prog, "u_tex");
    s->uniform_v_tex    = glGetUniformLocation(prog, "v_tex");
    s->uniform_uv_tex   = glGetUniformLocation(prog, "uv_tex");
    s->uniform_color_matrix = glGetUniformLocation(prog, "color_matrix");
    s->uniform_brightness   = glGetUniformLocation(prog, "brightness");
    s->uniform_contrast     = glGetUniformLocation(prog, "contrast");
    s->uniform_saturation   = glGetUniformLocation(prog, "saturation");
}

/* ─── Helper: set common program uniforms ─────────────────────────────── */

static void
glsink_set_common_uniforms(gl_sink_t* s)
{
    float cmat[9];
    glsink_get_color_matrix(s, cmat);
    if (s->uniform_color_matrix >= 0) {
        glUniformMatrix3fv(s->uniform_color_matrix, 1, GL_FALSE, cmat);
    }
    if (s->uniform_brightness >= 0) {
        glUniform1f(s->uniform_brightness, (float)s->brightness);
    }
    if (s->uniform_contrast >= 0) {
        glUniform1f(s->uniform_contrast, (float)s->contrast);
    }
    if (s->uniform_saturation >= 0) {
        glUniform1f(s->uniform_saturation, (float)s->saturation);
    }
}

/* ─── Helper: create textures (initial or resize) ─────────────────────── */

static int
glsink_create_textures(gl_sink_t* s, uint32_t width, uint32_t height)
{
    if (s->textures_valid && s->tex_width == width && s->tex_height == height) {
        return 0;  /* Already correct size */
    }

    /* Delete old textures */
    if (s->textures_valid) {
        glDeleteTextures(1, &s->tex_y);
        glDeleteTextures(1, &s->tex_u);
        glDeleteTextures(1, &s->tex_v);
        glDeleteTextures(1, &s->tex_uv);
        glDeleteTextures(1, &s->tex_rgb);
    }

    s->tex_width = width;
    s->tex_height = height;

    glGenTextures(1, &s->tex_y);
    glGenTextures(1, &s->tex_u);
    glGenTextures(1, &s->tex_v);
    glGenTextures(1, &s->tex_uv);
    glGenTextures(1, &s->tex_rgb);

    /* Y plane */
    glBindTexture(GL_TEXTURE_2D, s->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    /* U plane (half resolution) */
    glBindTexture(GL_TEXTURE_2D, s->tex_u);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    /* V plane (half resolution) */
    glBindTexture(GL_TEXTURE_2D, s->tex_v);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

    /* UV plane (NV12: half-res interleaved) */
    glBindTexture(GL_TEXTURE_2D, s->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width / 2, height / 2, 0,
                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);

    /* RGB texture */
    glBindTexture(GL_TEXTURE_2D, s->tex_rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);

    s->textures_valid = true;
    return 0;
}

/* ─── Helper: render a full-screen textured quad ──────────────────────── */

static void
glsink_render_quad(gl_sink_t* s, float win_w, float win_h,
                   float img_w, float img_h)
{
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    if (strcmp(s->scaling, "fit") == 0) {
        /* Letterbox / pillar-box */
        float aspect_img = img_w / img_h;
        float aspect_win = win_w / win_h;
        if (aspect_img > aspect_win) {
            scale_y = aspect_win / aspect_img;
        } else {
            scale_x = aspect_img / aspect_win;
        }
    } else if (strcmp(s->scaling, "crop") == 0) {
        /* Zoom to fill, crop edges */
        float aspect_img = img_w / img_h;
        float aspect_win = win_w / win_h;
        if (aspect_img > aspect_win) {
            scale_x = aspect_img / aspect_win;
        } else {
            scale_y = aspect_win / aspect_img;
        }
    }
    /* "stretch" — no scaling, fills window exactly */

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-scale_x, -scale_y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( scale_x, -scale_y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( scale_x,  scale_y);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-scale_x,  scale_y);
    glEnd();
}

/* ─── Helper: upload and render a YUV420P frame ────────────────────────── */

static void
glsink_render_yuv420p(gl_sink_t* s, const zst_video_frame_t* frame)
{
    if (!s->program_yuv420p) return;

    glUseProgram(s->program_yuv420p);
    s->active_program = s->program_yuv420p;
    glsink_get_uniforms(s, s->program_yuv420p);
    glsink_set_common_uniforms(s);

    /* Upload Y plane */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->tex_y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width, s->tex_height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Upload U plane */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s->tex_u);
    if (frame->stride[1] > 0 && frame->stride[1] != (int32_t)(s->tex_width / 2)) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[1]);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width / 2, s->tex_height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[1]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Upload V plane */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s->tex_v);
    if (frame->stride[2] > 0 && frame->stride[2] != (int32_t)(s->tex_width / 2)) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[2]);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width / 2, s->tex_height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[2]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Set texture units */
    if (s->uniform_y_tex >= 0) glUniform1i(s->uniform_y_tex, 0);
    if (s->uniform_u_tex >= 0) glUniform1i(s->uniform_u_tex, 1);
    if (s->uniform_v_tex >= 0) glUniform1i(s->uniform_v_tex, 2);

    /* Render */
    XWindowAttributes wa;
    XGetWindowAttributes(s->x_display, s->x_window, &wa);
    glViewport(0, 0, wa.width, wa.height);
    glsink_render_quad(s, (float)wa.width, (float)wa.height,
                       (float)frame->width, (float)frame->height);
}

/* ─── Helper: upload and render an NV12 frame ──────────────────────────── */

static void
glsink_render_nv12(gl_sink_t* s, const zst_video_frame_t* frame)
{
    if (!s->program_nv12) return;

    glUseProgram(s->program_nv12);
    s->active_program = s->program_nv12;
    glsink_get_uniforms(s, s->program_nv12);
    glsink_set_common_uniforms(s);

    /* Upload Y plane */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->tex_y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width, s->tex_height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->plane[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Upload UV plane (interleaved) */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s->tex_uv);
    if (frame->stride[1] > 0 && frame->stride[1] != (int32_t)(s->tex_width)) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[1]);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width / 2, s->tex_height / 2,
                    GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, frame->plane[1]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (s->uniform_y_tex >= 0)  glUniform1i(s->uniform_y_tex, 0);
    if (s->uniform_uv_tex >= 0) glUniform1i(s->uniform_uv_tex, 1);

    XWindowAttributes wa;
    XGetWindowAttributes(s->x_display, s->x_window, &wa);
    glViewport(0, 0, wa.width, wa.height);
    glsink_render_quad(s, (float)wa.width, (float)wa.height,
                       (float)frame->width, (float)frame->height);
}

/* ─── Helper: upload and render an RGB frame ───────────────────────────── */

static void
glsink_render_rgb(gl_sink_t* s, const zst_video_frame_t* frame)
{
    if (!s->program_rgb) return;

    glUseProgram(s->program_rgb);
    s->active_program = s->program_rgb;
    s->uniform_brightness = glGetUniformLocation(s->program_rgb, "brightness");
    s->uniform_contrast   = glGetUniformLocation(s->program_rgb, "contrast");
    s->uniform_saturation = glGetUniformLocation(s->program_rgb, "saturation");
    if (s->uniform_brightness >= 0) glUniform1f(s->uniform_brightness, (float)s->brightness);
    if (s->uniform_contrast >= 0)   glUniform1f(s->uniform_contrast, (float)s->contrast);
    if (s->uniform_saturation >= 0) glUniform1f(s->uniform_saturation, (float)s->saturation);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->tex_rgb);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->stride[0] / 3);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->tex_width, s->tex_height,
                    GL_RGB, GL_UNSIGNED_BYTE, frame->plane[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    GLint loc = glGetUniformLocation(s->program_rgb, "rgb_tex");
    if (loc >= 0) glUniform1i(loc, 0);

    XWindowAttributes wa;
    XGetWindowAttributes(s->x_display, s->x_window, &wa);
    glViewport(0, 0, wa.width, wa.height);
    glsink_render_quad(s, (float)wa.width, (float)wa.height,
                       (float)frame->width, (float)frame->height);
}

/* ─── Helper: detect pixel format from zst_video_frame_t ─────────────── */

typedef enum {
    GLFMT_YUV420P,
    GLFMT_NV12,
    GLFMT_RGB,
    GLFMT_UNKNOWN
} gl_pixel_fmt_t;

static gl_pixel_fmt_t
glsink_detect_format(const zst_video_frame_t* frame)
{
    if (!frame) return GLFMT_UNKNOWN;

    /* Check number of planes */
    if (frame->plane[1] && frame->plane[2] && !frame->plane[3]) {
        /* 3 planes: YUV420P / I420 */
        return GLFMT_YUV420P;
    }
    if (frame->plane[1] && !frame->plane[2]) {
        /* 2 planes likely indicates NV12 (Y + UV interleaved) */
        return GLFMT_NV12;
    }
    if (frame->plane[0] && !frame->plane[1]) {
        /* Single plane: RGB or BGR */
        return GLFMT_RGB;
    }
    return GLFMT_UNKNOWN;
}

/* ─── Helper: check X11 events (window close, ESC key) ────────────────── */

static int
glsink_check_events(gl_sink_t* s, zst_element_t* el)
{
    if (!s->x_display || !s->window_open) return 0;

    while (XPending(s->x_display)) {
        XEvent ev;
        XNextEvent(s->x_display, &ev);

        if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == s->wm_delete_message) {
                ZST_LOG_INFO("glsink", "window closed by user");
                return 1;  /* Signal EOS */
            }
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
            s->width = ev.xconfigure.width;
            s->height = ev.xconfigure.height;
        }
    }

    return 0;
}

/* ─── Element operations ───────────────────────────────────────────────── */

static zst_result_t
glsink_open(zst_element_t* el)
{
    gl_sink_t* s = el->priv;
    s->null_mode = false;
    s->window_open = false;
    s->gl_context = NULL;
    s->textures_valid = false;
    s->frame_count = 0;

    /* Try to open X11 display */
    s->x_display = XOpenDisplay(NULL);
    if (!s->x_display) {
        ZST_LOG_WARN("glsink", "no X11 display available — running in null-sink mode (frames discarded)");
        s->null_mode = true;
        return ZST_OK;
    }

    s->x_screen = DefaultScreen(s->x_display);

    /* Get window size from display if not explicitly configured */
    if (s->width == 0 || s->height == 0) {
        s->width = DisplayWidth(s->x_display, s->x_screen) / 2;
        s->height = DisplayHeight(s->x_display, s->x_screen) / 2;
    }

    /* Create window */
    Window root = RootWindow(s->x_display, s->x_screen);
    XSetWindowAttributes swa;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | ClientMessage;
    swa.background_pixel = BlackPixel(s->x_display, s->x_screen);
    swa.border_pixel = 0;

    s->x_window = XCreateWindow(s->x_display, root,
                                0, 0, s->width, s->height, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWEventMask | CWBackPixel | CWBorderPixel,
                                &swa);
    if (!s->x_window) {
        ZST_LOG_ERROR("glsink", "failed to create X11 window");
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = true;
        return ZST_OK;  /* Graceful fallback */
    }

    /* Set window title */
    XStoreName(s->x_display, s->x_window, s->window_title);
    XSetIconName(s->x_display, s->x_window, s->window_title);

    /* Handle window close button */
    s->wm_delete_message = XInternAtom(s->x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->x_display, s->x_window, &s->wm_delete_message, 1);

    /* Map window */
    XMapWindow(s->x_display, s->x_window);
    XFlush(s->x_display);

    /* Create GLX context */
    int glx_attrs[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 16,
        None
    };

    XVisualInfo* vi = glXChooseVisual(s->x_display, s->x_screen, glx_attrs);
    if (!vi) {
        ZST_LOG_ERROR("glsink", "glXChooseVisual failed — no suitable visual");
        XDestroyWindow(s->x_display, s->x_window);
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = true;
        return ZST_OK;
    }

    s->gl_context = glXCreateContext(s->x_display, vi, None, GL_TRUE);
    if (!s->gl_context) {
        ZST_LOG_ERROR("glsink", "glXCreateContext failed");
        XFree(vi);
        XDestroyWindow(s->x_display, s->x_window);
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = true;
        return ZST_OK;
    }

    glXMakeCurrent(s->x_display, s->x_window, s->gl_context);

    /* Check for double buffering */
    int dbuf = 0;
    glXGetConfig(s->x_display, vi, GLX_DOUBLEBUFFER, &dbuf);
    s->double_buffered = dbuf ? True : False;

    XFree(vi);

    /* Build shaders */
    if (glsink_build_shaders(s) != 0) {
        ZST_LOG_ERROR("glsink", "shader compilation failed — falling back to null mode");
        glXMakeCurrent(s->x_display, None, NULL);
        glXDestroyContext(s->x_display, s->gl_context);
        s->gl_context = NULL;
        XDestroyWindow(s->x_display, s->x_window);
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
        s->null_mode = true;
        return ZST_OK;
    }

    /* Create textures (initial allocation) */
    if (glsink_create_textures(s, 640, 480) != 0) {
        ZST_LOG_WARN("glsink", "initial texture allocation failed");
    }

    /* Set VSync */
    if (s->vsync && s->gl_context) {
        typedef void (*SwapIntervalFunc)(Display*, GLXDrawable, int);
        SwapIntervalFunc swapInterval =
            (SwapIntervalFunc)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
        if (swapInterval) {
            swapInterval(s->x_display, glXGetCurrentDrawable(), 1);
        }
    }

    /* Enable blending for any alpha-based operations */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    ZST_LOG_INFO("glsink", "opened window '%s' (%ux%u) with GL",
                 s->window_title, s->width, s->height);

    s->window_open = true;
    return ZST_OK;
}

static zst_result_t
glsink_close(zst_element_t* el)
{
    gl_sink_t* s = el->priv;

    if (s->gl_context) {
        glXMakeCurrent(s->x_display, s->x_window, s->gl_context);

        if (s->program_yuv420p) glDeleteProgram(s->program_yuv420p);
        if (s->program_nv12)    glDeleteProgram(s->program_nv12);
        if (s->program_rgb)     glDeleteProgram(s->program_rgb);

        if (s->textures_valid) {
            glDeleteTextures(1, &s->tex_y);
            glDeleteTextures(1, &s->tex_u);
            glDeleteTextures(1, &s->tex_v);
            glDeleteTextures(1, &s->tex_uv);
            glDeleteTextures(1, &s->tex_rgb);
            s->textures_valid = false;
        }

        glXMakeCurrent(s->x_display, None, NULL);
        glXDestroyContext(s->x_display, s->gl_context);
        s->gl_context = NULL;
    }

    if (s->x_display && s->window_open) {
        XDestroyWindow(s->x_display, s->x_window);
        s->window_open = false;
    }

    if (s->x_display) {
        XCloseDisplay(s->x_display);
        s->x_display = NULL;
    }

    s->null_mode = false;
    return ZST_OK;
}

static zst_result_t
glsink_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    gl_sink_t* s = el->priv;
    (void)out;

    if (!in) return ZST_ERROR;

    /* Check for EOS flag */
    if (in->flags & ZST_BUFFER_FLAG_EOS) {
        return ZST_EOF;
    }

    s->frame_count++;

    /* Null mode: just discard */
    if (s->null_mode || !s->gl_context) {
        return ZST_OK;
    }

    /* Process X11 events (window close, keyboard) */
    if (glsink_check_events(s, el)) {
        return ZST_EOF;
    }

    /* Get video frame */
    zst_video_frame_t* frame = (zst_video_frame_t*)in->payload;
    if (!frame) {
        return ZST_OK;  /* Discard non-video buffers */
    }

    /* Reallocate textures if frame size changed */
    if (frame->width > 0 && frame->height > 0) {
        glsink_create_textures(s, frame->width, frame->height);
    }

    /* Make GL context current */
    glXMakeCurrent(s->x_display, s->x_window, s->gl_context);

    /* Clear */
    glClear(GL_COLOR_BUFFER_BIT);

    /* Detect format and render */
    gl_pixel_fmt_t fmt = glsink_detect_format(frame);
    switch (fmt) {
        case GLFMT_YUV420P:
            glsink_render_yuv420p(s, frame);
            break;
        case GLFMT_NV12:
            glsink_render_nv12(s, frame);
            break;
        case GLFMT_RGB:
            glsink_render_rgb(s, frame);
            break;
        default:
            ZST_LOG_WARN("glsink", "unknown pixel format (%d planes), skipping frame",
                         (int)(frame->plane[1] ? (frame->plane[2] ? 3 : 2) : 1));
            break;
    }

    /* Swap buffers */
    if (s->double_buffered) {
        glXSwapBuffers(s->x_display, s->x_window);
    } else {
        glFlush();
    }

    /* Propagate EOS if window was closed during rendering */
    if (glsink_check_events(s, el)) {
        return ZST_EOF;
    }

    return ZST_OK;
}

static zst_result_t
glsink_set_property(zst_element_t* el, const char* name, const char* value)
{
    gl_sink_t* s = el->priv;

    if (strcmp(name, "window-title") == 0 || strcmp(name, "window_title") == 0 ||
        strcmp(name, "title") == 0) {
        snprintf(s->window_title, sizeof(s->window_title), "%s", value);
        if (s->x_display && s->window_open) {
            XStoreName(s->x_display, s->x_window, s->window_title);
        }
        return ZST_OK;
    }
    if (strcmp(name, "width") == 0) {
        s->width = (uint32_t)atol(value);
        return ZST_OK;
    }
    if (strcmp(name, "height") == 0) {
        s->height = (uint32_t)atol(value);
        return ZST_OK;
    }
    if (strcmp(name, "fullscreen") == 0) {
        s->fullscreen = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "vsync") == 0) {
        s->vsync = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "scaling") == 0) {
        if (strcmp(value, "fit") == 0 || strcmp(value, "stretch") == 0 ||
            strcmp(value, "crop") == 0) {
            snprintf(s->scaling, sizeof(s->scaling), "%s", value);
        }
        return ZST_OK;
    }
    if (strcmp(name, "max-lateness") == 0 || strcmp(name, "max_lateness") == 0) {
        s->max_lateness = (int64_t)atoll(value);
        return ZST_OK;
    }
    if (strcmp(name, "color-matrix") == 0 || strcmp(name, "color_matrix") == 0) {
        if (strcmp(value, "bt601") == 0 || strcmp(value, "bt709") == 0) {
            snprintf(s->color_matrix, sizeof(s->color_matrix), "%s", value);
        }
        return ZST_OK;
    }
    if (strcmp(name, "brightness") == 0) {
        s->brightness = atof(value);
        return ZST_OK;
    }
    if (strcmp(name, "contrast") == 0) {
        s->contrast = atof(value);
        return ZST_OK;
    }
    if (strcmp(name, "saturation") == 0) {
        s->saturation = atof(value);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
glsink_get_property(zst_element_t* el, const char* name,
                    char* value_out, size_t max_len)
{
    gl_sink_t* s = el->priv;

    if (strcmp(name, "window-title") == 0 || strcmp(name, "title") == 0) {
        snprintf(value_out, max_len, "%s", s->window_title);
        return ZST_OK;
    }
    if (strcmp(name, "width") == 0) {
        snprintf(value_out, max_len, "%u", s->width);
        return ZST_OK;
    }
    if (strcmp(name, "height") == 0) {
        snprintf(value_out, max_len, "%u", s->height);
        return ZST_OK;
    }
    if (strcmp(name, "fullscreen") == 0) {
        snprintf(value_out, max_len, "%s", s->fullscreen ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "vsync") == 0) {
        snprintf(value_out, max_len, "%s", s->vsync ? "true" : "false");
        return ZST_OK;
    }
    if (strcmp(name, "scaling") == 0) {
        snprintf(value_out, max_len, "%s", s->scaling);
        return ZST_OK;
    }
    if (strcmp(name, "max-lateness") == 0) {
        snprintf(value_out, max_len, "%lld", (long long)s->max_lateness);
        return ZST_OK;
    }
    if (strcmp(name, "color-matrix") == 0) {
        snprintf(value_out, max_len, "%s", s->color_matrix);
        return ZST_OK;
    }
    if (strcmp(name, "frame-count") == 0 || strcmp(name, "frame_count") == 0) {
        snprintf(value_out, max_len, "%llu", (unsigned long long)s->frame_count);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_element_ops_t g_glsink_ops = {
    .name = "glsink",
    .open = glsink_open,
    .close = glsink_close,
    .process = glsink_process,
    .set_property = glsink_set_property,
    .get_property = glsink_get_property,
};

/* ─── Public API ────────────────────────────────────────────────────────── */

zst_element_t*
zst_gl_sink_create(void)
{
    gl_sink_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    snprintf(priv->window_title, sizeof(priv->window_title), "zstreamer GL Sink");
    priv->width = 0;   /* Auto-detect on open */
    priv->height = 0;
    priv->fullscreen = 0;
    priv->vsync = 1;
    snprintf(priv->scaling, sizeof(priv->scaling), "fit");
    priv->max_lateness = 20000000; /* 20ms */
    snprintf(priv->color_matrix, sizeof(priv->color_matrix), "bt601");
    priv->brightness = 0.0;
    priv->contrast = 1.0;
    priv->saturation = 1.0;

    zst_element_t* el = zst_element_create(&g_glsink_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    if (sink) {
        zst_element_add_pad(el, sink);
    }

    return el;
}

zst_element_t*
zst_gl_sink_create_with_config(const zst_gl_sink_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_gl_sink_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("glsink");
    if (!el) return NULL;

    if (config->window_title)
        zst_element_set_property_string(el, "window-title", config->window_title);
    if (config->width > 0)
        zst_element_set_property_uint(el, "width", config->width);
    if (config->height > 0)
        zst_element_set_property_uint(el, "height", config->height);
    zst_element_set_property_bool(el, "fullscreen", config->fullscreen ? true : false);
    zst_element_set_property_bool(el, "vsync", config->vsync ? true : false);
    if (config->scaling)
        zst_element_set_property_string(el, "scaling", config->scaling);
    if (config->max_lateness > 0)
        zst_element_set_property_int(el, "max-lateness", config->max_lateness);
    if (config->color_matrix)
        zst_element_set_property_string(el, "color-matrix", config->color_matrix);
    zst_element_set_property_double(el, "brightness", config->brightness);
    zst_element_set_property_double(el, "contrast", config->contrast);
    zst_element_set_property_double(el, "saturation", config->saturation);

    return el;
}

/* ─── Dynamic plugin support ────────────────────────────────────────────── */

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "glsink") == 0) {
        return zst_gl_sink_create();
    }
    if (strcmp(name, "glsink2") == 0) {
        return zst_gl_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_glsink_properties[] = {
    { "window-title",  ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "zstreamer GL Sink", "Window title" },
    { "width",         ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "0", "Window width (0 = auto)" },
    { "height",        ZST_PROPERTY_UINT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "0", "Window height (0 = auto)" },
    { "fullscreen",    ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "false", "Fullscreen mode" },
    { "vsync",         ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "true", "Enable VSync" },
    { "scaling",       ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "fit", "Scaling mode: fit, stretch, crop" },
    { "max-lateness",  ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "20000000", "Maximum frame lateness in nanoseconds before dropping" },
    { "color-matrix",  ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE,
      "bt601", "Color matrix: bt601, bt709" },
    { "brightness",    ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "0.0", "Brightness adjustment (-1.0 to 1.0)" },
    { "contrast",      ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "1.0", "Contrast adjustment (0.0 to 2.0)" },
    { "saturation",    ZST_PROPERTY_DOUBLE, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE | ZST_PROPERTY_RUNTIME,
      "1.0", "Saturation adjustment (0.0 to 2.0)" }
};

static const zst_pad_template_t g_glsink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/x-raw" }
};

static const zst_element_desc_t g_glsink_elements[] = {
    {
        .name = "glsink",
        .long_name = "OpenGL Sink",
        .category = "Sink/Video",
        .description = "Displays video frames in an OpenGL window with GPU YUV→RGB conversion",
        .author = "zstreamer",
        .properties = g_glsink_properties,
        .nb_properties = sizeof(g_glsink_properties) / sizeof(g_glsink_properties[0]),
        .pads = g_glsink_pads,
        .nb_pads = sizeof(g_glsink_pads) / sizeof(g_glsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "glsink_plugin",
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
        *nb_elements_out = sizeof(g_glsink_elements) / sizeof(g_glsink_elements[0]);
    }
    return g_glsink_elements;
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
#endif /* BUILDING_PLUGIN */
