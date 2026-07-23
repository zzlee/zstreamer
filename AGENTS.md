# AGENTS.md — zstreamer

## Project Overview

`zstreamer` is a lightweight, modular multimedia streaming/pipeline framework written in C11.  
It provides a **GStreamer-like** pipeline architecture: elements connected via pads, data flowing as reference-counted buffers through thread-safe queues, driven by a configurable scheduler.

**GitHub:** https://github.com/zzlee/zstreamer

---

## Directory Layout

```
.
├── AGENTS.md          ← This file — project context for AI coding agents
├── CMakeLists.txt     ← CMake build system
├── Dockerfile         ← Ubuntu 24.04 dev environment
├── Dockerfile.gl      ← OpenGL/X11 glsink validation env (Xvfb + Mesa software rendering)
├── .dockerignore
├── .gitignore
├── include/           ← Public API headers
│   ├── zst_types.h        ← Base types, result codes, struct forward decls
│   ├── zst_buffer.h       ← Reference-counted buffer + typed memory
│   ├── zst_pad.h          ← SRC/SINK connection pads + probes/blocking/segments
│   ├── zst_element.h      ← Element ops vtable + state machine
│   ├── zst_bin.h          ← Composite element bins + ghost pads
│   ├── zst_segment.h      ← Segment seeking/clipping API
│   ├── zst_pipeline.h     ← Element container with state propagation
│   ├── zst_queue.h        ← Thread-safe bounded buffer queue
│   ├── zst_scheduler.h    ← Single / multi-thread pipeline driver
│   ├── zst_plugin.h       ← Dynamic plugin loading (dlopen)
│   ├── zst_bus.h          ← Async event bus for error/EOS/state notifications
│   ├── zst_caps.h         ← Caps negotiation (media type, resolution, format)
│   ├── zst_log.h          ← Lightweight logging system
│   ├── zst_clock.h        ← Clock for A/V sync
│   ├── zst_allocator.h    ← Memory allocator interface
│   ├── zst_buffer_pool.h  ← Pre-allocated buffer recycling
│   ├── zst_rtsp_server.h  ← RTSP server multi-session API
│   ├── zst_srt.h          ← SRT subtitle parser element
│   └── zst_st2110_sdp.h   ← ST2110 SDP extensions
├── src/               ← Core library + element implementations
│   ├── zst_buffer.c
│   ├── zst_bus.c
│   ├── zst_caps.c
│   ├── zst_element.c
│   ├── zst_pad.c
│   ├── zst_pipeline.c
│   ├── zst_queue.c
│   ├── zst_queue_element.c ← First-class queue element
│   ├── zst_bin.c          ← Element bins + ghost pads
│   ├── zst_scheduler.c
│   ├── zst_log.c          ← Logging implementation
│   ├── zst_plugin.c
│   ├── zst_clock.c        ← System clock + pipeline clock
│   ├── zst_allocator.c    ← Memory allocator
│   ├── zst_buffer_pool.c  ← Buffer pool
│   ├── v4l2_source.c      ← V4L2 camera capture (real V4L2 + mock fallback)
│   ├── v4l2_sink.c        ← V4L2 loopback/output sink (real V4L2 + mock fallback)
│   ├── alsa_source.c      ← ALSA audio capture (real ALSA + mock fallback)
│   ├── x264_encoder.c     ← x264 H.264 encoder (real x264)
│   ├── h264_decoder.c     ← FFmpeg libavcodec H.264 decoder
│   ├── h265_encoder.c     ← FFmpeg libavcodec H.265 encoder
│   ├── h265_decoder.c     ← FFmpeg libavcodec H.265 decoder
│   ├── aac_encoder.c      ← FFmpeg AAC audio encoder (real libavcodec)
│   ├── aac_decoder.c      ← FFmpeg AAC audio decoder
│   ├── mp4_muxer.c        ← FFmpeg/libavformat MP4 muxer (real libavformat)
│   ├── file_sink.c        ← FILE* writer
│   ├── file_source.c      ← FILE* reader
│   ├── fake_sink.c        ← Null sink for testing
│   ├── video_scaler.c     ← libswscale video format/resolution conversion
│   ├── audio_resampler.c  ← libswresample audio sample rate/format conversion
│   ├── video_test_src.c   ← Synthetic video test pattern source
│   ├── audio_test_src.c   ← Synthetic audio tone source
│   ├── text_overlay.c     ← Text overlay on video frames
│   ├── text_source.c      ← Timed text subtitle source
│   ├── srt_parser.c       ← SRT subtitle file parser
│   ├── srt_source.c       ← SRT source element
│   ├── srt_sink.c         ← SRT sink element
│   ├── srt_common.c       ← SRT shared helpers
│   ├── srt_plugin.c       ← Unified SRT plugin registration
│   ├── net_source.c       ← TCP/UDP network source (raw bytes)
│   ├── net_sink.c         ← TCP/UDP network sink (raw bytes)
│   ├── rtsp_source.c      ← RTSP client source (TCP interleaved + UDP)
│   ├── rtsp_sink.c        ← RTSP client sink
│   ├── rtsp_server.c      ← Multi-session RTSP server (TCP interleaved + UDP)
│   ├── rtmp_source.c      ← RTMP source (FLV demux)
│   ├── rtmp_sink.c        ← RTMP sink (FLV mux/publish)
│   ├── gl_sink.c          ← OpenGL/X11 display sink (GLX, GLSL YUV→RGB, null-mode fallback)
│   ├── mp4_demuxer.c      ← FFmpeg libavformat MP4 demuxer
│   ├── st2110_20_payloader.c ← ST2110-20 Video RTP payloader
│   ├── st2110_20_depayloader.c ← ST2110-20 Video RTP depayloader
│   ├── st2110_30_payloader.c ← ST2110-30 Audio RTP payloader
│   └── st2110_30_depayloader.c ← ST2110-30 Audio RTP depayloader
├── tests/
│   ├── test_core.c    ← Core unit tests: core + scheduler + queue + caps + bus + plugins + log + conversion + codecs + advanced features
│   ├── test_gl_sink.c ← glsink tests for factory/properties/lifecycle/Xvfb smoke coverage
│   ├── example_record.c ← Full pipeline demo with queue elements
│   └── demo_rtsp_mod.c  ← RTSP media-on-demand demo application

└── wiki/
    ├── architecture.md        ← Detailed design doc
    ├── implementation-plan.md ← Step-by-step roadmap (10 phases)
    ├── pipeline-flow.md       ← Scheduler flow diagram
    └── future.md              ← Planned features with Chinese notes
```

