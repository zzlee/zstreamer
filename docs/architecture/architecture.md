@page architecture_architecture Architecture

# Architecture

`zstreamer` is a **GStreamer-inspired** multimedia pipeline framework written in C11.
It decomposes media processing into a directed graph of **elements** connected via **pads**, with data flowing as **buffers** through **queues** driven by a **scheduler**.

## Core Concepts

### Buffer (zst_buffer)
The fundamental data carrier — a reference-counted blob with:

| Field      | Purpose                                 |
|------------|-----------------------------------------|
| `type`     | Video/audio frame, packet, or user      |
| `refcount` | Atomic ref-counting for zero-copy sharing |
| `pts/dts`  | Presentation / decode timestamps        |
| `duration` | Duration of the data                    |
| `memory`   | Typed memory descriptor (CPU, DMABUF, CUDA, Vulkan, oneAPI) |
| `payload`  | Opaque typed payload (video/audio frame structs) |
| `destroy`  | Optional custom destructor              |

**Lifecycle:**
```
zst_buffer_create() → refcount = 1
zst_buffer_ref()    → refcount++
zst_buffer_unref()  → refcount--; free when 0
```

### Memory Descriptor (zst_memory_t)
The raw allocation container wrapped inside a zst_buffer. It separates buffer metadata (timestamps, refcounts, types) from the concrete memory backend implementation, enabling a unified interface for zero-copy operations across CPU and accelerators.

| Field | Type | Purpose |
|---|---|---|
| `type` | `zst_memory_type_t` | Identifies the physical memory domain (CPU host memory, DMABUF, CUDA device pointer, Vulkan buffer memory, or Intel oneAPI Unified Shared Memory). |
| `data` | `void*` | Pointer to the mapped buffer. For CPU/DMABUF/Vulkan, this is a host-visible virtual address. For CUDA/oneAPI, this is a GPU device address. |
| `size` | `size_t` | Capacity of the allocation in bytes. |
| `priv` | `void*` | Opaque context pointer used by the allocator to manage resource release (e.g. holds file descriptor for DMABUF, allocation metadata, or custom wrapper structures). |
| `release` | `void (*)(void*)` | Callback invoked when the buffer is destroyed to free the backing memory using the allocator that created it. |

#### Backend Integration Models:
1. **CPU Host Memory (`ZST_MEMORY_CPU`)**:
   - `data` is mapped directly to standard `malloc`'d RAM.
   - `release` is a simple wrapper around `free()`.
2. **DMA-BUF (`ZST_MEMORY_DMABUF`)**:
   - Used for zero-copy file sharing between hardware modules (e.g. V4L2 and GPU/CPU).
   - `priv` stores the file descriptor (`fd`) dynamically duplicated during import.
   - `data` holds the `mmap`'d user-space virtual address, and `release` performs `munmap` and closes the `fd`.
3. **Vulkan Device Memory (`ZST_MEMORY_VULKAN`)**:
   - Backed by Vulkan logical device memory and buffers.
   - `priv` points to `VkDeviceMemory` and `VkBuffer` metadata.
   - `data` holds the mapped host-visible memory pointer.
4. **CUDA Device Memory (`ZST_MEMORY_CUDA`)**:
   - GPU-only memory allocated via `cudaMalloc`.
   - `data` holds the device address (not directly dereferenceable on CPU).
   - `release` triggers `cudaFree()`.
5. **Intel oneAPI SYCL Memory (`ZST_MEMORY_ONEAPI`)**:
   - Unified Shared Memory (USM) device-only allocation.
   - `data` holds the device-only USM pointer allocated via `sycl::malloc_device`.
   - `release` triggers `sycl::free()`.

### Pad (zst_pad)
Connection point on an element. Two directions:

- **SRC pad** — emits data (source / output)
- **SINK pad** — receives data (sink / input)

Pads are linked peer-to-peer:
```
src_pad → peer → sink_pad
sink_pad → peer → src_pad
```

