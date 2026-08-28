#pragma once

#include <stddef.h>
#include <stdint.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "Dante DEP audio is supported only on x86_64 and AArch64 natural-alignment ABIs"
#endif

#define DEP_HEADER_MAGIC          UINT32_C(0x50525354)
#define DEP_LAYOUT_SEPARATE       UINT32_C(0x00000001)
#define DEP_TIMING_WRITE_ACTIVE   UINT32_C(0x00010000)
#define DEP_ENCODING_PCM32        UINT32_C(32)
#define DEP_TIMING_SIGNAL_EVENT   UINT32_C(1)
#define DEP_TIMING_NAME_CAPACITY  256u

typedef struct {
    uint32_t magic;
    uint32_t object_bytes;
    uint32_t metadata_bytes;
    uint32_t flags;
    uint32_t tx_offset;
    uint32_t rx_offset;
    uint32_t timing_offset;
    uint32_t reset_serial;

    uint32_t sample_rate;
    uint32_t encoding;
    uint32_t samples_per_channel;
    uint32_t bytes_per_channel;
    uint32_t tx_channels;
    uint32_t rx_channels;
    uint32_t audio_reserved[2];

    uint32_t epoch_seconds;
    uint32_t epoch_samples;
    uint32_t samples_per_period;
    uint32_t period_alignment;
    uint64_t period_count;
    uint32_t drift_ppb;
    uint32_t monotonic_alignment;
    uint64_t monotonic_ns;
} dep_shared_header_t;

typedef struct {
    uint32_t descriptor_bytes;
    uint32_t kind;
    char name[DEP_TIMING_NAME_CAPACITY];
} dep_timing_descriptor_t;

_Static_assert(sizeof(void*) == 8, "Dante DEP audio requires a 64-bit ABI");
_Static_assert(sizeof(dep_shared_header_t) == 104, "unexpected DEP header layout");
_Static_assert(offsetof(dep_shared_header_t, magic) == 0, "unexpected DEP metadata offset");
_Static_assert(offsetof(dep_shared_header_t, sample_rate) == 32, "unexpected DEP audio offset");
_Static_assert(offsetof(dep_shared_header_t, epoch_seconds) == 64, "unexpected DEP time offset");
_Static_assert(offsetof(dep_shared_header_t, samples_per_period) == 72, "unexpected DEP period size offset");
_Static_assert(offsetof(dep_shared_header_t, period_count) == 80, "unexpected DEP period counter offset");
_Static_assert(offsetof(dep_shared_header_t, drift_ppb) == 88, "unexpected DEP drift offset");
_Static_assert(offsetof(dep_shared_header_t, monotonic_ns) == 96, "unexpected DEP monotonic offset");
_Static_assert(sizeof(dep_timing_descriptor_t) == 264, "unexpected DEP timing descriptor layout");
_Static_assert(offsetof(dep_timing_descriptor_t, name) == 8, "unexpected DEP timing name offset");
