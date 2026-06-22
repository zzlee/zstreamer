/*=============================================================================
    zst_srt_sink.h — SRT Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SINK_FACTORY "srtsink"

#define ZST_SRT_SINK_PROP_URI "uri"
#define ZST_SRT_SINK_PROP_HOST "host"
#define ZST_SRT_SINK_PROP_PORT "port"
#define ZST_SRT_SINK_PROP_MODE "mode"
#define ZST_SRT_SINK_PROP_LATENCY "latency"
#define ZST_SRT_SINK_PROP_PASSPHRASE "passphrase"
#define ZST_SRT_SINK_PROP_PBKEYLEN "pbkeylen"
#define ZST_SRT_SINK_PROP_STREAMID "streamid"
#define ZST_SRT_SINK_PROP_PAYLOAD_SIZE "payload-size"

#define ZST_SRT_SINK_PAD_SINK "sink"

zst_element_t* zst_srt_sink_create(void);

#ifdef __cplusplus
}
#endif
