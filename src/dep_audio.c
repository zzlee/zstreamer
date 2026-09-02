/**
 * @file dep_audio.c
 * @brief DEP Audio shared memory implementation — C port of Audinate dep_audio.
 *
 * Ported from:
 *   DanteBuffers.cpp  (Buffers class)
 *   DanteSharedMemory.cpp (SharedMemory class)
 *   DanteTiming.cpp   (Timing class)
 *   DanteRunner.cpp   (Runner class)
 *   DantePriority.cpp (Priority helper)
 *
 * Original: Copyright © 2020-2023 Audinate Pty Ltd ACN 120 828 006 (Audinate).
 * Ported to C for zstreamer.
 */

#include "dep_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================
 * SharedMemory
 * ==================================================================== */

int dep_audio_shm_open(dep_audio_shm_t* shm, const char* name)
{
    if (!shm || !name || strlen(name) == 0 || strlen(name) > 30)
        return EINVAL;

    memset(shm, 0, sizeof(*shm));
    shm->fd = -1;

    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return errno;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return err;
    }
    if (st.st_size == 0) {
        /* Creator has not yet resized the memory, try again later */
        close(fd);
        return EAGAIN;
    }

    void* data = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        int err = errno;
        close(fd);
        return err;
    }

    shm->fd   = fd;
    shm->size = st.st_size;
    shm->data = data;
    return 0;
}

void dep_audio_shm_close(dep_audio_shm_t* shm)
{
    if (!shm) return;
    if (shm->data && shm->size) {
        munmap(shm->data, shm->size);
        shm->data = NULL;
        shm->size = 0;
    }
    if (shm->fd >= 0) {
        close(shm->fd);
        shm->fd = -1;
    }
}

/* ====================================================================
 * Buffers
 * ==================================================================== */

void dep_audio_buffers_init(dep_audio_buffers_t* buf)
{
    memset(buf, 0, sizeof(*buf));
    buf->control.fd = buf->tx.fd = buf->rx.fd = -1;
}

int dep_audio_buffers_connect(dep_audio_buffers_t* buf, const char* name,
                               bool global_namespace)
{
    if (!buf || !name) return EINVAL;
    if (buf->connected) return EALREADY;

    buf->global_namespace = global_namespace;

    /* Open the main control SHM */
    int rc = dep_audio_shm_open(&buf->control, name);
    if (rc) return rc;

    const dep_audio_buffer_header_t* hdr =
        (const dep_audio_buffer_header_t*)buf->control.data;
    buf->header = hdr;

    /* Locate timing subheader */
    if (hdr->metadata.timing_object_subheader_offset_bytes) {
        buf->timing_obj = (const dep_audio_timing_object_t*)
            ((uint8_t*)buf->control.data +
             hdr->metadata.timing_object_subheader_offset_bytes);
    }

    /* Allocate channel pointer arrays */
    buf->num_tx_channels = hdr->audio.num_tx_channels;
    buf->num_rx_channels = hdr->audio.num_rx_channels;

    if (buf->num_tx_channels > 0) {
        buf->tx_channels = calloc(buf->num_tx_channels, sizeof(void*));
        if (!buf->tx_channels) { dep_audio_buffers_disconnect(buf); return ENOMEM; }
    }
    if (buf->num_rx_channels > 0) {
        buf->rx_channels = calloc(buf->num_rx_channels, sizeof(void*));
        if (!buf->rx_channels) { dep_audio_buffers_disconnect(buf); return ENOMEM; }
    }

    /* Resolve channel data base addresses */
    uint8_t* tx_base;
    uint8_t* rx_base;

    if (hdr->metadata.flags & DEP_AUDIO_FLAG_SEPARATE_MEMORY) {
        /* Separate TX/RX SHM regions: name+"Tx" / name+"Rx" */
        size_t base_len = strlen(name);
        char* tx_name = malloc(base_len + 3);
        char* rx_name = malloc(base_len + 3);
        if (!tx_name || !rx_name) {
            free(tx_name); free(rx_name);
            dep_audio_buffers_disconnect(buf);
            return ENOMEM;
        }
        memcpy(tx_name, name, base_len);
        memcpy(tx_name + base_len, "Tx", 3);
        memcpy(rx_name, name, base_len);
        memcpy(rx_name + base_len, "Rx", 3);

        rc = dep_audio_shm_open(&buf->tx, tx_name);
        free(tx_name);
        if (rc) { dep_audio_buffers_disconnect(buf); return rc; }

        rc = dep_audio_shm_open(&buf->rx, rx_name);
        free(rx_name);
        if (rc) { dep_audio_buffers_disconnect(buf); return rc; }

        tx_base = (uint8_t*)buf->tx.data +
                  hdr->metadata.first_tx_channel_offset_bytes;
        rx_base = (uint8_t*)buf->rx.data +
                  hdr->metadata.first_rx_channel_offset_bytes;
    } else {
        /* Single contiguous SHM region */
        tx_base = (uint8_t*)buf->control.data +
                  hdr->metadata.first_tx_channel_offset_bytes;
        rx_base = (uint8_t*)buf->control.data +
                  hdr->metadata.first_rx_channel_offset_bytes;
    }

    /* Build per-channel pointers */
    for (unsigned int c = 0; c < buf->num_tx_channels; c++)
        buf->tx_channels[c] = tx_base + c * hdr->audio.bytes_per_channel;
    for (unsigned int c = 0; c < buf->num_rx_channels; c++)
        buf->rx_channels[c] = rx_base + c * hdr->audio.bytes_per_channel;

    buf->connected = true;
    return 0;
}

