/*=============================================================================
    zst_mpegts_muxer.h — MPEG-TS Muxer convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_MPEGTS_MUXER_FACTORY "tsmux"

#define ZST_TSMUX_PROP_WIDTH "width"
#define ZST_TSMUX_PROP_HEIGHT "height"
#define ZST_TSMUX_PROP_FPS "fps"
#define ZST_TSMUX_PROP_SAMPLE_RATE "sample-rate"
#define ZST_TSMUX_PROP_CHANNELS "channels"
#define ZST_TSMUX_PROP_LOCATION "location"

#define ZST_TSMUX_PAD_VIDEO "video"
#define ZST_TSMUX_PAD_AUDIO "audio"
#define ZST_TSMUX_PAD_SRC "src"
#define ZST_MPEGTS_MUXER_PROP_WIDTH        "width"
#define ZST_MPEGTS_MUXER_PROP_HEIGHT       "height"
#define ZST_MPEGTS_MUXER_PROP_FPS          "fps"
#define ZST_MPEGTS_MUXER_PROP_SAMPLE_RATE  "sample-rate"
#define ZST_MPEGTS_MUXER_PROP_CHANNELS     "channels"
#define ZST_MPEGTS_MUXER_PROP_LOCATION     "location"

typedef struct {
    size_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t sample_rate;
    uint32_t channels;
    const char* location;
} zst_mpegts_muxer_config_t;

zst_element_t* zst_mpegts_muxer_create(void);
zst_element_t* zst_mpegts_muxer_create_with_config(const zst_mpegts_muxer_config_t* config);

#ifdef __cplusplus
}
#endif
