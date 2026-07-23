/*=============================================================================
    zst_st2110_30.h — ST2110-30 PCM Audio Payloader/Depayloader
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_ST2110_30_PAYLOADER_FACTORY "st2110_30_payloader"
#define ZST_ST2110_30_PAYLOADER_PROP_CHANNELS "channels"
#define ZST_ST2110_30_PAYLOADER_PROP_SAMPLE_RATE "sample-rate"
#define ZST_ST2110_30_PAYLOADER_PROP_BIT_DEPTH "bit-depth"
#define ZST_ST2110_30_PAYLOADER_PROP_RTP_PT "rtp-pt"

#define ZST_ST2110_30_DEPAYLOADER_FACTORY "st2110_30_depayloader"
#define ZST_ST2110_30_DEPAYLOADER_PROP_EXPECTED_CHANNELS "expected-channels"
#define ZST_ST2110_30_DEPAYLOADER_PROP_EXPECTED_SAMPLE_RATE "expected-sample-rate"

zst_element_t* zst_st2110_30_payloader_create(void);
zst_element_t* zst_st2110_30_depayloader_create(void);

#ifdef __cplusplus
}
#endif
