/*=============================================================================
    zst_st2110_21.h — SMPTE ST 2110-21 Elements
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

zst_element_t* zst_st2110_21_payloader_create(void);
zst_element_t* zst_st2110_21_depayloader_create(void);

#ifdef __cplusplus
}
#endif
