/*=============================================================================
    zst_srt_parser.h — Srt Parser convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SRT_PARSER_FACTORY "srt_parser"

#define ZST_SRT_PARSER_PAD_SRC "src"

zst_element_t* zst_srt_parser_create(const char* path);

#ifdef __cplusplus
}
#endif
