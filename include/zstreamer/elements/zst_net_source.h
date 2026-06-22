/*=============================================================================
    zst_net_source.h — Net Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NET_SOURCE_FACTORY "netsrc"

zst_element_t* zst_net_source_create(void);

#ifdef __cplusplus
}
#endif
