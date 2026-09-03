# Advanced Features — Phase 8

## 8a — Allocator API  (from `wiki/future.md`)  (✅ done)
- [x] `zst_allocator_t` interface: `alloc`, `free`, ref-counting
- [x] Default CPU allocator (malloc/free)
- [x] DMABUF allocator (Linux dma-buf)
  
  **Linux dma-buf Sharing and Mapping:**
  - [x] Context struct: `zst_dmabuf_mem_t` wrapping `fd`, `size`, and mapped CPU address pointer (`mmap_ptr`).
  - [x] Implement `zst_allocator_dmabuf_create(void)` backing allocator via Linux memory files/devices (e.g., `/dev/udmabuf`, `dma-buf` heaps, or fallback `memfd_create` with file-seals for compatibility testing).
  - [x] Implement `zst_allocator_dmabuf_import(zst_allocator_t* allocator, int fd, size_t size)` to wrap an existing DMABUF file descriptor.
  - [x] Implement `zst_allocator_dmabuf_get_fd(zst_allocator_t* allocator, void* ptr)` to retrieve the file descriptor from an allocated block.
  - [x] Implement custom `alloc` (which configures the memory struct and returns mapped user-space address) and `free` (which performs `munmap` and closes the fd).

- [x] CUDA / Vulkan device memory allocators
  
  **CUDA Device Memory Allocator:**
  - [x] Context struct: Not required since CUDA device memory pointers can be directly passed to `cudaFree()`. Private pointer `alloc->priv` is set to `NULL`.
  - [x] Implement `zst_allocator_cuda_create(void)` returning a CUDA-specific `zst_allocator_t`. Note: Constructor has been simplified to take no arguments, automatically targeting the default device.
  - [x] Implement `alloc` callback calling `cudaMalloc` (device allocation).
  - [x] Implement `free` callback calling `cudaFree`.
  - [x] Safely return `NULL` if CUDA runtime or GPU hardware is not available (such as in headless or CPU-only Docker environments), which is handled gracefully by tests.

  **Vulkan Device Memory Allocator:**
  - [x] Context struct: `zst_vulkan_allocator_t` tracking the list of active memory allocations via a dynamic array of `zst_vulkan_mem_t` structs (wrapping `VkDeviceMemory`, `VkBuffer`, size, and the mapped host pointer).
  - [x] Implement `zst_allocator_vulkan_create(void)` returning a Vulkan-specific `zst_allocator_t`. Note: Constructor has been simplified to automatically instantiate Vulkan, select the physical device, create the logical device, and locate a suitable host-visible/coherent memory type.
  - [x] Implement `alloc` callback creating a `VkBuffer`, allocating host-visible coherent `VkDeviceMemory` via `vkAllocateMemory`, binding, mapping with `vkMapMemory`, and registering mapping metadata.
  - [x] Implement `free` callback looking up allocation metadata, unmapping, freeing `VkDeviceMemory`, and destroying the `VkBuffer`.
  - [x] Safely return `NULL` if Vulkan loader, physical device, or host-visible/coherent memory is not available, which is handled gracefully by tests.

  **Intel oneAPI (SYCL) Device Memory Allocator:**
  - [x] Context struct: `zst_oneapi_allocator_t` tracking the SYCL queue (`sycl::queue`), context (`sycl::context`), and selected device metadata.
  - [x] Implement `zst_allocator_oneapi_create(void)` returning a oneAPI SYCL-specific `zst_allocator_t` automatically targeting the default available Intel GPU device (or CPU fallback).
  - [x] Implement `alloc` callback utilizing Unified Shared Memory (USM) device allocations via `sycl::malloc_device`.
  - [x] Implement `free` callback freeing the USM memory via `sycl::free`.
  - [x] Safely return `NULL` if oneAPI SYCL runtime environment, queue, or compatible hardware/driver is unavailable.

  **Jetson NvBuffer Allocator:**
  - [x] Context struct: `zst_nvbuffer_allocator_t` tracking the NvBuffer state.
  - [x] Implement `zst_allocator_nvbuffer_create(void)` returning an NvBuffer-specific `zst_allocator_t`.
  - [x] Implement `alloc` callback that creates an `NvBuffer`, allocates its memory, and maps it for CPU access if necessary.
  - [x] Implement `free` callback that unmaps and destroys the `NvBuffer`.
  - [x] Safely return `NULL` if the Jetson Multimedia API (NvBuffer) environment is not available.

