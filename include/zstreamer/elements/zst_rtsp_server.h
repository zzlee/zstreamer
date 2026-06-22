/*=============================================================================
    zst_rtsp_server.h — Rtsp Server convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_RTSP_SERVER_FACTORY "rtsp_server"


typedef struct {
    size_t struct_size;
} zst_rtsp_server_config_t;

zst_element_t* zst_rtsp_server_create(void);
zst_element_t* zst_rtsp_server_create_with_config(const zst_rtsp_server_config_t* config);

#ifdef __cplusplus
}
#endif
