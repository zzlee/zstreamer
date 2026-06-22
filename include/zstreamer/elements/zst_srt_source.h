/*=============================================================================
    zst_srt_source.h — SRT Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SOURCE_FACTORY "srtsrc"


typedef struct {
    size_t struct_size;
} zst_srt_source_config_t;

zst_element_t* zst_srt_source_create(void);
zst_element_t* zst_srt_source_create_with_config(const zst_srt_source_config_t* config);

#ifdef __cplusplus
}
#endif
