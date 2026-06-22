/*=============================================================================
    zst_aac_encoder.h — Aac Encoder convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AAC_ENCODER_FACTORY "aacenc"


typedef struct {
    size_t struct_size;
} zst_aac_encoder_config_t;

zst_element_t* zst_aac_encoder_create(void);
zst_element_t* zst_aac_encoder_create_with_config(const zst_aac_encoder_config_t* config);

#ifdef __cplusplus
}
#endif
