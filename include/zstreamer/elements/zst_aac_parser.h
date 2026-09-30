/* AAC access-unit to ADTS framing element. */
#pragma once
#include "zst_element.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZST_AAC_PARSER_FACTORY "aacparse"
#define ZST_AAC_PARSER_PROP_CONFIG "config" /* AudioSpecificConfig as hex, e.g. 1210 */
zst_element_t* zst_aac_parser_create(void);
#ifdef __cplusplus
}
#endif
