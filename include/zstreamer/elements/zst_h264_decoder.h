/*=============================================================================
    zst_h264_decoder.h — H264 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H264_DECODER_FACTORY "h264dec"


typedef struct {
    size_t struct_size;
} zst_h264_decoder_config_t;

zst_element_t* zst_h264_decoder_create(void);
zst_element_t* zst_h264_decoder_create_with_config(const zst_h264_decoder_config_t* config);

#ifdef __cplusplus
}
#endif