- [x] Buffer pools to eliminate per-frame allocation ✅

  **`zst_buffer_pool_t` — a recyclable pool of pre-allocated buffers**

  Every source element currently calls `zst_buffer_create()` per frame (see
  `v4l2_source.c`, `alsa_source.c`, `video_scaler.c`, etc.). A buffer pool
  pre-allocates a set of buffers upfront and recycles them, eliminating
  `malloc`/`free` overhead on every frame.

  **Data structure and lifecycle:**
  - [x] `zst_buffer_pool_t` struct with a LIFO/freelist of buffers
  - [x] Backed by a `zst_allocator_t` — pool allocates new buffers via allocator
  - [x] Config: `min_buffers`, `max_buffers`, `buffer_size`, `buffer_type`
  - [x] Thread-safe acquire/release via `pthread_mutex` + `pthread_condvar`
  - [x] Watermark callbacks: low-watermark triggers pre-fill, high-watermark triggers drain
  - [x] `zst_buffer_pool_create(allocator, config)` / `_destroy()` / `_flush()`

  **Acquire / release API:**
  - [x] `zst_buffer_pool_acquire(pool, timeout_ms)` — returns a buffer from the pool;
        blocks if empty until a buffer is returned or timeout expires
  - [x] `zst_buffer_pool_release(pool, buf)` — returns the buffer to the pool;
        resets refcount to 1, clears flags/metadata (but keeps underlying memory for reuse)
  - [x] Optional non-blocking acquire with `ZST_POOL_ACQUIRE_NONBLOCK` flag
  - [x] On release: if pool is at capacity, actually free the buffer instead of recycling

  **Integration with zst_buffer:**
  - [x] `zst_buffer_t` gets an optional `pool` back-pointer (or reuse `memory.priv`)
  - [x] `zst_buffer_create_with_pool(pool)` — acquire from pool instead of malloc
  - [x] `zst_buffer_unref()` checks for pool back-pointer: if pool is set, call
        `pool->release(buf)` instead of `free`; otherwise normal free path
  - [x] Pool buffers skip the `destroy` callback on recycle (only called on final unref when
        pool itself is destroyed)

  **Usage in elements (migration):**
  - [x] `v4l2_source`: allocate pool during `open()`, acquire per-frame in process()
        instead of `zst_buffer_create()`; release happens automatically on `unref`
  - [x] `alsa_source`: same pattern for audio frames
  - [x] `video_scaler`: pool for output buffers
  - [x] `audio_resampler`: pool for output buffers
  - [x] `x264enc` / `aacenc`: packet pool for encoded output
  - [x] `queue_element`: optionally attach pool to queue — return consumed buffers
        to the upstream pool automatically

  **Auto-configuration from caps:**
  - [x] `zst_buffer_pool_config_from_caps(caps)` — derive `buffer_size` from
        resolution × pixel format (video) or sample_rate × channels × format (audio)
  - [x] **Default pool sizing** — topology-aware `min_buffers` adjustment

    **Problem:** `zst_buffer_pool_config_from_caps()` only receives caps and has no
    access to the pipeline topology, and many pools are created during `open()` or
    lazily during first processing rather than when the graph is assembled.

    **Design decision — two-step configuration:**

    1. **Format sizing** (existing, unchanged): `config_from_caps(caps)` sets
       `buffer_size` from resolution/sample rate, with `min_buffers=2, max_buffers=8`.

    2. **Topology sizing** (new, at pipeline-build/run time): pipeline helpers
       query the linked downstream graph and adjust exposed pool configs before
       `start()`. A lightweight element back-reference also lets pad/scheduler
       paths re-apply sizing after lazily-created pools appear during processing.

       Effective rule: for each element-owned output pool, count reachable
       downstream queue elements and ensure `min_buffers >= downstream_queues + 2`;
       raise `max_buffers` as needed.

    - [x] Implement `zst_pipeline_count_elements_of_type(pipeline, type_name)`
    - [x] Implement `zst_pool_config_default_size()` helper
    - [x] Wire into pipeline start sequence and lazy-pool update paths
    - [x] Safely resize pool storage when `max_buffers` changes

  **Test deliverables:**
  - [x] Unit test: acquire/recycle loop (N buffers, M cycles, no net allocation)
  - [x] Unit test: acquire blocks when pool exhausted, unblocks on release
  - [x] Unit test: acquire with timeout returns NULL on expiry
  - [x] Unit test: pool-backed buffer unref returns buffer to pool
  - [x] Unit test: pool flush frees all cached buffers
  - [x] Unit test: topology-aware pool sizing with downstream queues
  - [x] Integration test: `videotestsrc → queue → filesink` with pool, verify zero
        calls to `malloc` after warm-up phase
    - [x] Unit test: basic allocator create/alloc/free/destroy
    - [x] Unit test: nonblock acquire with `ZST_POOL_ACQUIRE_NONBLOCK` flag

