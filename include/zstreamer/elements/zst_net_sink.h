/*=============================================================================
    zst_net_sink.h — Net Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NET_SINK_FACTORY "netsink"

zst_element_t* zst_net_sink_create(void);

#ifdef __cplusplus
}
#endif
