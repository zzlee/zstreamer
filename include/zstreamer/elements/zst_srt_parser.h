/*=============================================================================
    zst_srt_parser.h — Srt Parser convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_PARSER_FACTORY "srt_parser"


typedef struct {
    size_t struct_size;
    const char* path;
} zst_srt_parser_config_t;

zst_element_t* zst_srt_parser_create(const char* path);
zst_element_t* zst_srt_parser_create_with_config(const zst_srt_parser_config_t* config);

#ifdef __cplusplus
}
#endif
