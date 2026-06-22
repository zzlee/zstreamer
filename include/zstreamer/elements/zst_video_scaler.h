/*=============================================================================
    zst_video_scaler.h — Video Scaler convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VIDEO_SCALER_FACTORY "videoscaler"


typedef struct {
    size_t struct_size;
    int target_width;
    int target_height;
    const char* target_pixel_format;
} zst_video_scaler_config_t;

zst_element_t* zst_video_scaler_create(int target_width, int target_height, const char* target_pixel_format);
zst_element_t* zst_video_scaler_create_with_config(const zst_video_scaler_config_t* config);

#ifdef __cplusplus
}
#endif