## 8b — Clock  (from `wiki/future.md`)  (✅ done)
- [x] `zst_clock_t` interface: `get_time`, `wait`
- [x] System clock wrapping `CLOCK_MONOTONIC`
- [x] Pipeline-level master clock selection
- [x] Clock slaving for A/V sync — see [`wiki/clock-slaving.md`](clock-slaving.md)
  for detailed design and task breakdown
- [x] Jitter Measurement:
  - [x] PLL Sync Jitter: Track moving average of phase error in slave clock
  - [x] Media Transit Jitter: Track RFC 3550 standard packet transit variance on pads
  - [x] Metrics API: Implement functions to query tracking values dynamically

## 8c — Other Advanced Features

### Element Bin (composite sub-pipeline)

A container element that groups multiple elements into a single logical element with a clean external interface — analogous to GStreamer's `bin`. Enables reusable pipeline components, complex element composition, and hierarchical pipeline structures.

- [x] `zst_bin_t` as a `zst_element` subclass — state machine delegates to children
- [x] Ghost pads: `zst_ghost_pad_t` proxies an internal element's pad to the bin's external interface
- [x] Child management: `zst_bin_add()` / `zst_bin_remove()` with automatic state synchronisation
- [x] State propagation: `NULL→READY→PAUSED→PLAYING` cascade to all children
- [x] Error aggregation: child errors bubble up through the bin's bus
- [x] EOS passthrough: bin converges EOS from all sink-pad branches before signalling src
- [x] Use case: package `videotestsrc → queue → x264enc` as a reusable "capture" bin
- [x] Use case: create custom muxer bins with internal format conversion
- [x] Use case: isolate a sub-pipeline for separate threading / scheduling

### Pad Blocking / Probes (buffer interception)

Intercept data flowing through a pad without modifying the element's logic. Analogous to GStreamer's pad probes — enables frame-by-frame inspection, dynamic filtering, and pipeline debugging without element modification.

- [x] `zst_pad_add_probe(pad, callback, user_data)` — attach a probe callback to a pad
- [x] Probe types: `PRE_BUFFER` (before element process), `POST_BUFFER` (after process), `PRE_EVENT`, `POST_EVENT`
- [x] Return values: `PROBE_OK` (passthrough), `PROBE_DROP` (discard buffer), `PROBE_BLOCK` (pause data flow)
- [x] Pad blocking: `zst_pad_block(pad)` — block data flow at a pad, resume with `zst_pad_unblock()`
- [x] Block callback: fire on first blocked buffer, return `PROBE_OK` to unblock or `PROBE_REBLOCK` to keep blocking
- [x] Use case: frame-by-frame stepping through a pipeline (debugger pattern)
- [x] Use case: dynamic buffer dropping for bandwidth / QoS management
- [x] Use case: tap into pipeline data for parallel analysis (e.g. recording + preview)
- [x] Use case: insert custom processing at any pad boundary without writing an element

### Segment Seeking (timestamp-based clipping)

Enable playback of a specific time range within a stream — clip in, clip out, seeking, and looping. Unlike frame-accurate VCR-style seeking, this focuses on segment-based clipping for live recording and on-demand playback.