void dep_audio_buffers_disconnect(dep_audio_buffers_t* buf)
{
    if (!buf) return;
    dep_audio_shm_close(&buf->control);
    dep_audio_shm_close(&buf->tx);
    dep_audio_shm_close(&buf->rx);
    free(buf->tx_channels);
    free(buf->rx_channels);
    buf->tx_channels = NULL;
    buf->rx_channels = NULL;
    buf->header = NULL;
    buf->timing_obj = NULL;
    buf->connected = false;
}

void* dep_audio_buffers_get_tx_channel(const dep_audio_buffers_t* buf,
                                        unsigned int index)
{
    if (!buf || !buf->connected || index >= buf->num_tx_channels) return NULL;
    return buf->tx_channels[index];
}

void* dep_audio_buffers_get_rx_channel(const dep_audio_buffers_t* buf,
                                        unsigned int index)
{
    if (!buf || !buf->connected || index >= buf->num_rx_channels) return NULL;
    return buf->rx_channels[index];
}

/* ====================================================================
 * Timing
 * ==================================================================== */

int dep_audio_timing_open(dep_audio_timing_t* t,
                           const dep_audio_timing_object_t* subheader,
                           bool global_namespace)
{
    if (!t) return EINVAL;
    (void)global_namespace;

    t->semaphore = SEM_FAILED;
    t->type = DEP_AUDIO_TIMING_NONE;

    if (!subheader) return 0;

    t->type = subheader->object_type;

    if (t->type == DEP_AUDIO_TIMING_SIGNAL_EVENT) {
        t->semaphore = sem_open(subheader->object_name, 0);
        if (t->semaphore == SEM_FAILED) return errno;
    }

    return 0;
}

void dep_audio_timing_close(dep_audio_timing_t* t)
{
    if (!t) return;
    if (t->semaphore != SEM_FAILED) {
        sem_close(t->semaphore);
        t->semaphore = SEM_FAILED;
    }
}

int dep_audio_timing_wait(dep_audio_timing_t* t)
{
    if (!t) return EINVAL;

    switch (t->type) {
    case DEP_AUDIO_TIMING_SIGNAL_EVENT: {
        struct ts {
            time_t tv_sec;
            long   tv_nsec;
        } timeout;
        clock_gettime(CLOCK_REALTIME, (struct timespec*)&timeout);
        timeout.tv_sec += 1;
        if (sem_timedwait(t->semaphore, (struct timespec*)&timeout) == -1)
            return errno;
        break;
    }
    default:
        usleep(1000);
        break;
    }
    return 0;
}

/* ====================================================================
 * Runner
 * ==================================================================== */

#define RUNNER_INACTIVE_MAX_SECS 1

