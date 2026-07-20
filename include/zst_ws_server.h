/*=============================================================================
    zst_ws_server.h — WebSocket signaling server public API
=============================================================================*/
#pragma once

#include "zst_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zst_ws_server_s zst_ws_server_t;

zst_ws_server_t* zst_ws_server_create(int port);
zst_result_t     zst_ws_server_start(zst_ws_server_t* srv);
zst_result_t     zst_ws_server_stop(zst_ws_server_t* srv);
void             zst_ws_server_free(zst_ws_server_t* srv);
zst_result_t     zst_ws_send(zst_ws_server_t* srv, int client_id, const char* data, size_t len);

void             zst_ws_server_set_callbacks(zst_ws_server_t* srv,
    void (*on_connect)(int client_id, void* user_data),
    void (*on_message)(int client_id, const char* msg, size_t len, void* user_data),
    void (*on_disconnect)(int client_id, void* user_data),
    void* user_data);

#ifdef __cplusplus
}
#endif
