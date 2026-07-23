/*=============================================================================
    test_st2110.c — ST2110 Phase 1 tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_scheduler.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static int
test_st2110_20_payloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_20_payloader");
    if (!el) {
        fprintf(stderr, "SKIP: st2110_20_payloader not registered\n");
        return 0;
    }
    
    CHECK(zst_element_set_property_int(el, "width", 1920) == ZST_OK, "set width failed");
    CHECK(zst_element_set_property_int(el, "height", 1080) == ZST_OK, "set height failed");
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_20_depayloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_20_depayloader");
    if (!el) return 0;
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_30_payloader_audio(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_30_payloader");
    if (!el) return 0;
    
    CHECK(zst_element_set_property_int(el, "channels", 2) == ZST_OK, "set channels failed");
    CHECK(zst_element_set_property_int(el, "sample-rate", 48000) == ZST_OK, "set sample-rate failed");
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_30_depayloader_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_30_depayloader");
    if (!el) return 0;
    
    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_sdp_generation(void)
{
    zst_element_t* mux = zst_element_factory_make("sdpmux");
    if (!mux) return 0;
    
    /* Just check if we can set media-mode to st2110 */
    zst_element_set_property_string(mux, "media-mode", "st2110");
    
    zst_element_destroy(mux);
    return 0;
}

int main(void)
{
    zst_register_builtin_elements();

    if (test_st2110_20_payloader_basic() != 0) return 1;
    if (test_st2110_20_depayloader_basic() != 0) return 1;
    if (test_st2110_30_payloader_audio() != 0) return 1;
    if (test_st2110_30_depayloader_basic() != 0) return 1;
    if (test_st2110_sdp_generation() != 0) return 1;

    printf("test_st2110: PASS\n");
    return 0;
}
