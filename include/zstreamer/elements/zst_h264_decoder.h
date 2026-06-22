/*=============================================================================
    zst_h264_decoder.h — H264 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H264_DECODER_FACTORY "h264dec"

#define ZST_H264_DECODER_PAD_SINK "sink"
#define ZST_H264_DECODER_PAD_SRC "src"

zst_element_t* zst_h264_decoder_create(void);

#ifdef __cplusplus
}
#endif
