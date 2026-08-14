/*=============================================================================
    zst_x264_encoder.h — X264 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_X264_ENCODER_FACTORY "x264enc"

#define ZST_X264_ENCODER_PROP_PRESET    "preset"
#define ZST_X264_ENCODER_PROP_TUNE      "tune"
#define ZST_X264_ENCODER_PROP_CRF       "crf"
#define ZST_X264_ENCODER_PROP_BITRATE   "bitrate"
#define ZST_X264_ENCODER_PROP_GOP_SIZE  "gop-size"
#define ZST_X264_ENCODER_PROP_KEYINT_MIN "keyint-min"
#define ZST_X264_ENCODER_PROP_PROFILE   "profile"
#define ZST_X264_ENCODER_PROP_LEVEL     "level"
#define ZST_X264_ENCODER_PROP_FPS       "fps"
#define ZST_X264_ENCODER_PROP_THREADS   "threads"
#define ZST_X264_ENCODER_PROP_BFRAMES   "bframes"
#define ZST_X264_ENCODER_PROP_VBV_MAXRATE "vbv-maxrate"

zst_element_t* zst_x264_encoder_create(void);

#ifdef __cplusplus
}
#endif
