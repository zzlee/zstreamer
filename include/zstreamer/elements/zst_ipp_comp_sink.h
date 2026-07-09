/*=============================================================================
    zst_ipp_comp_sink.h — Intel IPP compositor display sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_IPP_COMP_SINK_FACTORY                 "ippcompsink"
#define ZST_IPP_COMP_SINK_PROP_WINDOW_TITLE       "window-title"
#define ZST_IPP_COMP_SINK_PROP_CANVAS_WIDTH       "canvas-width"
#define ZST_IPP_COMP_SINK_PROP_CANVAS_HEIGHT      "canvas-height"
#define ZST_IPP_COMP_SINK_PROP_BACKGROUND_COLOR   "background-color"
#define ZST_IPP_COMP_SINK_PROP_FULLSCREEN         "fullscreen"
#define ZST_IPP_COMP_SINK_PROP_VSYNC              "vsync"
#define ZST_IPP_COMP_SINK_PROP_INPUT_COUNT        "input-count"
#define ZST_IPP_COMP_SINK_PROP_REQUEST_PAD        "request-pad"

#define ZST_IPP_COMP_SINK_PAD_PROP_X              "x"
#define ZST_IPP_COMP_SINK_PAD_PROP_Y              "y"
#define ZST_IPP_COMP_SINK_PAD_PROP_WIDTH          "width"
#define ZST_IPP_COMP_SINK_PAD_PROP_HEIGHT         "height"
#define ZST_IPP_COMP_SINK_PAD_PROP_Z_ORDER        "z-order"
#define ZST_IPP_COMP_SINK_PAD_PROP_ALPHA          "alpha"
#define ZST_IPP_COMP_SINK_PAD_PROP_VISIBLE        "visible"
#define ZST_IPP_COMP_SINK_PAD_PROP_SCALING        "scaling"

typedef struct {
    size_t struct_size;
    const char* window_title;
    uint32_t canvas_width;
    uint32_t canvas_height;
    const char* background_color;
    int fullscreen;
    int vsync;
    uint32_t input_count;
    int64_t max_lateness;
    double display_rate;
} zst_ipp_comp_sink_config_t;

zst_element_t* zst_ipp_comp_sink_create(void);
zst_element_t* zst_ipp_comp_sink_create_with_config(const zst_ipp_comp_sink_config_t* config);

/* Request a sink pad.  If name is NULL, the next sink_%u pad is created. */
zst_pad_t* zst_ipp_comp_sink_request_pad(zst_element_t* el, const char* name);
zst_result_t zst_ipp_comp_sink_release_pad(zst_element_t* el, zst_pad_t* pad);

/* Capture the current composited output into an RGBA pixel buffer.
 * width/height: pixel dimensions for the capture.
 * rgba_out: caller-allocated buffer of width*height*4 bytes.
 * Returns ZST_OK on success, ZST_ERROR if in null mode or no context. */
zst_result_t zst_ipp_comp_sink_capture(zst_element_t* el,
                                        uint32_t width, uint32_t height,
                                        uint8_t* rgba_out);

#ifdef __cplusplus
}
#endif
