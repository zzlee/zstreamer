/*=============================================================================
    zst_st2110_20.h — SMPTE ST 2110-20 (Uncompressed Video) elements
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ST2110_20_PAYLOADER_FACTORY "st2110_20_payloader"
#define ZST_ST2110_20_PAYLOADER_PROP_WIDTH "width"
#define ZST_ST2110_20_PAYLOADER_PROP_HEIGHT "height"
#define ZST_ST2110_20_PAYLOADER_PROP_SAMPLING "sampling"
#define ZST_ST2110_20_PAYLOADER_PROP_RTP_PT "rtp-pt"

#define ZST_ST2110_20_DEPAYLOADER_FACTORY "st2110_20_depayloader"
#define ZST_ST2110_20_DEPAYLOADER_PROP_EXPECTED_LINE_LENGTH "expected-line-length"
#define ZST_ST2110_20_DEPAYLOADER_PROP_REORDER_BUFFER_DEPTH "reorder-buffer-depth"
#define ZST_ST2110_20_DEPAYLOADER_PROP_REORDER_TIMEOUT_MS "reorder-timeout-ms"

/**
 * @brief Create an ST 2110-20 payloader element.
 *
 * Packetizes uncompressed video buffers into RFC 4175 RTP packets.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_20_payloader_create(void);

/**
 * @brief Create an ST 2110-20 depayloader element.
 *
 * Reassembles RFC 4175 RTP packets into uncompressed video buffers.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_20_depayloader_create(void);

#ifdef __cplusplus
}
#endif
