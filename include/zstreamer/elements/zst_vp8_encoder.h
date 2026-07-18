/*=============================================================================
    zst_vp8_encoder.h — VP8 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VP8_ENCODER_FACTORY "vp8enc"

#define ZST_VP8_ENCODER_PROP_BITRATE    "bitrate"
#define ZST_VP8_ENCODER_PROP_GOP_SIZE   "gop-size"
#define ZST_VP8_ENCODER_PROP_GOP        "gop"
#define ZST_VP8_ENCODER_PROP_FPS        "fps"
#define ZST_VP8_ENCODER_PROP_THREADS    "threads"
#define ZST_VP8_ENCODER_PROP_WIDTH      "width"
#define ZST_VP8_ENCODER_PROP_HEIGHT     "height"

zst_element_t* zst_vp8_encoder_create(void);

#ifdef __cplusplus
}
#endif
