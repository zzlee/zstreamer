/*=============================================================================
    zst_mpegts_demuxer.h — MPEG-TS Demuxer convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_MPEGTS_DEMUXER_FACTORY "tsdemux"

#define ZST_TSDEMUX_PROP_LOCATION "location"

#define ZST_TSDEMUX_PAD_SINK "sink"
#define ZST_TSDEMUX_PAD_VIDEO "video"
#define ZST_TSDEMUX_PAD_AUDIO "audio"
#define ZST_MPEGTS_DEMUXER_PROP_LOCATION     "location"

typedef struct {
    size_t struct_size;
    const char* location;
} zst_mpegts_demuxer_config_t;

zst_element_t* zst_mpegts_demuxer_create(void);
zst_element_t* zst_mpegts_demuxer_create_with_config(const zst_mpegts_demuxer_config_t* config);

#ifdef __cplusplus
}
#endif
