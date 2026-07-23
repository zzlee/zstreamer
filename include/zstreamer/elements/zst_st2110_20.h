/*=============================================================================
    zst_st2110_20.h — SMPTE ST 2110-20 (Uncompressed Video) elements
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

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