- [x] `zst_segment_t` data structure: `start`, `stop`, `rate`, `base`, `position`
- [x] Segment event: `ZST_EVENT_SEGMENT` propagated downstream from source elements
- [x] Source element seeking: `zst_element_seek(element, rate, segment)` → element jumps to new position
- [x] Sink element clipping: apply `start`/`stop` segment bounds — discard buffers outside the window
- [x] `SEEK` support for format-specific sources where available (filesrc maps segment range to byte offset/length)
- [x] Use case: clip a recording to a specific time range (start=30.0, stop=120.0)
- [x] Use case: loop playback of a segment for stress testing
- [x] Use case: seek to a specific position in a recorded file source
- [x] Use case: pause/resume from last position (stop position as resumption point)

## 8d — Element Public API and Plugin-First Feature Exposure

Expose the features of all official, dynamic, and future elements through a stable C API. The existing `dlopen` plugin/factory system should be the primary public interface; per-element C headers should be optional convenience wrappers, not the only supported way to use elements.

### Design Principles

- [x] Treat `zst_element_factory_make()` as the primary user-facing element creation API
- [x] Use one registry path for built-in official elements and dynamically loaded plugins
- [x] Make elements self-describing: factory name, category, description, properties, pads, caps, and creation function
- [x] Keep element private structs private; users interact through `zst_element_t`, pads, caps, properties, and introspection
- [x] Preserve the current string property API for compatibility while adding typed helpers
- [x] Provide optional official convenience headers for first-party elements only

### Plugin / Element Metadata

Extend the plugin ABI in a backward-compatible way using `abi_version` and `struct_size`. Keep the current `create_element(const char* name)` path during transition so existing plugins continue to load.

- [x] Add `zst_property_type_t` for `STRING`, `INT`, `UINT`, `DOUBLE`, `BOOL`, and `ENUM`
- [x] Add property flags: `READABLE`, `WRITABLE`, `RUNTIME`
- [x] Add `zst_property_spec_t` with name, type, flags, default value, and description
- [x] Add `zst_pad_template_t` with pad name, direction, and caps string
- [x] Add `zst_element_desc_t` describing each element exported by a plugin
- [x] Extend `zst_plugin_desc_t` to expose an array of `zst_element_desc_t`
- [x] Support plugins that expose multiple element factories from one `.so`

Candidate public structures:

```c
typedef enum {
    ZST_PROPERTY_STRING,
    ZST_PROPERTY_INT,
    ZST_PROPERTY_UINT,
    ZST_PROPERTY_DOUBLE,
    ZST_PROPERTY_BOOL,
    ZST_PROPERTY_ENUM
} zst_property_type_t;

typedef enum {
    ZST_PROPERTY_READABLE = 1u << 0,
    ZST_PROPERTY_WRITABLE = 1u << 1,
    ZST_PROPERTY_RUNTIME  = 1u << 2
} zst_property_flags_t;

typedef struct {
    const char* name;
    zst_property_type_t type;
    uint32_t flags;
    const char* default_value;
    const char* description;
} zst_property_spec_t;

typedef struct {
    const char* name;
    zst_pad_direction_t direction;
    const char* caps;
} zst_pad_template_t;

typedef struct {
    const char* name;
    const char* long_name;
    const char* category;
    const char* description;
    const char* author;

    const zst_property_spec_t* properties;
    uint32_t nb_properties;

    const zst_pad_template_t* pads;
    uint32_t nb_pads;

    zst_element_t* (*create)(void);
} zst_element_desc_t;
```

### Factory Introspection APIs

Applications, CLIs, UIs, and tests should be able to discover available elements at runtime, including third-party plugin elements unknown at compile time.

- [x] `zst_element_factory_list()` — list all registered element descriptors
- [x] `zst_element_factory_get_desc(name)` — get metadata for one factory name
- [x] Introspection should work for both built-in and plugin-backed elements
- [x] Return property and pad metadata without requiring element instantiation where possible

Candidate API:

```c
uint32_t zst_element_factory_list(
    const zst_element_desc_t*** elements_out);

const zst_element_desc_t* zst_element_factory_get_desc(
    const char* name);
```

### Typed Property Helpers

Keep the current string API:

```c
zst_element_set_property(el, "chunk-size", "4096");
```

Add typed wrappers for safer user code:

- [x] `zst_element_set_property_string()` / `zst_element_get_property_string()`
- [x] `zst_element_set_property_int()` / `zst_element_get_property_int()`
- [x] `zst_element_set_property_uint()` / `zst_element_get_property_uint()`
- [x] `zst_element_set_property_double()` / `zst_element_get_property_double()`
- [x] `zst_element_set_property_bool()` / `zst_element_get_property_bool()`
- [x] Validate typed helper calls against descriptor metadata when available

