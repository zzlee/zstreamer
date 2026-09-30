/*=============================================================================
    zst_aac_decoder.h — Aac Decoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_DECODER_FACTORY "aacdec"

#define ZST_AAC_DECODER_PROP_THREADS  "threads"

/* Optional "decoder" string property: "aac" (default) or "aac_fixed".
 * Set before the first packet. Output format retains AVSampleFormat values;
 * aac_fixed normally emits planar S32 rather than planar float. */
zst_element_t* zst_aac_decoder_create(void);

#ifdef __cplusplus
}
#endif
