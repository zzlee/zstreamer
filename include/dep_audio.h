/**
 * @file dep_audio.h
 * @brief DEP Audio shared memory interface — C port of Audinate dep_audio module.
 *
 * Provides the same buffer_header_t, timing, and runner functionality as the
 * qcap-dev dep_audio C++ library, but as a pure-C API suitable for zstreamer.
 *
 * Original: Copyright © 2020-2023 Audinate Pty Ltd ACN 120 828 006 (Audinate).
 * Ported to C for zstreamer.
 */

#ifndef DEP_AUDIO_H
#define DEP_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Buffer header layout (must match DEP SHM) ─────────────────────── */

#define DEP_AUDIO_ENCODING_PCM32        32
#define DEP_AUDIO_ENCODING_FLOAT32      65
#define DEP_AUDIO_HEADER_MAGIC          0x50525354  /* 'PRST' */
#define DEP_AUDIO_FLAG_SEPARATE_MEMORY  0x1
#define DEP_AUDIO_FLAG_TIMING_SYNC      0x10000

#define DEP_AUDIO_TIMING_OBJECT_NAME_LEN 256

enum {
    DEP_AUDIO_TIMING_NONE        = 0,
    DEP_AUDIO_TIMING_SIGNAL_EVENT = 1
};

/**
 * Overlay structure for the DEP shared memory buffer header block.
 * This exactly mirrors Dante::buffer_header_t.
 */
typedef struct dep_audio_buffer_header {
    struct {
        uint32_t magic_marker;
        uint32_t buffer_length;
        uint32_t metadata_header_length;
        uint32_t flags;
        uint32_t first_tx_channel_offset_bytes;
        uint32_t first_rx_channel_offset_bytes;
        uint32_t timing_object_subheader_offset_bytes;
        uint32_t reset_count;
    } metadata;

    struct {
        uint32_t sample_rate;
        uint32_t encoding;
        uint32_t samples_per_channel;
        uint32_t bytes_per_channel;
        uint32_t num_tx_channels;
        uint32_t num_rx_channels;
        uint32_t pad7;
        uint32_t pad8;
    } audio;

    struct {
        uint32_t epoch_seconds;
        uint32_t epoch_samples;
        uint32_t samples_per_period;
        uint64_t period_count;
        uint32_t clock_drift_ppb;
        uint64_t monotonic;
    } time;
} dep_audio_buffer_header_t;

/**
 * Timing object subheader embedded in the SHM metadata.
 */
typedef struct dep_audio_timing_object {
    uint32_t subheader_length_bytes;
    uint32_t object_type;
    char     object_name[DEP_AUDIO_TIMING_OBJECT_NAME_LEN];
} dep_audio_timing_object_t;

/* ── SharedMemory ──────────────────────────────────────────────────── */

/** Opaque shared memory handle. */
typedef struct dep_audio_shm {
    int    fd;
    size_t size;
    void*  data;
} dep_audio_shm_t;

/**
 * Open and mmap a POSIX shared memory object.
 * @param name  Name (without leading '/') of the shm object.
 * @return 0 on success, errno on failure.
 */
int  dep_audio_shm_open(dep_audio_shm_t* shm, const char* name);

/** Unmap and close the shared memory. */
void dep_audio_shm_close(dep_audio_shm_t* shm);

/* ── Buffers ───────────────────────────────────────────────────────── */

/** Handle for connected DEP SHM buffers. */
typedef struct dep_audio_buffers {
    dep_audio_shm_t  control;          /**< Main SHM region (metadata + timing). */
    dep_audio_shm_t  tx;               /**< Separate TX channel data (if SEPARATE_MEMORY). */
    dep_audio_shm_t  rx;               /**< Separate RX channel data (if SEPARATE_MEMORY). */

    const volatile dep_audio_buffer_header_t*  header;
    const dep_audio_timing_object_t*           timing_obj;

    void** tx_channels;                /**< Array of per-channel write pointers. */
    void** rx_channels;                /**< Array of per-channel read pointers. */
    unsigned int num_tx_channels;
    unsigned int num_rx_channels;
    bool  connected;
    bool  global_namespace;
} dep_audio_buffers_t;

/** Initialise a buffers handle to zero. */
void dep_audio_buffers_init(dep_audio_buffers_t* buf);

