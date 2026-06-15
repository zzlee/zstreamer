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
│   └── zst_srt.h          ← SRT subtitle parser element
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
│   ├── h264_encoder.c     ← x264 H.264 encoder (real x264)
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
│   ├── net_source.c       ← TCP/UDP network source (raw bytes)
│   ├── net_sink.c         ← TCP/UDP network sink (raw bytes)
│   ├── rtsp_source.c      ← RTSP client source (TCP interleaved + UDP)
│   ├── rtsp_sink.c        ← RTSP client sink
│   ├── rtsp_server.c      ← Multi-session RTSP server (TCP interleaved + UDP)
│   ├── rtmp_source.c      ← RTMP source (FLV demux)
│   ├── rtmp_sink.c        ← RTMP sink (FLV mux/publish)
│   └── mp4_demuxer.c      ← FFmpeg libavformat MP4 demuxer
├── tests/
│   ├── test_core.c    ← 62 unit tests: core + scheduler + queue + caps + bus + plugins + log + conversion + codecs + advanced features
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

```bash
# Native
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure

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
```

### Build Options

| Option            | Default | Description                           |
|-------------------|---------|---------------------------------------|
| `BUILD_TESTS`     | ON      | Build unit tests                      |
| `BUILD_SHARED`    | OFF     | Build core as `.so` instead of `.a`   |
| `ENABLE_PLUGINS`  | ON      | Enable dlopen-based plugin loading    |

### Docker Targets

The Dockerfile has two build targets:

| Target | Command                                    | Purpose                       |
|--------|--------------------------------------------|-------------------------------|
| `ci`   | `docker run --rm zstreamer`                 | One-shot `ctest` (default)    |
| `dev`  | `docker run --rm -it zstreamer bash`        | Interactive shell with build  |

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
| Real Element Implementations| ✅ 32 elements: v4l2_source, v4l2_sink, alsa_source, h264_encoder, h264_decoder, h265_encoder, h265_decoder, aac_encoder, aac_decoder, mp4_muxer, mp4_demuxer, file_sink, file_source, fake_sink, video_scaler, audio_resampler, video_test_src, audio_test_src, text_overlay, text_source, srt_parser, net_source, net_sink, rtsp_source, rtsp_sink, rtsp_server, rtmp_source, rtmp_sink, srt_source, srt_sink, mpegts_muxer, mpegts_demuxer |
| Planned Element Additions   | 📝 (none remaining) |
| Caps Negotiation            | ✅ Done                          |
| Event Bus                   | ✅ Done                          |
| Dynamic Plugins             | ✅ Done                          |
| Logging System              | ✅ Done                          |
| Unit Tests                  | ✅ 65 core tests + test_net_source + test_net_sink + install test, all passing |
| Allocator API + Pool        | ✅ Done (allocator interface, pool, elements migrated, topology-aware sizing, comprehensive tests) |
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
| MP4 Demuxer                 | ✅ Done                        |
| Clock Sync Bugfix           | ✅ Fixed (frame-to-frame delta comparison in scheduler — see [wiki/clock-sync-debug.md](wiki/clock-sync-debug.md)) |
| CI Pipeline                 | 📝 Future                        |

---

## Coding Conventions

- The project provides a DMABUF allocator fallback using `memfd_create`. Use `zst_allocator_dmabuf_create()` to create it, `zst_allocator_dmabuf_get_fd()` to export the file descriptor, and `zst_allocator_dmabuf_import()` to map an existing fd into the allocator.

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
   - [`wiki/phase-future.md`](phase-future.md) — Phases 9–10 (CI, docs)
5. `CMakeLists.txt` — Build targets and dependencies
6. `src/zst_queue_element.c` — Queue element implementation
7. `src/v4l2_source.c` — Real V4L2 capture (reference for HW element pattern)
8. `src/h264_encoder.c` — Real x264 integration (reference for encoder pattern)