---

## Build
All compilation and testing for this project must be performed inside Docker containers to ensure environmental consistency.

```bash
# Docker — one-shot test (fastest, uses cached build)
docker build -t zstreamer .
docker run --rm zstreamer                     # runs ctest --output-on-failure

# Docker — verbose test output
docker run --rm --entrypoint bash zstreamer \
    -c "/workspace/build/ctest -V"

# Docker — interactive shell (source + build tree available)
docker run --rm -it zstreamer bash            # starts in /workspace
# then: cd /workspace/build && ctest -V

# Docker — live code mount (edit on host, rebuild in container, no docker build needed)
docker run --rm -it \
    -v $(pwd):/workspace \
    zstreamer bash
# then: cd /workspace/build && cmake .. && make -j && ctest -V

# Docker — rebuild after source changes (cache-friendly)
docker build -t zstreamer . && docker run --rm zstreamer

# Docker — Cross-compile for ARM64 (Petalinux / Xilinx SC6f0)
docker build -f Dockerfile.xlnk2_arm64 -t zstreamer-xlnk2-arm64 .
```

### Build Options

| Option            | Default | Description                           |
|-------------------|---------|---------------------------------------|
| `BUILD_TESTS`     | ON      | Build unit tests                      |
| `BUILD_SHARED`    | OFF     | Build core as `.so` instead of `.a`   |
| `ENABLE_PLUGINS`  | ON      | Enable dlopen-based plugin loading    |
| `ENABLE_JETSON`   | OFF     | Enable Jetson NvBuffer allocator support |
| `ENABLE_GLSINK`   | ON      | Build OpenGL/X11 display sink when X11/OpenGL dependencies are found |

