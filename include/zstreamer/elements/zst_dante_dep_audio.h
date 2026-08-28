#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_DANTE_DEP_AUDIO_SOURCE_FACTORY "dantedepaudiosrc"
#define ZST_DANTE_DEP_AUDIO_SINK_FACTORY   "dantedepaudiosink"

#define ZST_DANTE_DEP_AUDIO_PROP_SHM_NAME          "shm-name"
#define ZST_DANTE_DEP_AUDIO_PROP_CHANNELS          "channels"
#define ZST_DANTE_DEP_AUDIO_PROP_QUEUE_PERIODS     "queue-periods"
#define ZST_DANTE_DEP_AUDIO_PROP_BLOCK_SAMPLES     "block-samples"
#define ZST_DANTE_DEP_AUDIO_PROP_TX_LEAD_US        "tx-lead-us"
#define ZST_DANTE_DEP_AUDIO_PROP_RECONNECT_MS      "reconnect-interval-ms"
#define ZST_DANTE_DEP_AUDIO_PROP_SAMPLE_RATE       "sample-rate"
#define ZST_DANTE_DEP_AUDIO_PROP_EXPECTED_SAMPLE_RATE "expected-sample-rate"
#define ZST_DANTE_DEP_AUDIO_PROP_ACTIVE            "active"
#define ZST_DANTE_DEP_AUDIO_PROP_PERIODS           "periods"
#define ZST_DANTE_DEP_AUDIO_PROP_RESETS             "resets"
#define ZST_DANTE_DEP_AUDIO_PROP_OVERRUNS           "overruns"
#define ZST_DANTE_DEP_AUDIO_PROP_UNDERFLOWS         "underflows"
#define ZST_DANTE_DEP_AUDIO_PROP_PERIOD_COUNT       "period-count"
#define ZST_DANTE_DEP_AUDIO_PROP_RESET_COUNT        "reset-count"
#define ZST_DANTE_DEP_AUDIO_PROP_OVERRUN_COUNT      "overrun-count"
#define ZST_DANTE_DEP_AUDIO_PROP_UNDERFLOW_COUNT    "underflow-count"

zst_element_t* zst_dante_dep_audio_source_create(void);
zst_element_t* zst_dante_dep_audio_sink_create(void);

#ifdef __cplusplus
}
#endif
