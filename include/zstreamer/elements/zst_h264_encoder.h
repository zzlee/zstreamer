/*=============================================================================
    zst_h264_encoder.h — H264 Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H264_ENCODER_FACTORY "h264enc"


typedef struct {
    size_t struct_size;
} zst_h264_encoder_config_t;

zst_element_t* zst_h264_encoder_create(void);
zst_element_t* zst_h264_encoder_create_with_config(const zst_h264_encoder_config_t* config);

#ifdef __cplusplus
}
#endif
