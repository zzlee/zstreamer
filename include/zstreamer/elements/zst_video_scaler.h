/*=============================================================================
    zst_video_scaler.h — Video Scaler convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VIDEO_SCALER_FACTORY "videoscaler"

#define ZST_VIDEO_SCALER_PAD_SINK "sink"
#define ZST_VIDEO_SCALER_PAD_SRC "src"

zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);

#ifdef __cplusplus
}
#endif
