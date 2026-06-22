/*=============================================================================
    zst_srt_sink.h — SRT Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SINK_FACTORY "srtsink"


typedef struct {
    size_t struct_size;
} zst_srt_sink_config_t;

zst_element_t* zst_srt_sink_create(void);
zst_element_t* zst_srt_sink_create_with_config(const zst_srt_sink_config_t* config);

#ifdef __cplusplus
}
#endif
