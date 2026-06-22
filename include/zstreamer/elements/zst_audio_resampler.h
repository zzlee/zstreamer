/*=============================================================================
    zst_audio_resampler.h — Audio Resampler convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_AUDIO_RESAMPLER_FACTORY "audioresampler"


typedef struct {
    size_t struct_size;
    int target_sample_rate;
    int target_channels;
    const char* target_sample_format;
} zst_audio_resampler_config_t;

zst_element_t* zst_audio_resampler_create(int target_sample_rate, int target_channels, const char* target_format);
zst_element_t* zst_audio_resampler_create_with_config(const zst_audio_resampler_config_t* config);

#ifdef __cplusplus
}
#endif