Each pad carries optional **caps** (media type negotiation) and **push/pull** function pointers for both task-based and pull-based data flow.

### Element (zst_element)
A processing node in the pipeline. Elements implement the **ops** vtable:

| Op       | Called during state transition |
|----------|--------------------------------|
| `open`   | NULL → READY (allocate resources) |
| `close`  | READY → NULL (release resources) |
| `start`  | PAUSED → PLAYING (start streaming) |
| `stop`   | PLAYING → PAUSED (stop streaming) |
| `process`| Active streaming: transform `in` → `out` |

Elements own an array of source pads and sink pads. Multi-pad elements (e.g. a muxer with separate video/audio inputs) add multiple pads.

### State Machine

```
NULL  ──open──→  READY  ──start──→  PLAYING
  ↑                │                    │
  └──close──┘      └──────stop─────────┘
```

`PAUSED` is reserved but not yet wired — elements can optionally implement it for preroll.

### Pipeline (zst_pipeline)
An ordered container of elements. Its primary job is **state propagation** — calling `zst_element_set_state()` on every element in sequence.

```
pipe = zst_pipeline_create()
zst_pipeline_add(pipe, src)
zst_pipeline_add(pipe, encoder)
zst_pipeline_set_state(pipe, ZST_STATE_PLAYING)
```

### Queue (zst_queue)
Thread-safe blocking queue between processing stages.

- **SYNC mode**: bounded with back-pressure (push blocks when full)
- **ASYNC mode**: drops buffers when full (best-effort)
- Configurable limits: max buffers, max bytes, max duration
- Timeout-aware `push` / `pop` (including `timeout_ms=0` for try-lock)
- `flush` for cleanup on state transitions

Implemented with `pthread_mutex` + `pthread_condvar`.

### Queue Element (zst_queue_element)
A first-class zst_element subclass wrapping `zst_queue_t`. Unlike internal queues, the queue element is explicitly placed in the pipeline by the user. Each queue element has:

- One **sink pad** — receives buffers into the queue
- One **src pad** — pushes dequeued buffers downstream
- A **worker thread** that pops from the queue and pushes via `zst_pad_push()`

```
v4l2src → queue → x264enc → queue → mp4mux → queue → filesink
```

Every queue element is a threading boundary: upstream runs in its thread, downstream runs in the queue's thread.

### Scheduler (zst_scheduler)
Drives the pipeline's execution model.

| Mode            | Behaviour                            |
|-----------------|---------------------------------------|
| SINGLE_THREAD   | Sequential processing in calling thread |
| MULTI_THREAD    | Worker thread pool per element chain    |

In multi-thread mode each worker pops from its input queue, calls `process()`, and pushes to the next stage — a classic **pipeline parallelism** pattern.

### Element Implementations

The framework includes **50+ built-in and plugin element types** across nine categories. All registerable by factory name via `zst_element_factory_make()`:

**Source elements** — produce data into the pipeline:

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `videotestsrc` | Synthetic video test patterns | Core |
| `audiotestsrc` | Synthetic audio test tones | Core |
| `textsource` | Video frames containing timed text | Core |
| `filesrc` | Reads buffers from a local file | stdio |
| `httpsrc` | Reads buffers from HTTP/HTTPS servers | `libcurl` |
| `v4l2src` | Captures video from a V4L2 device (real + mock) | `libv4l2` |
| `alsasrc` | Captures audio from ALSA (real + mock) | `libasound` |
| `sc6f0src` | SC6F0 platform video/audio capture with dynamic signal detection | `libv4l2` + `libasound` |
| `netsrc` | Receives raw data from TCP/UDP/Unix sockets | Core |
| `rtspsrc` | Receives A/V from an RTSP endpoint (TCP interleaved + UDP) | `libavformat` |
| `rtmpsrc` | Receives A/V from an RTMP endpoint | `libavformat` |
| `srtsrc` | Receives data over Secure Reliable Transport (SRT) | `libsrt` |

