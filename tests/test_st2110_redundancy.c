/*=============================================================================
    test_st2110_redundancy.c — ST2110 Redundancy Phase 3 tests
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
test_st2110_redundancy_mux_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_redundancy_mux");
    if (!el) {
        fprintf(stderr, "SKIP: st2110_redundancy_mux not registered\n");
        return 0;
    }

    CHECK(zst_element_set_property(el, "fec-enabled", "true") == ZST_OK, "set fec-enabled failed");
    CHECK(zst_element_set_property(el, "primary-addr", "239.0.0.1") == ZST_OK, "set primary-addr failed");

    zst_element_destroy(el);
    return 0;
}

static int
test_st2110_redundancy_demux_basic(void)
{
    zst_element_t* el = zst_element_factory_make("st2110_redundancy_demux");
    if (!el) return 0;

    CHECK(zst_element_set_property(el, "failover-detection-ms", "200") == ZST_OK, "set failover-detection-ms failed");
    CHECK(zst_element_set_property(el, "recovery-detection-ms", "1000") == ZST_OK, "set recovery-detection-ms failed");

    zst_element_destroy(el);
    return 0;
}

int main(void)
{
    zst_register_builtin_elements();

    if (test_st2110_redundancy_mux_basic() != 0) return 1;
    if (test_st2110_redundancy_demux_basic() != 0) return 1;

    printf("test_st2110_redundancy: PASS\n");
    return 0;
}
