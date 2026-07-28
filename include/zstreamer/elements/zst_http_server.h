/*=============================================================================
    zst_http_server.h — HTTP Server convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_HTTP_SERVER_FACTORY "http_server"

#define ZST_HTTP_SERVER_PROP_PORT          "port"
#define ZST_HTTP_SERVER_PROP_DOCUMENT_ROOT "document-root"

typedef struct {
    size_t struct_size;
    int port;
    const char* document_root;
} zst_http_server_config_t;

zst_element_t* zst_http_server_element_create(void);
zst_element_t* zst_http_server_element_create_with_config(const zst_http_server_config_t* config);

#ifdef __cplusplus
}
#endif
