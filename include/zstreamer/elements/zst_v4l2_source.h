/*=============================================================================
    zst_v4l2_source.h — V4L2 Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_V4L2_SOURCE_FACTORY "v4l2src"


typedef struct {
    size_t struct_size;
} zst_v4l2_source_config_t;

zst_element_t* zst_v4l2_source_create(void);
zst_element_t* zst_v4l2_source_create_with_config(const zst_v4l2_source_config_t* config);

#ifdef __cplusplus
}
#endif
