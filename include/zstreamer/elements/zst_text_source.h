/*=============================================================================
    zst_text_source.h — Text Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_TEXT_SOURCE_FACTORY "textsource"

#define ZST_TEXTSOURCE_PROP_WIDTH "width"
#define ZST_TEXTSOURCE_PROP_HEIGHT "height"
#define ZST_TEXTSOURCE_PROP_FPS "fps"
#define ZST_TEXTSOURCE_PROP_TEXT "text"
#define ZST_TEXTSOURCE_PROP_TEXT_CONTENT "text-content"
#define ZST_TEXTSOURCE_PROP_FONT_SIZE "font-size"
#define ZST_TEXTSOURCE_PROP_FONT_SIZE "font_size"
#define ZST_TEXTSOURCE_PROP_FONT_PATH "font-path"
#define ZST_TEXTSOURCE_PROP_FONT_PATH "font_path"
#define ZST_TEXTSOURCE_PROP_BG_COLOR "bg-color"
#define ZST_TEXTSOURCE_PROP_BACKGROUND_COLOR "background-color"
#define ZST_TEXTSOURCE_PROP_TEXT_COLOR "text-color"
#define ZST_TEXTSOURCE_PROP_COLOR "color"
#define ZST_TEXTSOURCE_PROP_TEXT_COLOR "text_color"
#define ZST_TEXTSOURCE_PROP_PIXEL_FORMAT "pixel-format"
#define ZST_TEXTSOURCE_PROP_PIXEL_FORMAT "pixel_format"
#define ZST_TEXTSOURCE_PROP_NUM_BUFFERS "num-buffers"
#define ZST_TEXTSOURCE_PROP_NUM_BUFFERS "num_buffers"
#define ZST_TEXTSOURCE_PROP_LOOP "loop"
#define ZST_TEXTSOURCE_PROP_USE_CLOCK "use-clock"
#define ZST_TEXTSOURCE_PROP_DO_TIMESTAMP "do-timestamp"
#define ZST_TEXTSOURCE_PROP_X "x"
#define ZST_TEXTSOURCE_PROP_Y "y"

#define ZST_TEXTSOURCE_PAD_SRC "src"

zst_element_t* zst_text_source_create(void);

#ifdef __cplusplus
}
#endif
