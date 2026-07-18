/*=============================================================================
    zst_vp8_decoder.h — VP8 Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_VP8_DECODER_FACTORY "vp8dec"

#define ZST_VP8_DECODER_PROP_THREADS  "threads"

zst_element_t* zst_vp8_decoder_create(void);

#ifdef __cplusplus
}
#endif
