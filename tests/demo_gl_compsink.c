/*=============================================================================
    demo_gl_compsink.c — OpenGL compositor demo with 5 test patterns
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_gl_comp_sink.h"
#include "zst_pad.h"
#include "zst_bus.h"

#define CHECK_OK(expr, label) \
    do { \
        zst_result_t _r = (expr); \
        if (_r != ZST_OK) { \
            fprintf(stderr, "%s failed: %d\n", (label), (int)_r); \
            goto fail; \
        } \
    } while (0)

#define CHECK_PTR(ptr, label) \
    do { \
        if (!(ptr)) { \
            fprintf(stderr, "%s failed\n", (label)); \
            goto fail; \
        } \
    } while (0)

int main(int argc, char** argv)
{
    zst_pipeline_t* pipe = NULL;
    zst_scheduler_t* sched = NULL;
    zst_element_t* comp = NULL;

    zst_element_t* vsrc[5] = {0};
    zst_element_t* overlay[5] = {0};

    int rc = 1;

    CHECK_OK(zst_register_builtin_elements(), "register builtins");

    pipe = zst_pipeline_create();
    CHECK_PTR(pipe, "zst_pipeline_create");

    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    sched = zst_scheduler_create(&cfg);
    CHECK_PTR(sched, "zst_scheduler_create");

    comp = zst_element_factory_make("glcompsink");
    CHECK_PTR(comp, "factory make glcompsink");
    CHECK_OK(zst_element_set_property_int(comp, "canvas-width", 1280), "canvas-width");
    CHECK_OK(zst_element_set_property_int(comp, "canvas-height", 720), "canvas-height");
    CHECK_OK(zst_pipeline_add(pipe, comp), "add glcompsink");

    const char* patterns[5] = {"bars", "gradient", "checkerboard", "noise", "black"};

    struct { int x; int y; int w; int h; int z; } layouts[5] = {
        { 0,   0,   640, 360, 0 },
        { 640, 0,   640, 360, 0 },
        { 0,   360, 640, 360, 0 },
        { 640, 360, 640, 360, 0 },
        { 320, 180, 640, 360, 1 }
    };

    for (int i = 0; i < 5; i++) {
        vsrc[i] = zst_element_factory_make("videotestsrc");
        overlay[i] = zst_element_factory_make("textoverlay");

        CHECK_PTR(vsrc[i], "factory make videotestsrc");
        CHECK_PTR(overlay[i], "factory make textoverlay");

        CHECK_OK(zst_element_set_property_string(vsrc[i], "pattern", patterns[i]), "vsrc pattern");
        CHECK_OK(zst_element_set_property_int(vsrc[i], "width", layouts[i].w), "vsrc width");
        CHECK_OK(zst_element_set_property_int(vsrc[i], "height", layouts[i].h), "vsrc height");
        CHECK_OK(zst_element_set_property_bool(vsrc[i], "real-time-pacing", true), "vsrc real-time-pacing");

        CHECK_OK(zst_element_set_property_bool(overlay[i], "timecode", true), "overlay timecode");
        CHECK_OK(zst_element_set_property_int(overlay[i], "font-size", 64), "overlay font-size");
        char text[64];
        snprintf(text, sizeof(text), "Video %d", i + 1);
        CHECK_OK(zst_element_set_property_string(overlay[i], "text", text), "overlay text");

        CHECK_OK(zst_pipeline_add(pipe, vsrc[i]), "add vsrc");
        CHECK_OK(zst_pipeline_add(pipe, overlay[i]), "add overlay");

        CHECK_OK(zst_pad_link(zst_element_get_pad(vsrc[i], "src"), zst_element_get_pad(overlay[i], "sink")), "link vsrc->overlay");

        zst_pad_t* sink_pad = zst_gl_comp_sink_request_pad(comp, NULL);
        CHECK_PTR(sink_pad, "request glcompsink pad");

        char prop_prefix[32];
        snprintf(prop_prefix, sizeof(prop_prefix), "%s::x", sink_pad->name);
        CHECK_OK(zst_element_set_property_int(comp, prop_prefix, layouts[i].x), "pad x");
        snprintf(prop_prefix, sizeof(prop_prefix), "%s::y", sink_pad->name);
        CHECK_OK(zst_element_set_property_int(comp, prop_prefix, layouts[i].y), "pad y");
        snprintf(prop_prefix, sizeof(prop_prefix), "%s::width", sink_pad->name);
        CHECK_OK(zst_element_set_property_int(comp, prop_prefix, layouts[i].w), "pad width");
        snprintf(prop_prefix, sizeof(prop_prefix), "%s::height", sink_pad->name);
        CHECK_OK(zst_element_set_property_int(comp, prop_prefix, layouts[i].h), "pad height");
        snprintf(prop_prefix, sizeof(prop_prefix), "%s::z-order", sink_pad->name);
        CHECK_OK(zst_element_set_property_int(comp, prop_prefix, layouts[i].z), "pad z-order");

        CHECK_OK(zst_pad_link(zst_element_get_pad(overlay[i], "src"), sink_pad), "link overlay->comp");
    }

    zst_scheduler_attach(sched, pipe);

    printf("Starting pipeline... (close the window or press ESC/Q to exit)\n");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_READY), "set READY");
    CHECK_OK(zst_pipeline_set_state(pipe, ZST_STATE_PLAYING), "set PLAYING");

    CHECK_OK(zst_scheduler_run(sched), "run scheduler");

    /* Wait for EOS or error on the pipeline bus */
    for (;;) {
        zst_event_t* ev = NULL;
        zst_result_t r = zst_bus_pop(zst_pipeline_get_bus(pipe), &ev, 100);
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_EOS) {
                printf("Received EOS. Exiting...\n");
                zst_event_destroy(ev);
                break;
            } else if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "Pipeline error from element '%s': %s (result=%d)\n",
                        ev->src ? ev->src->ops->name : "unknown",
                        ev->as.error.message ? ev->as.error.message : "unknown",
                        (int)ev->as.error.result);
                zst_event_destroy(ev);
                goto fail;
            } else if (ev->type == ZST_EVENT_KEY_PRESS) {
                printf("[Bus Event: KeyPress] sym=%u, code=%u, str='%s'\n",
                       ev->as.key_press.key_sym,
                       ev->as.key_press.key_code,
                       ev->as.key_press.key_str);
                if (ev->as.key_press.key_sym == 0xFF1B /* XK_Escape */ ||
                    ev->as.key_press.key_sym == 0x0071 /* XK_q */ ||
                    strcmp(ev->as.key_press.key_str, "q") == 0 ||
                    strcmp(ev->as.key_press.key_str, "Escape") == 0) {
                    printf("User pressed Escape/Q. Exiting...\n");
                    zst_event_destroy(ev);
                    break;
                }
                if (ev->as.key_press.key_sym == 0xFFC8 /* XK_F11 */) {
                    bool fs = false;
                    zst_element_get_property_bool(comp, "fullscreen", &fs);
                    zst_element_set_property_bool(comp, "fullscreen", !fs);
                    printf("Toggled fullscreen via application: %s\n", !fs ? "ON" : "OFF");
                }
            } else if (ev->type == ZST_EVENT_MOUSE_BUTTON) {
                printf("[Bus Event: MouseButton] button=%u, pressed=%d, x=%d, y=%d\n",
                       ev->as.mouse_button.button,
                       ev->as.mouse_button.pressed,
                       ev->as.mouse_button.x,
                       ev->as.mouse_button.y);
            } else if (ev->type == ZST_EVENT_MOUSE_MOTION) {
                printf("[Bus Event: MouseMotion] x=%d, y=%d\n",
                       ev->as.mouse_motion.x,
                       ev->as.mouse_motion.y);
            }
            zst_event_destroy(ev);
        }
    }

    rc = 0;

fail:
    if (pipe) zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    if (sched) zst_scheduler_destroy(sched);
    if (pipe) zst_pipeline_destroy(pipe);

    return rc;
}
