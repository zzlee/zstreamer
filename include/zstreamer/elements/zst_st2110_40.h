/*=============================================================================
    zst_st2110_40.h — SMPTE ST 2110-40 (Ancillary Data) elements
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ST2110_40_PAYLOADER_FACTORY "st2110_40_payloader"
#define ZST_ST2110_40_PAYLOADER_PROP_AUX_DATA_TYPE "aux-data-type"
#define ZST_ST2110_40_PAYLOADER_PROP_SAMPLING_FREQUENCY "sampling-frequency"

#define ZST_ST2110_40_DEPAYLOADER_FACTORY "st2110_40_depayloader"

/**
 * @brief Create an ST 2110-40 payloader element.
 *
 * Packetizes ancillary data into RTP packets.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_40_payloader_create(void);

/**
 * @brief Create an ST 2110-40 depayloader element.
 *
 * Reassembles RTP packets into ancillary data buffers.
 *
 * @return A new element, or NULL on failure.
 */
zst_element_t* zst_st2110_40_depayloader_create(void);

#ifdef __cplusplus
}
#endif
