/*=============================================================================
    zst_st2110_22.h — SMPTE ST 2110-22 (JPEG XS Video) elements
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ST2110_22_PAYLOADER_FACTORY "st2110_22_payloader"
#define ZST_ST2110_22_PAYLOADER_PROP_WIDTH "width"
#define ZST_ST2110_22_PAYLOADER_PROP_HEIGHT "height"
#define ZST_ST2110_22_PAYLOADER_PROP_FPS_NUM "fps-num"
#define ZST_ST2110_22_PAYLOADER_PROP_FPS_DEN "fps-den"
#define ZST_ST2110_22_PAYLOADER_PROP_BPP "bpp"

#define ZST_ST2110_22_DEPAYLOADER_FACTORY "st2110_22_depayloader"

/**
 * @brief Create an ST 2110-22 payloader element.
 *
 * Encodes uncompressed video using JPEG-XS and packetizes into RTP packets.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_22_payloader_create(void);

/**
 * @brief Create an ST 2110-22 depayloader element.
 *
 * Depayloads RTP packets and decodes JPEG-XS into uncompressed video.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_22_depayloader_create(void);

#ifdef __cplusplus
}
#endif