### Docker Targets

The Dockerfile has two build targets:

| Target | Command                                    | Purpose                       |
|--------|--------------------------------------------|-------------------------------|
| `ci`   | `docker run --rm zstreamer`                 | One-shot `ctest` (default)    |
| `dev`  | `docker run --rm -it zstreamer bash`        | Interactive shell with build  |
| GL sink | `docker build -f Dockerfile.gl -t zstreamer-gl . && docker run --rm zstreamer-gl` | Headless glsink validation with Xvfb + Mesa software rendering |

### NVIDIA Jetson Build (Dockerfile.jetson)

For NVIDIA Jetson platforms (running JetPack/L4T), you can build the framework with native NvBuffer allocator support.

#### Prerequisites (Native Jetson Host)
- A Jetson host device (ARM64 running L4T).
- [NVIDIA Container Toolkit](https://github.com/NVIDIA/nvidia-container-toolkit) installed and configured on the host.

#### Building and Running on x86_64 Hosts (via QEMU Emulation)
If you want to build or run the ARM64 Jetson container on an x86_64 host, you must register `qemu-user-static` interpreters:

1. Register the QEMU interpreters on your x86_64 host:
   ```bash
   docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
   ```
2. Build specifying the ARM64 platform:
   ```bash
   docker build --platform linux/arm64 -f Dockerfile.jetson -t zstreamer-jetson .
   ```
3. Run under emulation (note: Tegra hardware-accelerated encoding/decoding will not be functional under emulation without the actual Jetson SoC hardware):
   ```bash
   docker run --rm -it --platform linux/arm64 zstreamer-jetson bash
   ```

#### Build the Jetson Image (Natively on Jetson)
Build the container image using the Jetson-specific Dockerfile:
```bash
docker build -f Dockerfile.jetson -t zstreamer-jetson .
```

#### Run the Jetson Container (Natively on Jetson)
To access the Jetson GPU, hardware video encoder/decoder, and `NvBuffer` hardware allocator from inside the container, you must run the container with the NVIDIA container runtime:

```bash
# Run unit tests inside the Jetson container
docker run --rm --runtime nvidia zstreamer-jetson

# Start an interactive developer shell
docker run --rm -it --runtime nvidia zstreamer-jetson bash
```

If you encounter device-access issues, manually map the required Tegra device nodes:
```bash
docker run --rm -it --runtime nvidia \
    --device /dev/nvhost-msenc \
    --device /dev/nvhost-ctrl \
    --device /dev/nvhost-ctrl-gpu \
    --device /dev/nvhost-vic \
    --device /dev/nvmap \
    zstreamer-jetson bash
```

---

## Release & Packaging

The project supports generating releases for both native x86_64 environments and cross-compiled ARM64 environments using the packaging script [package.sh](file:///home/zzlee/zstreamer/scripts/package.sh).

### 1. Native x86_64 Release
To package locally for the host architecture (x86_64):
```bash
./scripts/package.sh <version>
```
To run the packaging and publish via GitHub Releases automatically, push a tag matching `v*` (e.g. `v0.1.0`), which triggers the `.github/workflows/release.yml` pipeline.

### 2. Cross-Compiled ARM64 Release
To package for the ARM64 embedded platform (Petalinux / Xilinx SC6f0) using the cross-compilation Docker container:
```bash
docker run --entrypoint /bin/bash --rm \
    -e USER=root -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) \
    -v $(pwd):/workspace \
    yuan88yuan/xlnk2_arm64:v1 \
    -c "source /opt/qcap-dev-init && cd /workspace && ./scripts/package.sh <version>"
```

### Generated Output (in `dist/`)
* **x86_64/amd64**:
  * `dist/zstreamer-<version>-linux-x86_64.tar.gz` (and `.zip`)
  * `dist/zstreamer-elements-<version>-linux-x86_64.tar.gz` (and `.zip`)
  * `dist/zstreamer-dev_<version>_amd64.deb`
  * `dist/zstreamer-elements-dev_<version>_amd64.deb`
* **ARM64**:
  * `dist/zstreamer-<version>-linux-arm64.tar.gz` (and `.zip`)
  * `dist/zstreamer-elements-<version>-linux-arm64.tar.gz` (and `.zip`)
  * `dist/zstreamer-dev_<version>_arm64.deb`
  * `dist/zstreamer-elements-dev_<version>_arm64.deb`

---

## Architecture

| Component      | Role                                                  |
|----------------|-------------------------------------------------------|
| **zst_pipeline**| Container of elements; propagates state to all        |
| **zst_bin**     | Composite element container with ghost pads           |
| **zst_element** | Processing node with src/sink pads + ops vtable       |
| **zst_pad**     | Connection point; linked peer-to-peer between elements; probes/blocking and segment clipping |
| **zst_buffer**  | Ref-counted data carrier with typed memory + timestamps|
| **zst_queue**      | Thread-safe bounded queue (mutex + condvar)           |
| **zst_queue_element** | Queue as a first-class element with worker thread   |
| **zst_scheduler**    | Drives pipeline: single-thread inline or multi-thread pool |
| **zst_caps**     | Caps negotiation — media type, resolution, format intersection |
| **zst_bus**      | Async event bus for error/EOS/state/warning/segment notifications |
| **zst_plugin**   | `dlopen()`-based dynamic element loading              |
| **zst_log**      | Lightweight logging system with compile-time levels   |
| **video_scaler** | Pixel format + resolution conversion via `libswscale`  |
| **audio_resampler** | Sample rate + format conversion via `libswresample` |
| **mp4_demuxer**  | MP4/fMP4 file demuxer via `libavformat`                |

### State Machine

```
ZST_STATE_NULL  ──open──→  ZST_STATE_READY  ──start──→  ZST_STATE_PLAYING
     ↑                        │                              │
     └────────close───────────┘               stop────────────┘
```

`ZST_STATE_PAUSED` is reserved for future preroll support.

---

## Current Status

| Phase                       | Status                           |
|-----------------------------|----------------------------------|
| Scaffolding                 | ✅ CMake, Docker, git, AGENTS.md |
| Core Framework              | ✅ All 8 core modules implemented|
| Scheduler Integration       | ✅ Topological sort, push/pull, EOS, state hardening |
| Queue Element               | ✅ First-class queue with worker thread |
| Real Element Implementations| ✅ 37 elements: v4l2_source, v4l2_sink, alsa_source, alsa_sink, x264enc, x265enc, h264_decoder, h265_encoder, h265_decoder, aac_encoder, aac_decoder, mp4_muxer, mp4_demuxer, file_sink, file_source, fake_sink, video_scaler, audio_resampler, video_test_src, audio_test_src, text_overlay, text_source, srt_parser, net_source, net_sink, rtsp_source, rtsp_sink, rtsp_server, rtmp_source, rtmp_sink, srt_source, srt_sink, mpegts_muxer, mpegts_demuxer, http_source, glsink, audiomixer (with pan support) |
| Planned Element Additions   | 📝 x11sink, vaapidec (VA-API Video Decoder), Xilinx VCU encoder/decoder |
| Caps Negotiation            | ✅ Done                          |
| Event Bus                   | ✅ Done                          |
| Dynamic Plugins             | ✅ Done                          |
| Logging System              | ✅ Done                          |
| Unit Tests                  | ✅ Core tests + network tests + glsink tests; glsink validated via `Dockerfile.gl` (`test_gl_sink` + `test_core` pass under Xvfb/Mesa) |
| Allocator API + Pool        | ✅ Done (allocator interface, pool, CPU/DMABUF/CUDA/Vulkan/oneAPI allocators, elements migrated, topology-aware sizing, comprehensive tests) |
| Clock                       | ✅ Done (system clock + pipeline integration) |
| Element Public API (8d)     | ✅ Done (Descriptor ABI, plugin introspection, typed properties, official metadata, convenience headers, library & installation layout) |
| Text Overlay (4s)           | ✅ Included in Element Implementations |
| A/V Sync (clock slaving)    | ✅ Done (Scheduler wait integration, QoS dropping, and clock slaving verification tests) |
| RTSP Server Multi-Session (4z) | ✅ Done (port 8554, multiple mount points, per-client threads, H.264/AAC RTP, TCP interleaved + UDP unicast transport) |
| SRT Subtitle Parser (4u)    | ✅ Done                          |
| H.264 Decoder (4v)          | ✅ Done                          |
| H.265 Encoder               | ✅ Done                          |
| H.265 Decoder               | ✅ Done                          |
| AAC Decoder (4y)            | ✅ Done                          |
| Net Source / Net Sink (4m/n)| ✅ Done                          |
| RTSP Source (UDP support)   | ✅ Done (TCP interleaved + UDP unicast transport) |
| RTSP Sink (4p)              | ✅ Done                          |
| RTSP Server (UDP support)   | ✅ Done (TCP interleaved + UDP unicast transport) |
| RTMP Source/Sink (4q/4r)    | ✅ Done                          |
| Element Bins (8c)           | ✅ Done (composite bins + ghost pads) |
| Pad Probes / Blocking (8c)  | ✅ Done                          |
| Segment Seeking (8c)        | ✅ Done                          |
| SRT Transport Protocols     | ✅ Done                          |
| MPEG-TS mux/demux           | ✅ Done                          |
| MP4 Demuxer                 | ✅ Done (Phase F: refactored to dynamic pads) |
| Adaptive Stream Demuxing    | ✅ Done (Core APIs, dynamic-pad demuxers refactored, and comprehensive test coverage completed) |
| Audio Mixer (4am)           | ✅ Done (synchronous mixer with dynamic request sink pads, per-pad volume/mute, S16LE+F32LE, worker thread) |
| ASRC Drift Compensation     | ✅ Done (PTS-based drift detection + swr_set_compensation in audioresampler; passthrough bypass fix allows ASRC with equal nominal rates) |
| Fractional Rate Override    | ✅ Done (rate-numer/rate-denom properties on audioresampler for explicit fractional target rates; uses swr_set_compensation for fine-grained ratio adjustment) |
| OpenGL Sink (4ak)           | ✅ Initial implementation (GLX/X11 backend, GLSL YUV→RGB, null-mode fallback, dynamic plugin, Dockerfile.gl tests); known review follow-ups are tracked in [wiki/phase-elements.md](wiki/phase-elements.md#4ak--opengl-sink-glsink) |
| OpenGL Compositor Sink (4al) | ✅ Completed implementation with threaded rendering, clock-driven composition, dynamic pad removal, and enhanced UI features (borders, fullscreen). |
| Clock Sync Bugfix           | ✅ Fixed (frame-to-frame delta comparison in scheduler — see [wiki/clock-sync-debug.md](wiki/clock-sync-debug.md)) |
| CI Pipeline                 | ✅ Done (GitHub Actions CI with unit and docker-run loopback integration tests) |
| Documentation               | ✅ Phase 10 Done (Doxygen, Tutorials, Architecture Deep-Dives, Plugin Authoring) |
| ARM64 Cross-compilation     | ✅ Done (added optional dependency guards, CMake support, unit test skips, and build verification via Dockerfile.xlnk2_arm64) |
| WebRTC Phases 1-7           | ✅ Done (libdatachannel integration, signaling, H264/VP8/VP9 media send/recv, data channels, RTCP QoS, VP8/VP9 codec support — see [wiki/phase-webrtc.md](wiki/phase-webrtc.md)) |
| WebRTC Phase 8 (Chrome)     | ✅ Done (8a: multi-track routing - Done, 8b: TWCC filter - Done, 8c: WebSocket signaling - Done, 8d: SDP compat - Done, 8e: ICE restart - Done, 8f: codec selection - Done, 8g: demo server - Done, 8h: stun/turn - Done) |
| WebRTC Phase 9 (TWCC)       | ✅ Done (transport-cc-02 RTP header extension injection, RTCP CCFB RFC 8888 parsing, delay-based AIMD GCC estimator, loss-based estimator, combined GCC min(delay,loss), ZST_EVENT_WEBRTC_REMB bus events, encoder bitrate adaptation, test_webrtc_twcc) |
| WebRTC Phase 10 (Docs)      | ✅ Done (browser interop docs, examples, architecture diagrams) |
| ST2110 Phase 1 (Foundation) | ✅ Done (ST2110-20 video, ST2110-30 audio, SDP extensions) |
| ST2110 Phase 2 (Timing)     | ✅ Done (PTP clock element, scheduler PTP wait, ST2110-21 payloader properties) |

---

## Coding Conventions

- The project provides a DMABUF allocator fallback using `memfd_create`. Use `zst_allocator_dmabuf_create()` to create it, `zst_allocator_dmabuf_get_fd()` to export the file descriptor, and `zst_allocator_dmabuf_import()` to map an existing fd into the allocator.

- The project provides GPU and accelerator device allocators: `zst_allocator_cuda_create()`, `zst_allocator_vulkan_create()`, and `zst_allocator_oneapi_create()`. In CPU-only or non-supported environments, these constructors safely return `NULL`, which is handled gracefully by element negotiation and unit tests.

- **Language:** C11 (`-std=c11`)
- **Naming:** `zst_` prefix for all public symbols, `snake_case`
- **Error handling:** Return `zst_result_t` — `ZST_OK` (0) on success, negative on error
- **Ownership:** Buffers are ref-counted; elements own their pads; pipeline owns elements
- **Thread safety:** Queue is MT-safe; buffer refcount is atomic; element/pipeline ops are NOT thread-safe (serialised by scheduler)

## Key Files for Agent Context

When working on this project, the most important files to read first:

1. `include/zst_types.h` — All forward declarations and error codes
2. `include/zst_buffer.h` — Buffer structure (used everywhere)
3. `wiki/architecture.md` — Full architectural understanding
4. `wiki/implementation-plan.md` — Top-level plan index; sub-plans:
   - [`wiki/phase-core.md`](phase-core.md) — Phases 0–3 (scaffolding, framework, scheduler, queue)
   - [`wiki/phase-elements.md`](phase-elements.md) — Phase 4 (12+ element implementations)
   - [`wiki/phase-infrastructure.md`](phase-infrastructure.md) — Phases 5–7 (caps, bus, plugins)
   - [`wiki/phase-advanced.md`](phase-advanced.md) — Phase 8 (allocator, clock, advanced)
   - [`wiki/phase-testing-ci.md`](phase-testing-ci.md) — Phase 9 (testing, CI)
   - [`wiki/phase-documentation.md`](phase-documentation.md) — Phase 10 (docs)
   - [`wiki/phase-webrtc.md`](phase-webrtc.md) — WebRTC phases
   - [`wiki/phase-st2110.md`](phase-st2110.md) — SMPTE ST 2110 Network Video Standard Support
5. `CMakeLists.txt` — Build targets and dependencies
6. `src/zst_queue_element.c` — Queue element implementation
7. `src/v4l2_source.c` — Real V4L2 capture (reference for HW element pattern)
8. `src/x264_encoder.c` — Real x264 integration (reference for encoder pattern)

## Development Rules
* When implementing QoS (Quality of Service) frame dropping logic (e.g., `max-lateness`), carefully compare `zst_time_t` values. Since `zst_time_t` is unsigned, subtracting a property like `max-lateness` from the current time can cause underflow if the current time is small. Instead, use a safe check such as `if (current > buf->pts && (current - buf->pts) > max_lateness)`.
