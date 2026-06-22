/*=============================================================================
    zst_aac_decoder.h — Aac Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_DECODER_FACTORY "aacdec"

#define ZST_AAC_DECODER_PAD_SINK "sink"
#define ZST_AAC_DECODER_PAD_SRC "src"

zst_element_t* zst_aac_decoder_create(void);

#ifdef __cplusplus
}
#endif