Candidate API:

```c
zst_result_t zst_element_set_property_string(zst_element_t* el, const char* name, const char* value);
zst_result_t zst_element_set_property_int(zst_element_t* el, const char* name, int64_t value);
zst_result_t zst_element_set_property_uint(zst_element_t* el, const char* name, uint64_t value);
zst_result_t zst_element_set_property_double(zst_element_t* el, const char* name, double value);
zst_result_t zst_element_set_property_bool(zst_element_t* el, const char* name, bool value);

zst_result_t zst_element_get_property_string(zst_element_t* el, const char* name, char* value_out, size_t max_len);
zst_result_t zst_element_get_property_int(zst_element_t* el, const char* name, int64_t* value_out);
zst_result_t zst_element_get_property_uint(zst_element_t* el, const char* name, uint64_t* value_out);
zst_result_t zst_element_get_property_double(zst_element_t* el, const char* name, double* value_out);
zst_result_t zst_element_get_property_bool(zst_element_t* el, const char* name, bool* value_out);
```

### Unified Built-In and Dynamic Registration

The current dynamic plugin flow should remain valid:

```c
zst_plugin_registry_init();
zst_plugin_registry_scan(path);
zst_plugin_registry_scan_env();
zst_element_factory_make("filesrc");
```

Add built-in registration so official elements and dynamic plugins are available through the same factory/introspection API.

- [x] Add `zst_register_builtin_elements()` or equivalent initialization hook
- [x] Register built-in official elements into the same factory registry as dynamic plugins
- [x] Ensure `zst_element_factory_make()` does not care whether an element is built-in or plugin-backed
- [x] Install plugin `.so` files to a stable plugin directory and continue supporting `ZSTREAMER_PLUGIN_PATH`

Candidate usage:

```c
zst_plugin_registry_init();
zst_register_builtin_elements();
zst_plugin_registry_scan_env();

zst_element_t* src = zst_element_factory_make("filesrc");
zst_element_t* enc = zst_element_factory_make("x264enc");
zst_element_t* sink = zst_element_factory_make("filesink");
```

### Metadata for Official Elements

Add `zst_element_desc_t` metadata for every official element:

- [x] `filesrc`, `filesink`, `fakesink`
- [x] `v4l2source`, `alsasource`
- [x] `x264enc`, `h264dec`
- [x] `h265encoder`, `h265decoder`
- [x] `aacencoder`, `aacdecoder`
- [x] `mp4muxer`
- [x] `videoscaler`, `audioresampler`
- [x] `videotestsrc`, `audiotestsrc`
- [x] `textoverlay`, `textsource`, `srtparser`
- [x] `netsrc`, `netsink`
- [x] `rtspsource`, `rtspsink`, `rtspserver`
- [x] future RTMP source/sink and other elements

Each descriptor should document:

- [x] factory name
- [x] long name, category, and description
- [x] source and sink pad templates
- [x] supported/static caps where known
- [x] readable/writable properties and defaults
- [x] read-only statistics where applicable

### Optional Official Convenience Headers

Install optional first-party headers under a stable namespace, for example:

```text
include/zstreamer/elements/zst_file_source.h
include/zstreamer/elements/zst_file_sink.h
include/zstreamer/elements/zst_fake_sink.h
include/zstreamer/elements/zst_x264_encoder.h
...
```

These headers may expose constructor convenience functions, property name macros, and optional config structs with `struct_size` for ABI extension.

- [x] Add convenience headers for official elements where useful
- [x] Add property name macros to avoid string literals in user code
- [x] Use `struct_size` in config structs for forward-compatible extension
- [x] Implement wrappers on top of the same element implementations; do not bypass the generic factory/property model

Example:

```c
#define ZST_FILE_SOURCE_PROP_PATH       "path"
#define ZST_FILE_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_FILE_SOURCE_PROP_LOOP       "loop"

zst_element_t* zst_file_source_create(const char* path);
```

### Library and Installation Layout

Recommended installable artifacts:

```text
libzstreamer.so              core framework and registry
libzstreamer-elements.so     official element implementations, if not linked into core
lib/zstreamer/plugins/*.so   dynamic plugins
include/zstreamer/...        public headers
```

