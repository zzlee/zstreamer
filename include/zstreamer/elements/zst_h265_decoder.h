/*=============================================================================
    zst_h265_decoder.h — H265 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H265_DECODER_FACTORY "h265dec"


typedef struct {
    size_t struct_size;
} zst_h265_decoder_config_t;

zst_element_t* zst_h265_decoder_create(void);
zst_element_t* zst_h265_decoder_create_with_config(const zst_h265_decoder_config_t* config);

#ifdef __cplusplus
}
#endif
