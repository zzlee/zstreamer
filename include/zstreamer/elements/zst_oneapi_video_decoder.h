/*=============================================================================
    zst_oneapi_video_decoder.h — Intel oneAPI/oneVPL Video Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ONEAPI_VIDEO_DECODER_FACTORY       "oneapidec"
#define ZST_ONEAPI_VIDEO_DECODER_FACTORY_ALIAS "oneapi_video_decoder"

#define ZST_ONEAPI_VIDEO_DECODER_PROP_CODEC    "codec"

zst_element_t* zst_oneapi_video_decoder_create(void);

#ifdef __cplusplus
}
#endif
