/*=============================================================================
    zst_hls_sink.h — HLS Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_HLS_SINK_FACTORY "hls_sink"

#define ZST_HLS_SINK_PROP_WIDTH           "width"
#define ZST_HLS_SINK_PROP_HEIGHT          "height"
#define ZST_HLS_SINK_PROP_FPS             "fps"
#define ZST_HLS_SINK_PROP_SAMPLE_RATE     "sample-rate"
#define ZST_HLS_SINK_PROP_CHANNELS        "channels"
#define ZST_HLS_SINK_PROP_LOCATION        "location"
#define ZST_HLS_SINK_PROP_VIDEO_CODEC     "video-codec"
#define ZST_HLS_SINK_PROP_AUDIO_CODEC     "audio-codec"
#define ZST_HLS_SINK_PROP_TARGET_DURATION "target-duration"
#define ZST_HLS_SINK_PROP_PLAYLIST_LENGTH "playlist-length"
#define ZST_HLS_SINK_PROP_FORMAT          "format"

typedef struct {
    size_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t sample_rate;
    uint32_t channels;
    const char* location;
    const char* video_codec;
    const char* audio_codec;
    int target_duration;
    int playlist_length;
    const char* format;
} zst_hls_sink_config_t;

zst_element_t* zst_hls_sink_create(void);
zst_element_t* zst_hls_sink_create_with_config(const zst_hls_sink_config_t* config);

#ifdef __cplusplus
}
#endif
