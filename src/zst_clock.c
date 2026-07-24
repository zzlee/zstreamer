/*=============================================================================
    zst_clock.c - High-Precision Phase-Locked Loop (PLL) Clock Slaving
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include "zst_clock.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

zst_clock_t*
zst_clock_ref(zst_clock_t* clock)
{
    if (!clock) return NULL;
    __sync_fetch_and_add(&clock->refcount, 1);
    return clock;
}

typedef struct {
    zst_clock_t* master;
    zst_clock_t* reference;
    pthread_t    thread;
    volatile int running;

    double       alpha;            /* Active scaling factor applied to reference clock */
    double       smoothed_ratio;   /* Low-pass filtered baseline frequency ratio */
    double       integral_error;   /* PI controller error integrator */
    zst_time_t   base_master;
    zst_time_t   base_ref;

    double       phase_jitter;     /* Moving average of absolute phase error */
    double       max_phase_error;  /* Maximum observed absolute phase error */

    pthread_mutex_t lock;
} slave_clock_priv_t;

static void*
slave_clock_worker(void* arg)
{
    zst_clock_t* clock = arg;
    slave_clock_priv_t* priv = clock->priv;

    zst_time_t last_master = zst_clock_get_time(priv->master);
    zst_time_t last_ref    = zst_clock_get_time(priv->reference);

    while (__atomic_load_n(&priv->running, __ATOMIC_ACQUIRE)) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        zst_time_t now_master = zst_clock_get_time(priv->master);
        zst_time_t now_ref    = zst_clock_get_time(priv->reference);

        if (now_master > last_master && now_ref > last_ref) {
            zst_time_t diff_master = now_master - last_master;
            zst_time_t diff_ref    = now_ref - last_ref;
            double dt = (double)diff_ref / 1000000000.0;

            if (dt > 0.0) {
                /* Capture the slave clock's calculated time immediately before lock updates */
                zst_time_t current_slave_time = zst_clock_get_time(clock);
                double freq_ratio = (double)diff_master / (double)diff_ref;

                pthread_mutex_lock(&priv->lock);

                /* 1. Track baseline physical frequency difference via a low-pass filter */
                priv->smoothed_ratio = priv->smoothed_ratio * 0.9 + freq_ratio * 0.1;

                /* 2. Calculate the phase offset (error) between the master clock and slave clock in seconds */
                double phase_error = (double)((int64_t)now_master - (int64_t)current_slave_time) / 1000000000.0;
                double abs_error = phase_error > 0.0 ? phase_error : -phase_error;

                priv->phase_jitter = priv->phase_jitter * 0.9 + abs_error * 0.1;
                if (abs_error > priv->max_phase_error) {
                    priv->max_phase_error = abs_error;
                }

                /* 3. Integrate phase error over time, with windup protection limits to prevent overshoot */
                priv->integral_error += phase_error * dt;
                if (priv->integral_error > 0.5) {
                    priv->integral_error = 0.5;
                } else if (priv->integral_error < -0.5) {
                    priv->integral_error = -0.5;
                }

                /* 4. Implement PI control loop: Adjust tracking rate (alpha) based on phase and integral error */
                double Kp = 0.15;
                double Ki = 0.015;
                double correction = (Kp * phase_error) + (Ki * priv->integral_error);

                /* Cap maximum correction value to +/- 0.5% to maintain frequency tracking stability */
                if (correction > 0.005) {
                    correction = 0.005;
                } else if (correction < -0.005) {
                    correction = -0.005;
                }

                priv->alpha = priv->smoothed_ratio + correction;

                /* 5. Advance baselines cleanly. By setting base_master to the exact current_slave_time 
                 * calculated right before this lock, we mathematically eliminate time jumps or gaps. */
                priv->base_master = current_slave_time;
                priv->base_ref    = now_ref;

                pthread_mutex_unlock(&priv->lock);
            }
        }

        last_master = now_master;
        last_ref = now_ref;
    }

    return NULL;
}

static zst_time_t
slave_clock_get_time(zst_clock_t* clock)
{
    slave_clock_priv_t* priv = clock->priv;
    zst_time_t now_ref = zst_clock_get_time(priv->reference);

    pthread_mutex_lock(&priv->lock);
    double alpha = priv->alpha;
    zst_time_t base_master = priv->base_master;
    zst_time_t base_ref = priv->base_ref;
    pthread_mutex_unlock(&priv->lock);

    int64_t diff_ref = (int64_t)now_ref - (int64_t)base_ref;
    return base_master + (int64_t)((long double)diff_ref * (long double)alpha);
}

