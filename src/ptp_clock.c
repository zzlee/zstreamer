#define _POSIX_C_SOURCE 200809L

#include "zstreamer/elements/zst_ptp_clock.h"
#include "zst_clock.h"
#include "zst_log.h"
#include "zst_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define CLOCKFD 3
#define FD_TO_CLOCKID(fd)   ((~(clockid_t) (fd) << 3) | CLOCKFD)

typedef struct {
    char ptp_interface[64];
    int ptp_domain;
    int ptp_slave_only;
    char ptp_mode[32];

    zst_clock_t* provided_clock;
} ptp_clock_t;

static zst_time_t ptp_custom_get_time(zst_clock_t* clock) {
    int fd = (int)(intptr_t)clock->priv;
    struct timespec ts;
    if (fd >= 0) {
        clockid_t clkid = FD_TO_CLOCKID(fd);
        if (clock_gettime(clkid, &ts) == 0) {
            return (zst_time_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        }
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    return (zst_time_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void ptp_custom_wait(zst_clock_t* clock, zst_time_t time) {
    int fd = (int)(intptr_t)clock->priv;
    zst_time_t now = ptp_custom_get_time(clock);
    if (now >= time) return;
    
    zst_time_t diff = time - now;
    struct timespec ts;
    ts.tv_sec = diff / 1000000000ULL;
    ts.tv_nsec = diff % 1000000000ULL;
    
    if (fd >= 0) {
        clockid_t clkid = FD_TO_CLOCKID(fd);
        clock_nanosleep(clkid, 0, &ts, NULL);
    } else {
        nanosleep(&ts, NULL);
    }
}

static void ptp_custom_destroy(zst_clock_t* clock) {
    int fd = (int)(intptr_t)clock->priv;
    if (fd >= 0) {
        close(fd);
    }
    free(clock);
}

static zst_result_t
ptp_clock_open(zst_element_t* el)
{
    ptp_clock_t* s = el->priv;
    
    int fd = -1;
    if (strncmp(s->ptp_interface, "/dev/ptp", 8) == 0) {
        fd = open(s->ptp_interface, O_RDONLY);
        if (fd >= 0) {
            ZST_LOG_INFO("ptp_clock", "Opened PTP hardware clock %s", s->ptp_interface);
        } else {
            ZST_LOG_WARN("ptp_clock", "Failed to open %s, falling back to CLOCK_REALTIME", s->ptp_interface);
        }
    }
    
    zst_clock_t* clk = calloc(1, sizeof(*clk));
    if (!clk) {
        if (fd >= 0) close(fd);
        return ZST_ERROR;
    }
    
    clk->refcount = 1;
    clk->get_time = ptp_custom_get_time;
    clk->wait = ptp_custom_wait;
    clk->destroy = ptp_custom_destroy;
    clk->priv = (void*)(intptr_t)fd;
    clk->is_ptp = 1;
    
    s->provided_clock = clk;
    return ZST_OK;
}

static zst_result_t
ptp_clock_close(zst_element_t* el)
{
    ptp_clock_t* s = el->priv;
    if (s->provided_clock) {
        zst_clock_unref(s->provided_clock);
        s->provided_clock = NULL;
    }
    return ZST_OK;
}

static zst_result_t
ptp_clock_start(zst_element_t* el)
{
    if (el->bus) {
        zst_bus_post(el->bus, zst_event_new_clock_sync(el));
    }
    return ZST_OK;
}

static zst_clock_t*
ptp_clock_provide_clock(zst_element_t* el)
{
    ptp_clock_t* s = el->priv;
    if (s->provided_clock) {
        return zst_clock_ref(s->provided_clock);
    }
    return NULL;
}

static zst_result_t
ptp_clock_set_property(zst_element_t* el, const char* name, const char* value)
{
    ptp_clock_t* s = el->priv;
    if (!name || !value) return ZST_ERROR;

    if (strcmp(name, "ptp-interface") == 0) {
        strncpy(s->ptp_interface, value, sizeof(s->ptp_interface) - 1);
        return ZST_OK;
    }
    if (strcmp(name, "ptp-domain") == 0) {
        s->ptp_domain = atoi(value);
        return ZST_OK;
    }
    if (strcmp(name, "ptp-slave-only") == 0) {
        s->ptp_slave_only = (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    if (strcmp(name, "ptp-mode") == 0) {
        strncpy(s->ptp_mode, value, sizeof(s->ptp_mode) - 1);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
ptp_clock_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    ptp_clock_t* s = el->priv;
    if (!name || !value_out) return ZST_ERROR;

    if (strcmp(name, "ptp-interface") == 0) {
        snprintf(value_out, max_len, "%s", s->ptp_interface);
    } else if (strcmp(name, "ptp-domain") == 0) {
        snprintf(value_out, max_len, "%d", s->ptp_domain);
    } else if (strcmp(name, "ptp-slave-only") == 0) {
        snprintf(value_out, max_len, "%s", s->ptp_slave_only ? "true" : "false");
    } else if (strcmp(name, "ptp-mode") == 0) {
        snprintf(value_out, max_len, "%s", s->ptp_mode);
    } else {
        return ZST_ERROR;
    }
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name = "ptp_clock",
    .open = ptp_clock_open,
    .close = ptp_clock_close,
    .start = ptp_clock_start,
    .provide_clock = ptp_clock_provide_clock,
    .set_property = ptp_clock_set_property,
    .get_property = ptp_clock_get_property,
};

zst_element_t*
zst_ptp_clock_create(void)
{
    ptp_clock_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    strncpy(s->ptp_interface, "/dev/ptp0", sizeof(s->ptp_interface) - 1);
    s->ptp_domain = 0;
    s->ptp_slave_only = 1;
    strncpy(s->ptp_mode, "slave", sizeof(s->ptp_mode) - 1);

    zst_element_t* el = zst_element_create(&g_ops, s);
    if (!el) {
        free(s);
        return NULL;
    }
    return el;
}
