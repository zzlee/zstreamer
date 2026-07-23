#ifndef ZST_ST2110_REDUNDANCY_H
#define ZST_ST2110_REDUNDANCY_H

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

zst_element_t* zst_st2110_redundancy_mux_create(void);
zst_element_t* zst_st2110_redundancy_demux_create(void);

#ifdef __cplusplus
}
#endif

#endif