**Sink elements** — consume data from the pipeline:

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `filesink` | Writes incoming buffers to a local file | stdio |
| `fakesink` | Consumes buffers and records statistics (testing) | Core |
| `v4l2sink` | Outputs video to a V4L2 loopback device (real + mock) | `libv4l2` |
| `alsasink` | Outputs audio to ALSA playback (real + mock) | `libasound` |
| `netsink` | Sends raw data over TCP/UDP/Unix sockets | Core |
| `rtspsink` | Publishes A/V to an RTSP endpoint | `libavformat` |
| `rtmpsink` | Publishes A/V to an RTMP endpoint | `libavformat` |
| `srtsink` | Sends data over Secure Reliable Transport (SRT) | `libsrt` |
| `x11sink` | Displays raw video in an X11 window | Xlib |
| `glsink` | Displays video with GPU YUV→RGB conversion | OpenGL/X11 |
| `glcompsink` | Composites multiple raw video streams into one OpenGL window | OpenGL/X11 |

**Codec / Encoder elements:**

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `x264enc` | Encodes raw video to H.264 (ultrafast, CRF) | `libx264` |
| `x265enc` | Encodes raw video to H.265 | `libx265` |
| `aacenc` | Encodes raw audio to AAC (S16→FLTP) | `libavcodec` |
| `nvenc` | Encodes raw video to H.264/H.265 via V4L2 | NVIDIA V4L2 |
| `oneapienc` | Encodes raw video via Intel oneVPL | oneAPI/SYCL |
| `vaapienc` | Encodes raw video via Linux VA-API | VA-API |

**Codec / Decoder elements:**

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `h264dec` | Decodes H.264 video to raw frames | `libavcodec` |
| `h265dec` | Decodes H.265 video to raw frames | `libavcodec` |
| `aacdec` | Decodes AAC audio to raw PCM | `libavcodec` |
| `nvdec` | Decodes H.264/H.265 via V4L2 | NVIDIA V4L2 |
| `vaapidec` | Decodes H.264/H.265 via Linux VA-API | VA-API |

**Muxer / Demuxer elements:**

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `mp4mux` | Muxes encoded A/V into fragmented MP4 | `libavformat` |
| `mp4demux` | Demuxes MP4/MOV/M4A into encoded A/V (dynamic pads) | `libavformat` |
| `tsmux` | Muxes encoded A/V into MPEG-TS (.ts) | `libavformat` |
| `tsdemux` | Demuxes MPEG-TS into encoded A/V (dynamic pads) | `libavformat` |
| `sdpmuxer` | Generates SDP descriptions for RTP sessions | Core |

**Filter / Processing elements:**

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `queue` | Thread-safe bounded buffer queue element | Core |
| `videoscaler` | Video resolution/format conversion | `libswscale` |
| `audioresampler` | Sample rate / channel / format conversion + ASRC drift | `libswresample` |
| `textoverlay` | Overlays timed text on video frames | `libfreetype` |
| `audiomixer` | Synchronous multi-input audio mixing with per-pad pan/volume | Core |
| `nvvideoscaler` | Hardware video scaling via V4L2 | NVIDIA V4L2 |

**RTP / Streaming elements:**

| Factory Name | Description | Library |
|-------------|-------------|---------|
| `rtppay` | Packetizes H.264/H.265/AAC/PCM into RTP buffers | Core |
| `rtpdepay` | Depayloads RTP packets back to access units | Core |
| `srt_parser` | Parses SubRip (SRT) subtitle data | Core |
| `rtsp_server` | Multi-session RTSP server with dynamic mount callbacks | `libavformat` |

All elements implement the standard `zst_element_ops_t` vtable, register via the builtin factory or as dlopen plugins, and participate in pipeline state machines, caps negotiation, and buffer pooling.

