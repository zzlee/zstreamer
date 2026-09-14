/*=============================================================================
    zst_h26x_parser.h - H.264/H.265 elementary stream parser
 =============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H26X_PARSER_FACTORY "h26xparse"

/* Creates a parser that normalizes AVC/HEVC input to Annex-B. */
zst_element_t* zst_h26x_parser_create(void);

#ifdef __cplusplus
}
#endif