/**
 * Connect to DEP shared memory.
 * @param name           SHM name (e.g. "DanteEP").
 * @param global_namespace  True for global name space (Windows; ignored on POSIX).
 * @return 0 on success, negative error code on failure.
 */
int  dep_audio_buffers_connect(dep_audio_buffers_t* buf, const char* name, bool global_namespace);

/** Disconnect and free resources. */
void dep_audio_buffers_disconnect(dep_audio_buffers_t* buf);

/** Get pointer to TX channel buffer (caller writes samples here). */
void* dep_audio_buffers_get_tx_channel(const dep_audio_buffers_t* buf, unsigned int index);

/** Get pointer to RX channel buffer (caller reads samples here). */
void* dep_audio_buffers_get_rx_channel(const dep_audio_buffers_t* buf, unsigned int index);

/* ── Timing ────────────────────────────────────────────────────────── */

/** Opaque timing handle wrapping a POSIX semaphore. */
typedef struct dep_audio_timing {
    sem_t*  semaphore;
    int     type;
} dep_audio_timing_t;

/**
 * Open timing object from the SHM timing subheader.
 * @return 0 on success, errno on failure.
 */
int  dep_audio_timing_open(dep_audio_timing_t* t, const dep_audio_timing_object_t* subheader, bool global_namespace);

/** Close the timing object. */
void dep_audio_timing_close(dep_audio_timing_t* t);

/**
 * Wait for the next DEP period signal (sem_timedwait with 1s timeout).
 * @return 0 on success, ETIMEDOUT on timeout (not fatal), other errno on error.
 */
int  dep_audio_timing_wait(dep_audio_timing_t* t);

/* ── Runner ────────────────────────────────────────────────────────── */

/** Callback invoked on each period: n = number of new periods available. */
typedef void (*dep_audio_transfer_fn)(unsigned int n, void* user_data);

/** Callback invoked when DEP is reset (magic cleared / reset_count changed). */
typedef void (*dep_audio_reset_fn)(void* user_data);

/** Callback invoked when active state changes. */
typedef void (*dep_audio_active_changed_fn)(bool active, void* user_data);

/** Handle for the runner processing loop. */
typedef struct dep_audio_runner {
    dep_audio_buffers_t* buffers;
    const volatile dep_audio_buffer_header_t* header;
    const dep_audio_timing_object_t*          timing_obj;

    dep_audio_transfer_fn       transfer_fn;
    dep_audio_reset_fn          reset_fn;
    dep_audio_active_changed_fn active_changed_fn;
    void*                       user_data;

    uint16_t reset_count;
    uint64_t period_count;
    uint64_t last_active_monotonic;
    bool     is_active;
    bool     running;
} dep_audio_runner_t;

/**
 * Run the DEP processing loop (blocking).
 * @param runner     Initialised runner handle.
 * @param transfer_fn Called when new audio data is available.
 * @param reset_fn    Called on DEP reset (may be NULL).
 * @param active_changed_fn Called on active state change (may be NULL).
 * @param user_data   Opaque pointer forwarded to callbacks.
 * @return 0 on clean exit, negative on error.
 */
int dep_audio_runner_run(dep_audio_runner_t* runner,
                         dep_audio_transfer_fn transfer_fn,
                         dep_audio_reset_fn reset_fn,
                         dep_audio_active_changed_fn active_changed_fn,
                         void* user_data);

/** Signal the runner to stop. */
void dep_audio_runner_stop(dep_audio_runner_t* runner);

/** Check if runner is currently active (receiving periods). */
bool dep_audio_runner_is_active(const dep_audio_runner_t* runner);

/* ── Priority ──────────────────────────────────────────────────────── */

/**
 * Set real-time thread priority (SCHED_FIFO) for the calling thread.
 * Requires root (CAP_SYS_NICE). Prints warning if not root.
 */
void dep_audio_set_priority(const char* prog_name);

/** Clean up priority resources (no-op on POSIX). */
void dep_audio_cleanup_priority(void);

/* ── Memory barrier ────────────────────────────────────────────────── */

/**
 * Acquire memory barrier — ensures subsequent reads see data written
 * before the DEP period_count was updated.
 */
static inline void dep_audio_memory_barrier_acquire(void)
{
    volatile int dummy = 1;
    (void)dummy;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif

#endif /* DEP_AUDIO_H */
