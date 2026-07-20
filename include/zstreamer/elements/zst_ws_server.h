/*=============================================================================
    zst_ws_server.h — WebSocket Server convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_WS_SERVER_FACTORY "ws_server"

#define ZST_WS_SERVER_PROP_PORT "port"

zst_element_t* zst_ws_server_element_create(void);

#ifdef __cplusplus
}
#endif