### Plugin (zst_plugin)
Dynamic element loading via `dlopen()`:

- Each `.so` exports `zst_get_plugin()`
- Plugin descriptor carries name, author, version
- Element factory function creates named elements

## Pipeline Data Flow

```
┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐    ┌──────────┐
│ v4l2src  │───→│ queue_el  │───→│ x264enc  │───→│ queue_el  │───→│ mp4mux   │───→│ queue_el  │───→│ filesink │
└──────────┘    └───────────┘    └──────────┘    └───────────┘    └──────────┘    └───────────┘    └──────────┘

┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────┐      ┆
│ alsasrc  │───→│ queue_el  │───→│ aacenc   │───→│ queue_el  │──────┘
└──────────┘    └───────────┘    └──────────┘    └───────────┘
```

Explicit queue elements define threading boundaries. The scheduler assigns
one thread per source element; queue elements each have their own worker
thread for pushing downstream, decoupling producers from consumers.

## Design Principles

1. **Minimal dependencies** — core needs only pthreads; optional plugins bring in libv4l2, x264, ffmpeg.
2. **Zero-copy by default** — buffers are ref-counted and shared across pads.
3. **Explicit state machine** — every resource transition is traceable.
4. **Pluggable everything** — elements are loaded at runtime; scheduler strategy is configurable.
5. **C11** — portable, embeddable, FFI-friendly.

---

## Recently Implemented

The following features were previously planned and have been implemented. See `wiki/implementation-plan.md` for details.

### Event Bus  (✅ done — Phase 6)

An async notification channel (`zst_bus_t`) decoupled from the data path. Elements and the pipeline post events (`EOS`, `ERROR`, `STATE_CHANGED`) to the bus; applications listen via `zst_bus_pop()` or a callback.

### Caps Negotiation  (✅ done — Phase 5)

Pads now carry rich caps with dimensions, format, framerate, channels, sample rate.
- `zst_caps_intersect()` to find compatible formats
- Auto-negotiation at link time
- Video scaler (`libswscale`) and audio resampler (`libswresample`) are available
  for format conversion — see Phase 4g/4h in `implementation-plan.md`

### Allocator API  (✅ done — Phase 8a)

`zst_allocator_t` interface for custom memory backends:
- [x] Default CPU allocator (malloc/free) with refcounting
- [x] Integrated with `zst_buffer_create_with_allocator()`
- [x] DMABUF (Linux dma-buf for zero-copy between HW blocks)
- [x] CUDA / Vulkan / oneAPI (SYCL) device memory
- [x] Buffer pools to eliminate per-frame allocation

### Clock  (✅ done — Phase 8b)

`zst_clock_t` master clock wrapping `CLOCK_MONOTONIC`, with:
- [x] `zst_clock_get_time()` / `zst_clock_wait()`
- [x] Pipeline-level clock selection (`zst_pipeline_set_clock`)
- [x] Clock slaving for A/V sync
- [x] Jitter measurement

### Adaptive Stream Demuxing  (✅ Done — Post-P0)

Support for dynamic demuxers whose streams and caps can change while running:
- [x] First-class stream model (`zst_stream_info_t`) and stream query APIs
- [x] Pad presence templates (`always` / `sometimes` / `request`)
- [x] Deferred pad destruction and safe dynamic pad add/remove helpers
- [x] Bus events (`PAD_ADDED`, `PAD_REMOVED`, `STREAM_ADDED`, `STREAM_REMOVED`, `CAPS_CHANGED`, etc.)
- [x] Downstream in-band pad events and sticky event replay on late linking
- [x] Caps v2 generic fields (`codec_data`, `profile`, `stream-format`, `alignment`, `language`)
- [x] Safe pipeline reconfiguration transaction APIs
- [x] Refactored `tsdemux` and `mp4demux` to use the dynamic stream model
- [x] Initial dynamic/late-linking tests
