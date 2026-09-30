/*=============================================================================
    zst_h26x_parser.h - H.264/H.265 elementary stream parser
 =============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_H26X_PARSER_FACTORY "h26xparse"
#define ZST_H26X_PARSER_CAPS_AVCC "codec_data-avcc"
#define ZST_H26X_PARSER_CAPS_FRAMERATE "framerate"

/* Creates a parser that normalizes AVC/HEVC input to Annex-B. H.264 output
 * caps retain Annex-B codec_data and additionally expose avcC, dimensions,
 * and the SPS VUI frame rate when available. */
zst_element_t* zst_h26x_parser_create(void);

#ifdef __cplusplus
}
#endif
