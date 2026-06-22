/*=============================================================================
    zst_text_source.h — Text Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_SOURCE_FACTORY "textsource"


typedef struct {
    size_t struct_size;
} zst_text_source_config_t;

zst_element_t* zst_text_source_create(void);
zst_element_t* zst_text_source_create_with_config(const zst_text_source_config_t* config);

#ifdef __cplusplus
}
#endif