- [x] Decide whether official elements live in `libzstreamer`, `libzstreamer-elements`, plugins, or a supported combination
- [x] Install public headers and optional convenience headers
- [x] Install official plugin `.so` files to a stable plugin directory
- [x] Add CMake/pkg-config metadata so users can link core and official elements cleanly

### Test Deliverables

- [x] Existing plugin ABI loads during transition
- [x] Registry lists built-in elements
- [x] Registry lists dynamically loaded plugin elements
- [x] Descriptors for official elements contain expected properties and pads (covered for `filesrc`, `filesink`, `fakesink`)
- [x] `zst_element_factory_make()` creates the same element whether backed by built-in registration or plugin registration
- [x] Typed property helpers set/get values correctly
- [x] Typed helper validation rejects wrong property types where metadata is available
- [x] Public convenience headers compile and link from an external-style test target
- [x] Third-party test plugin descriptors are discoverable

### User-Facing Examples

Generic plugin-friendly API:

```c
zst_plugin_registry_init();
zst_register_builtin_elements();
zst_plugin_registry_scan_env();

zst_element_t* src = zst_element_factory_make("filesrc");
zst_element_set_property_string(src, "path", "input.h264");
zst_element_set_property_uint(src, "chunk-size", 4096);

zst_element_t* sink = zst_element_factory_make("filesink");
zst_element_set_property_string(sink, "path", "output.h264");
```

Optional official convenience API:

```c
#include "zstreamer/elements/zst_file_source.h"
#include "zstreamer/elements/zst_file_sink.h"

zst_element_t* src = zst_file_source_create("input.h264");
zst_element_t* sink = zst_file_sink_create("output.h264");
```

---

## ASRC Drift Compensation in audioresampler  (✅ Implemented)

ASRC (Asynchronous Sample Rate Conversion) compensates for sample-clock drift
between the upstream source and the nominal sampling rate.  This is essential
when combining audio from independent clocks (e.g., two USB microphones, or a
live mic and a file playback).

### Design Decision: ASRC lives in audioresampler, not audiomixer

ASRC is implemented as an optional mode of the standalone `audioresampler`
element rather than being baked into `audiomixer`.  Rationale:

- **Separation of concerns**: resampling is a DSP transformation; mixing is a
  summation.  Combining them violates single responsibility.
- **Reusability**: ASRC drift compensation is useful anywhere, not just before a
  mixer (e.g., a resampler feeding a file sink).
- **Pipeline composability**: users explicitly see and control where conversion
  happens.  The audiomixer receives already-aligned buffers.
- **Existing infrastructure**: `audioresampler` already uses `libswresample`;
  the ASRC feature builds on the same backend.

### Properties

| Property | Type | Default | Description |
|---|---|---|---|
| `asrc-mode` | String | `"none"` | `"none"` = fixed-ratio SRC; `"pts"` = PTS-based drift compensation |
| `max-drift-ppm` | Double | `1000` | Maximum expected drift in parts per million (0.1%) |
| `drift-check-interval` | Int | `4` | Check drift every N processed buffers |
| `total-input-samples` | Int (R/O) | `0` | Cumulative input samples processed |
| `total-output-samples` | Int (R/O) | `0` | Cumulative output samples produced |
| `drift-adjust-count` | Int (R/O) | `0` | Number of drift adjustments performed |
| `rate-numer` | Int | `0` | Explicit target rate numerator (0 = use `sample-rate`). Overrides when set with `rate-denom`. E.g. `4800001` gives ~48000.01 Hz output when combined with `rate-denom=100` |
| `rate-denom` | Int | `0` | Explicit target rate denominator. E.g. `100` with `rate-numer=4800001` yields target rate = 4800001/100 = 48000.01 Hz |
| `block-samples` | Int | `0` | Chunk output buffers into fixed-size sample blocks (e.g. 512). Useful for downstream hardware ring buffers (e.g. Dante DEP) to avoid burst overruns. 0 = output entire resampled buffer as one chunk. |

For real-world hardware integration and debugging (e.g., Dante DEP ring buffer pacing, underflow elimination, and PTS synchronization), see [Dante Audio Clock Drift & Buffer Underflow Debug](dante-audio-clock-drift-debug.md).