static void
slave_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    slave_clock_priv_t* priv = clock->priv;

    /* time is a relative duration from the caller's perspective.
       Convert it to the reference clock's time domain and pass
       the same relative duration to the reference clock. */
    pthread_mutex_lock(&priv->lock);
    double alpha = priv->alpha;
    pthread_mutex_unlock(&priv->lock);

    zst_time_t ref_duration = (alpha > 0.0)
        ? (zst_time_t)((long double)time / (long double)alpha)
        : time;
    zst_clock_wait(priv->reference, ref_duration);
}

static void
slave_clock_destroy(zst_clock_t* clock)
{
    slave_clock_priv_t* priv = clock->priv;
    if (priv) {
        if (__atomic_load_n(&priv->running, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&priv->running, 0, __ATOMIC_RELEASE);
            pthread_join(priv->thread, NULL);
        }
        if (priv->master) {
            zst_clock_unref(priv->master);
        }
        if (priv->reference) {
            zst_clock_unref(priv->reference);
        }
        pthread_mutex_destroy(&priv->lock);
        free(priv);
    }
}

zst_clock_t*
zst_clock_slave_create(zst_clock_t* master, zst_clock_t* reference)
{
    if (!master || !reference) return NULL;

    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    slave_clock_priv_t* priv = calloc(1, sizeof(*priv));
    if (!priv) {
        free(clock);
        return NULL;
    }

    priv->master = zst_clock_ref(master);
    priv->reference = zst_clock_ref(reference);

    /* Initialize tracking variables with safe defaults */
    priv->alpha = 1.0;
    priv->smoothed_ratio = 1.0;
    priv->integral_error = 0.0;
    priv->phase_jitter = 0.0;
    priv->max_phase_error = 0.0;
    
    zst_time_t now_master = zst_clock_get_time(master);
    zst_time_t now_ref = zst_clock_get_time(reference);
    priv->base_master = now_master;
    priv->base_ref = now_ref;

    pthread_mutex_init(&priv->lock, NULL);

    clock->refcount = 1;
    clock->get_time = slave_clock_get_time;
    clock->wait     = slave_clock_wait;
    clock->destroy  = slave_clock_destroy;
    clock->priv     = priv;

    __atomic_store_n(&priv->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&priv->thread, NULL, slave_clock_worker, clock) != 0) {
        __atomic_store_n(&priv->running, 0, __ATOMIC_RELEASE);
        zst_clock_unref(priv->master);
        zst_clock_unref(priv->reference);
        pthread_mutex_destroy(&priv->lock);
        free(priv);
        free(clock);
        return NULL;
    }

    return clock;
}

void
zst_clock_unref(zst_clock_t* clock)
{
    if (!clock) return;
    if (__sync_sub_and_fetch(&clock->refcount, 1) > 0)
        return;

    if (clock->destroy)
        clock->destroy(clock);

    free(clock);
}

zst_time_t
zst_clock_get_time(zst_clock_t* clock)
{
    if (!clock || !clock->get_time) return 0;
    return clock->get_time(clock);
}

void
zst_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    if (!clock || !clock->wait) return;
    clock->wait(clock, time);
}

static zst_time_t
system_clock_get_time(zst_clock_t* clock)
{
    (void)clock;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (zst_time_t)ts.tv_sec * 1000000000ULL + (zst_time_t)ts.tv_nsec;
}

static void
system_clock_wait(zst_clock_t* clock, zst_time_t time)
{
    (void)clock;
    struct timespec ts;
    ts.tv_sec = time / 1000000000ULL;
    ts.tv_nsec = time % 1000000000ULL;
    /* Since we wait for a duration, not an absolute time, use relative wait */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
}

static void
system_clock_destroy(zst_clock_t* clock)
{
    (void)clock;
}

zst_clock_t*
zst_clock_system_create(void)
{
    zst_clock_t* clock = calloc(1, sizeof(*clock));
    if (!clock) return NULL;

    clock->refcount = 1;
    clock->get_time = system_clock_get_time;
    clock->wait     = system_clock_wait;
    clock->destroy  = system_clock_destroy;
    clock->priv     = NULL;

    return clock;
}

zst_result_t
zst_clock_get_sync_stats(zst_clock_t* clock, double* jitter_sec_out, double* max_error_sec_out)
{
    if (!clock || !clock->priv) return ZST_ERROR;
    slave_clock_priv_t* priv = clock->priv;
    pthread_mutex_lock(&priv->lock);
    if (jitter_sec_out) *jitter_sec_out = priv->phase_jitter;
    if (max_error_sec_out) *max_error_sec_out = priv->max_phase_error;
    pthread_mutex_unlock(&priv->lock);
    return ZST_OK;
}

zst_result_t
zst_clock_set_external_reference(zst_clock_t* clock, int ref_type)
{
    if (!clock) return ZST_ERROR;
    if (ref_type == ZST_CLOCK_PTP) {
        clock->is_ptp = 1;
        return ZST_OK;
    }
    return ZST_ERROR_NOT_IMPLEMENTED;
}