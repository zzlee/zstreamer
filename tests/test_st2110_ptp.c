/*=============================================================================
    test_st2110_ptp.c — ST2110 Phase 2 PTP Timing tests
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_bus.h"
#include "zst_clock.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", (msg)); \
        return 1; \
    } \
} while (0)

static int
test_ptp_clock_properties(void)
{
    zst_element_t* el = zst_element_factory_make("ptp_clock");
    if (!el) {
        fprintf(stderr, "SKIP: ptp_clock not registered\n");
        return 0;
    }
    
    CHECK(zst_element_set_property_string(el, "ptp-interface", "/dev/ptp1") == ZST_OK, "set interface failed");
    CHECK(zst_element_set_property_int(el, "ptp-domain", 127) == ZST_OK, "set domain failed");
    CHECK(zst_element_set_property_bool(el, "ptp-slave-only", false) == ZST_OK, "set slave-only failed");
    CHECK(zst_element_set_property_string(el, "ptp-mode", "master") == ZST_OK, "set mode failed");
    
    char val[64];
    CHECK(zst_element_get_property_string(el, "ptp-interface", val, sizeof(val)) == ZST_OK, "get interface failed");
    CHECK(strcmp(val, "/dev/ptp1") == 0, "interface mismatch");
    
    int64_t domain_val;
    CHECK(zst_element_get_property_int(el, "ptp-domain", &domain_val) == ZST_OK, "get domain failed");
    CHECK(domain_val == 127, "domain mismatch");
    
    zst_element_destroy(el);
    return 0;
}

static int
test_ptp_clock_sync(void)
{
    zst_element_t* el = zst_element_factory_make("ptp_clock");
    if (!el) return 0;
    
    /* Set state to READY to trigger open() */
    CHECK(zst_element_set_state(el, ZST_STATE_READY) == ZST_OK, "open failed");
    
    zst_clock_t* clk = NULL;
    if (el->ops && el->ops->provide_clock) {
        clk = el->ops->provide_clock(el);
    }
    CHECK(clk != NULL, "provide_clock failed");
    
    /* Check time */
    zst_time_t t1 = zst_clock_get_time(clk);
    CHECK(t1 > 0, "get_time returned invalid time");
    
    /* Check wait */
    zst_time_t target = t1 + 1000000ULL; // 1 millisecond
    zst_clock_wait(clk, target);
    zst_time_t t2 = zst_clock_get_time(clk);
    CHECK(t2 >= target, "wait returned too early");
    
    zst_clock_unref(clk);
    
    /* Check event posting on start */
    zst_bus_t* bus = zst_bus_create();
    el->bus = bus;
    
    CHECK(zst_element_set_state(el, ZST_STATE_PLAYING) == ZST_OK, "start failed");
    
    int found_sync = 0;
    while (!found_sync) {
        zst_event_t* ev = NULL;
        if (zst_bus_pop(bus, &ev, 100) != ZST_OK || !ev) {
            break;
        }
        if (ev->type == ZST_EVENT_CLOCK_SYNC) {
            found_sync = 1;
        }
        zst_event_destroy(ev);
    }
    CHECK(found_sync == 1, "no sync event posted on start");
    
    zst_element_set_state(el, ZST_STATE_NULL);
    zst_bus_destroy(bus);
    zst_element_destroy(el);
    return 0;
}

int main(void)
{
    zst_register_builtin_elements();

    if (test_ptp_clock_properties() != 0) return 1;
    if (test_ptp_clock_sync() != 0) return 1;

    printf("test_st2110_ptp: PASS\n");
    return 0;
}