static uint64_t get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int dep_audio_runner_run(dep_audio_runner_t* runner,
                          dep_audio_transfer_fn transfer_fn,
                          dep_audio_reset_fn reset_fn,
                          dep_audio_active_changed_fn active_changed_fn,
                          void* user_data)
{
    if (!runner || !runner->buffers || !runner->buffers->connected)
        return EINVAL;

    runner->transfer_fn       = transfer_fn;
    runner->reset_fn          = reset_fn;
    runner->active_changed_fn = active_changed_fn;
    runner->user_data         = user_data;
    runner->running           = true;
    runner->is_active         = false;

    runner->header = runner->buffers->header;
    if (!runner->header) return EINVAL;

    dep_audio_timing_t timing;
    memset(&timing, 0, sizeof(timing));

    int rc = dep_audio_timing_open(&timing, runner->buffers->timing_obj,
                                    runner->buffers->global_namespace);
    if (rc) {
        fprintf(stderr, "[dep_audio] Error opening timing object: %s\n",
                strerror(rc));
        return rc;
    }

    bool reset_needed = true;
    runner->reset_count   = runner->header->metadata.reset_count;
    runner->period_count  = runner->header->time.period_count;
    runner->last_active_monotonic = get_monotonic_ns();

    while (runner->running && runner->header->metadata.magic_marker) {
        /* Handle reset */
        if (runner->reset_count != runner->header->metadata.reset_count ||
            reset_needed) {
            reset_needed = false;
            runner->reset_count = runner->header->metadata.reset_count;
            while (runner->reset_count & 0x1) {
                dep_audio_timing_wait(&timing);
                runner->reset_count = runner->header->metadata.reset_count;
            }
            dep_audio_memory_barrier_acquire();
            runner->period_count = runner->header->time.period_count;

            if (reset_fn) reset_fn(user_data);
            dep_audio_memory_barrier_acquire();
            continue;
        }

        /* Wait for new period if none available */
        if (runner->period_count == runner->header->time.period_count) {
            dep_audio_timing_wait(&timing);
        }

        uint64_t now = get_monotonic_ns();

        if (runner->period_count < runner->header->time.period_count) {
            uint64_t n = runner->header->time.period_count - runner->period_count;
            dep_audio_memory_barrier_acquire();

            if (transfer_fn)
                transfer_fn((unsigned int)n, user_data);

            runner->period_count += n;
            runner->last_active_monotonic = now;

            if (!runner->is_active) {
                runner->is_active = true;
                if (active_changed_fn) active_changed_fn(true, user_data);
            }
        } else {
            /* Check for inactive timeout */
            uint64_t elapsed = now - runner->last_active_monotonic;
            if (elapsed > 1000000000ULL * RUNNER_INACTIVE_MAX_SECS) {
                if (runner->is_active) {
                    runner->is_active = false;
                    if (active_changed_fn) active_changed_fn(false, user_data);
                }
            }
        }
    }

    dep_audio_timing_close(&timing);
    runner->header = NULL;
    return 0;
}

void dep_audio_runner_stop(dep_audio_runner_t* runner)
{
    if (runner) runner->running = false;
}

bool dep_audio_runner_is_active(const dep_audio_runner_t* runner)
{
    return runner ? runner->is_active : false;
}

/* ====================================================================
 * Priority
 * ==================================================================== */

void dep_audio_set_priority(const char* prog_name)
{
    if (geteuid() == 0) {
        int policy;
        struct sched_param param;
        pthread_getschedparam(pthread_self(), &policy, &param);

        if (policy == SCHED_FIFO || policy == SCHED_RR) {
            if (param.sched_priority < sched_get_priority_max(policy)) {
                param.sched_priority++;
                int ret = pthread_setschedparam(pthread_self(), policy, &param);
                if (ret != 0)
                    fprintf(stderr, "[dep_audio] Increasing Dante thread priority failed: %d\n", ret);
            }
        } else {
            policy = SCHED_FIFO;
            param.sched_priority = sched_get_priority_min(policy);
            int ret = pthread_setschedparam(pthread_self(), policy, &param);
            if (ret != 0)
                fprintf(stderr, "[dep_audio] Setting Dante thread scheduling policy failed: %d\n", ret);
        }
    } else {
        fprintf(stderr, "[dep_audio] WARNING: %s has not been run as root so thread priority will not be changed to realtime.\n",
                prog_name ? prog_name : "The application");
    }
}

void dep_audio_cleanup_priority(void)
{
    /* No-op on POSIX */
}
