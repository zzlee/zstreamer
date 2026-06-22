/*=============================================================================
    zst_h264_encoder.h — H264 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H264_ENCODER_FACTORY "h264enc"

#define ZST_H264_ENCODER_PAD_SINK "sink"
#define ZST_H264_ENCODER_PAD_SRC "src"

zst_element_t* zst_h264_encoder_create(void);

#ifdef __cplusplus
}
#endif
