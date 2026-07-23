/*=============================================================================
    zst_st2110_30.h — ST2110-30 PCM Audio Payloader/Depayloader
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

zst_element_t* zst_st2110_30_payloader_create(void);
zst_element_t* zst_st2110_30_depayloader_create(void);

#ifdef __cplusplus
}
#endif
