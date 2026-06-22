/*=============================================================================
    zst_aac_decoder.h — Aac Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_DECODER_FACTORY "aacdec"


typedef struct {
    size_t struct_size;
} zst_aac_decoder_config_t;

zst_element_t* zst_aac_decoder_create(void);
zst_element_t* zst_aac_decoder_create_with_config(const zst_aac_decoder_config_t* config);

#ifdef __cplusplus
}
#endif
