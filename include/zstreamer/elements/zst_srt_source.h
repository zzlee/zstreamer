/*=============================================================================
    zst_srt_source.h — SRT Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_SOURCE_FACTORY "srtsrc"

#define ZST_SRT_SOURCE_PROP_URI                  "uri"
#define ZST_SRT_SOURCE_PROP_HOST                 "host"
#define ZST_SRT_SOURCE_PROP_PORT                 "port"
#define ZST_SRT_SOURCE_PROP_MODE                 "mode"
#define ZST_SRT_SOURCE_PROP_LATENCY              "latency"
#define ZST_SRT_SOURCE_PROP_PASSPHRASE           "passphrase"
#define ZST_SRT_SOURCE_PROP_PBKEYLEN             "pbkeylen"
#define ZST_SRT_SOURCE_PROP_STREAMID             "streamid"
#define ZST_SRT_SOURCE_PROP_PAYLOAD_SIZE         "payload-size"
#define ZST_SRT_SOURCE_PROP_TLPKTDROP            "tlpktdrop"
#define ZST_SRT_SOURCE_PROP_MAXBW                "maxbw"
#define ZST_SRT_SOURCE_PROP_RCVBUF               "rcvbuf"
#define ZST_SRT_SOURCE_PROP_SNDBUF               "sndbuf"

zst_element_t* zst_srt_source_create(void);

#ifdef __cplusplus
}
#endif