### Algorithm (`pts` mode)

1. **Drift detection** — For each buffer, compare the PTS delta (`pts − last_pts`)
   against the expected delta at the nominal input rate.  The difference (in
   input-sample units) is the instantaneous drift.

   ```c
   expected_samples = delta_pts × rate_in / 1e9
   drift_input = nb_samples − expected_samples
   drift_output = drift_input × rate_out / rate_in
   ```

2. **Accumulation** — Drift is accumulated in the output-sample domain.

3. **Sanity limiting** — Drift per buffer is capped at `±2 × max-drift-ppm`
   to reject PTS discontinuities (seeks, streams).

4. **Compensation** — When accumulated drift ≥ 1 output sample, call
   `swr_set_compensation(ctx, −drift, next_out_samples)` to smoothly correct
   the resampling ratio over the next output block.

### Pipeline example

```
# ASRC-enabled resampler: handles drift from a live source at nominal 44.1kHz
# to a mixer that expects 48kHz

alasrc device=hw:0 !
  audioresampler sample-rate=48000 asrc-mode=pts !
  audiomixer ! filesink

alasrc device=hw:1 !
  audioresampler sample-rate=48000 asrc-mode=pts !
  audiomixer
```

### Implementation checklist

- [x] `asrc-mode` property: `"none"` (fixed SRC) / `"pts"` (drift compensation)
- [x] `max-drift-ppm` property with default 1000 ppm (0.1%)
- [x] `drift-check-interval` property (default: 4 buffers)
- [x] Read-only stats: `total-input-samples`, `total-output-samples`, `drift-adjust-count`
- [x] PTS-based drift detection with PTS-discontinuity rejection
- [x] `swr_set_compensation()`-based ratio adjustment
- [x] Compensation clamping (max 25% of block) to prevent audible glitches
- [x] ASRC state reset on SwrContext reconfiguration (rate/format change)
- [x] Property specs registered in `zst_builtins.c`
- [x] `zst_audio_resampler_create_with_config()` convenience constructor
- [x] Existing tests unchanged (100% pass)
- [x] Passthrough bypass: ASRC mode and rate override now force swr engagement even when nominal integer rates match (critical for PTS drift detection with equal rates)
- [x] `rate-numer`/`rate-denom` properties for explicit fractional target rates

### Fractional Rate Override (`rate-numer` / `rate-denom`)

The `audioresampler` supports explicit fractional target rates via `rate-numer` and
`rate-denom` properties. This allows conversions where the exact rate cannot be
expressed as an integer (e.g., 48000 → 48000.01 Hz).

**How it works:**

1. The target rate is computed as `target = rate-numer / rate-denom` (e.g.,
   `4800001 / 100 = 48000.01` Hz).
2. The nearest integer rate is used for the underlying `SwrContext` (e.g.,
   `48001`). If this equals the input rate, it's nudged by ±1 to force swr
   to create a resampling filter (otherwise swr uses a memcpy path that
   ignores `swr_set_compensation()`).
3. `swr_set_compensation()` is called before every `swr_convert()` to
   fine-tune the ratio from the integer rate to the exact fractional target.
   Compensation is re-applied each call so it never expires.
4. The output buffer size (`av_rescale_rnd`) uses the true target ratio for
   correct allocation, independent of the swr-configured integer rate.

**Limitations:**

- The output frame's `sample_rate` field reports the nearest integer rate,
  not the exact fractional target. The tiny metadata error (~0.02% for
  48000.01 vs 48001) is absorbed by PTS-based timing downstream.
- For extremely small differences (e.g., 48000 → 48000.01, ratio ≈ 1.0000002),
  the compensation accumulates very slowly (~1 extra sample per 4688 buffers
  at 1024-sample frames, i.e., ~100 seconds). Prefer ASRC mode
  (`asrc-mode=pts`) for drift-based scenarios where the clock difference
  naturally manifests in PTS timestamps.
- Setting `sample-rate` clears the rate override (`rate-numer` = `rate-denom` = 0).

**Combining with ASRC:**

`rate-numer`/`rate-denom` can be combined with `asrc-mode=pts`. The rational
override sets the base conversion ratio, and ASRC further fine-tunes it based
on PTS drift detection. This is useful when you know the approximate ratio
but the clocks also drift independently.
