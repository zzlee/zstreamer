/*=============================================================================
    zst_alsa_source.h — Alsa Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ALSA_SOURCE_FACTORY "alsasrc"

#define ZST_ALSA_SOURCE_PROP_DEVICE        "device"
#define ZST_ALSA_SOURCE_PROP_SAMPLE_RATE   "sample-rate"
#define ZST_ALSA_SOURCE_PROP_CHANNELS      "channels"
#define ZST_ALSA_SOURCE_PROP_SAMPLE_FORMAT "sample-format"
#define ZST_ALSA_SOURCE_PROP_LATENCY       "latency"

zst_element_t* zst_alsa_source_create(void);

#ifdef __cplusplus
}
#endif
