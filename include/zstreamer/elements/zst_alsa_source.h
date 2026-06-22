/*=============================================================================
    zst_alsa_source.h — Alsa Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ALSA_SOURCE_FACTORY "alsasrc"


typedef struct {
    size_t struct_size;
} zst_alsa_source_config_t;

zst_element_t* zst_alsa_source_create(void);
zst_element_t* zst_alsa_source_create_with_config(const zst_alsa_source_config_t* config);

#ifdef __cplusplus
}
#endif
