# zstreamer

A lightweight, modular multimedia streaming and pipeline framework written in **C11**.
`zstreamer` implements a **GStreamer-inspired** architecture: elements connected via pads,
data flowing as reference-counted buffers through thread-safe queues, driven by a
configurable scheduler.

---

## ✨ Highlights

- **GStreamer-like pipeline model** — elements, pads, buffers, queues, caps negotiation
- **53 built-in elements** — capture, encode/decode, mux, network I/O, RTSP, text overlay
- **Thread-safe design** — bounded queues, atomic ref-counting, multi-threaded scheduler
- **Real codec support** — x264, FFmpeg (H.264/H.265/AAC encode & decode), libswscale, libswresample
- **RTSP server & client** — multi-session server (TCP interleaved + UDP), RTSP source/sink
- **Dynamic plugins** — `dlopen`-based element loading at runtime
- **Async event bus** — error, EOS, state-change, and warning notifications
- **Clock & A/V sync** — system clock, QoS dropping, clock slaving
- **Buffer pools** — pre-allocated buffer recycling with custom allocator interface
- **Lightweight logging** — compile-time level filtering with custom handler support
- **171 unit tests** — core framework, scheduler, caps, bus, plugins, conversion, clock, elements

---

## 📂 Repository Layout

```
.
├── include/              Public API headers (17 headers)
│   └── zstreamer/
│       └── elements/     Per-element convenience headers (38 headers)
├── src/                  Core framework + 53 element implementations
├── tests/                Unit tests, examples, demos
│   ├── test_core.c       171 unit tests covering all core modules
│   ├── test_net_source.c Network source smoke test
│   ├── test_net_sink.c   Network sink smoke test
│   ├── example_record.c  V4L2 + ALSA → H.264/AAC → MP4 pipeline
│   ├── demo_colorbar_mp4.c  Colorbar + timecode + tone → MP4 demo
│   └── example_text_overlay.c  Text overlay example
├── wiki/                 Architecture docs & implementation plan
├── cmake/                pkg-config & CMake export templates
├── CMakeLists.txt        Build configuration
├── Dockerfile            Ubuntu 24.04 dev/CI container
└── AGENTS.md             Project context for AI coding agents
```

---

## 🚀 Quick Start

### Prerequisites

| Library | Package (Ubuntu 24.04) |
|---------|------------------------|
| FFmpeg (libavformat, libavcodec, libavutil) | `libavformat-dev libavcodec-dev libavutil-dev` |
| x264 | `libx264-dev` |
| libswscale | `libswscale-dev` |
| libswresample | `libswresample-dev` |
| ALSA | `libasound2-dev` |
| V4L2 | `libv4l-dev` |
| FreeType | `libfreetype-dev` |
| pthreads | (system) |

### Build (Native)

```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

### Build (Docker) — Fastest

```bash
# One-shot: build + test
docker build -t zstreamer .
docker run --rm zstreamer

# Interactive shell
docker run --rm -it zstreamer bash

# Live code mount (edit on host, rebuild in container)
docker run --rm -it -v $(pwd):/workspace zstreamer bash
# inside: cd /workspace/build && cmake .. && make -j && ctest -V
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `ON` | Build test programs and examples |
| `BUILD_SHARED` | `OFF` | Build core as `.so` instead of `.a` |
| `ENABLE_PLUGINS` | `ON` | Enable `dlopen`-based plugin loading |

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        zst_pipeline                         │
│  (container — owns elements, propagates state, holds clock) │
│                                                             │
│   ┌──────────┐    ┌──────────┐    ┌──────────┐             │
│   │ element  │    │  queue   │    │ element  │             │
│   │ [src]────┼───►│ [sink]   │───►│ [sink]   │             │
│   │          │pad │ [src]────┼pad │          │             │
│   └──────────┘    └──────────┘    └──────────┘             │
│                                                             │
│   zst_buffer (ref-counted) flows through pads & queues      │
│   zst_scheduler drives push/pull across the graph           │
└─────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | Role |
|-----------|------|
| **zst_pipeline** | Element container; state propagation; holds pipeline clock |
| **zst_element** | Processing node with src/sink pads + ops vtable |
| **zst_pad** | Peer-to-peer connection point between elements |
| **zst_buffer** | Ref-counted data carrier with typed memory + timestamps |
| **zst_queue** | Thread-safe bounded queue (mutex + condvar) |
| **zst_queue_element** | Queue as a first-class pipeline element with worker thread |
| **zst_scheduler** | Pipeline driver — single-thread inline or multi-thread pool |
| **zst_caps** | Media type negotiation (format, resolution, sample rate) |
| **zst_bus** | Async event bus for error/EOS/state/warning notifications |
| **zst_plugin** | `dlopen()`-based dynamic element loading |
| **zst_clock** | System clock + pipeline-level A/V sync with QoS |
| **zst_allocator** | Memory allocator interface |
| **zst_buffer_pool** | Pre-allocated buffer recycling |
| **zst_log** | Lightweight logging with compile-time level filtering |

### State Machine

```
ZST_STATE_NULL  ──open──►  ZST_STATE_READY  ──start──►  ZST_STATE_PLAYING
     ▲                          │                              │
     └────────close─────────────┘               stop───────────┘
```

---

## 🔌 Supported Elements (53)

### Sources

| Element | Description |
|---------|-------------|
| `v4l2_source` | V4L2 camera capture (real V4L2 + mock fallback) |
| `alsa_source` | ALSA audio capture (real ALSA + mock fallback) |
| `file_source` | FILE* reader |
| `video_test_src` | Synthetic video test pattern (colour bars) |
| `audio_test_src` | Synthetic audio tone generator |
| `text_source` | Timed text / subtitle source |
| `net_source` | TCP/UDP network source (raw bytes) |
| `http_source` | HTTP/HTTPS network source |
| `rtmp_source` | RTMP client source |
| `srt_source` | Secure Reliable Transport (SRT) source |
| `rtsp_source` | RTSP client source (TCP interleaved + UDP) |

### Encoders / Decoders

| Element | Description |
|---------|-------------|
| `x264_encoder` | H.264 encoder via x264 |
| `h264_decoder` | H.264 decoder via FFmpeg libavcodec |
| `h265_encoder` | H.265/HEVC encoder via FFmpeg libavcodec |
| `h265_decoder` | H.265/HEVC decoder via FFmpeg libavcodec |
| `aac_encoder` | AAC audio encoder via FFmpeg libavcodec |
| `nv_video_encoder` | Hardware H.264/H.265 encoder via NV V4L2 |
| `vaapi_video_encoder` | Hardware H.264/H.265 encoder via Linux VA-API |
| `nv_video_decoder` | Hardware H.264/H.265 decoder via NV V4L2 |
| `aac_decoder` | AAC audio decoder via FFmpeg libavcodec |
| `oneapi_video_encoder` | Hardware H.264/H.265 encoder via Intel oneVPL |
| `vaapi_video_decoder` | Hardware H.264/H.265 decoder via Linux VA-API |

### Filters / Converters

| Element | Description |
|---------|-------------|
| `video_scaler` | Pixel format + resolution conversion via libswscale |
| `audio_resampler` | Sample rate + format conversion via libswresample |
| `text_overlay` | Burn text / timecode onto video frames (FreeType) |
| `nv_video_scaler` | Hardware video scaler via NV V4L2 |
| `srt_parser` | SRT subtitle file parser |
| `audiomixer` | Mixes multiple audio streams into one |

### Muxers / Sinks / Servers

| Element | Description |
|---------|-------------|
| `mp4_muxer` | MP4 container muxer via libavformat |
| `mp4_demuxer` | MP4 container demuxer |
| `mpegts_demuxer` | MPEG-TS demuxer |
| `file_sink` | FILE* writer |
| `fake_sink` | Null sink (for testing / benchmarks) |
| `net_sink` | TCP/UDP network sink (raw bytes) |
| `rtsp_sink` | RTSP client sink |
| `rtmp_sink` | RTMP client sink |
| `srt_sink` | Secure Reliable Transport (SRT) sink |
| `mpegts_muxer` | MPEG-TS container muxer |
| `rtsp_server` | Multi-session RTSP server (TCP interleaved + UDP unicast) |
| `sdpmuxer` | Generates SDP descriptions for H.264/H.265/AAC RTP sessions |
| `rtppay` | Packetizes buffers into RTP packets |
| `rtpdepay` | Depayloads RTP packet buffers into access units |
| `x11sink` | Displays raw video frames in an X11 window |
| `glsink` | Displays video frames in an OpenGL window |
| `glcompsink` | Composites multiple video streams into one OpenGL window |

---

## 💡 Usage Example

A minimal pipeline that captures video and audio, encodes, muxes to MP4 and writes to disk:

```c
#include "zst_pipeline.h"
#include "zst_scheduler.h"
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"

int main(void) {
    zst_register_builtin_elements();

    zst_pipeline_t* pipe = zst_pipeline_create();

    /* Create elements via factory */
    zst_element_t* vsrc  = zst_element_factory_make("videotestsrc",  "vsrc");
    zst_element_t* h264  = zst_element_factory_make("x264enc",       "enc");
    zst_element_t* asrc  = zst_element_factory_make("audiotestsrc",  "asrc");
    zst_element_t* aac   = zst_element_factory_make("aacenc",        "aac");
    zst_element_t* mux   = zst_element_factory_make("mp4mux",        "mux");
    zst_element_t* sink  = zst_element_factory_make("filesink",      "out");

    zst_element_set_property_string(sink, "location", "output.mp4");

    /* Add to pipeline */
    zst_pipeline_add(pipe, vsrc);
    zst_pipeline_add(pipe, h264);
    zst_pipeline_add(pipe, asrc);
    zst_pipeline_add(pipe, aac);
    zst_pipeline_add(pipe, mux);
    zst_pipeline_add(pipe, sink);

    /* Link: vsrc→h264→mux, asrc→aac→mux, mux→sink */
    zst_pad_link(vsrc->src_pad, h264->sink_pad);
    zst_pad_link(h264->src_pad, zst_element_get_pad(mux, "video"));
    zst_pad_link(asrc->src_pad, aac->sink_pad);
    zst_pad_link(aac->src_pad,  zst_element_get_pad(mux, "audio"));
    zst_pad_link(mux->src_pad,  sink->sink_pad);

    /* Run */
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 4
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
    zst_scheduler_set_pipeline(sched, pipe);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    /* Cleanup */
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);
    return 0;
}
```

See [`tests/demo_colorbar_mp4.c`](tests/demo_colorbar_mp4.c) for a complete working demo with typed properties.

---

## 🧪 Testing

```bash
# Run all CTest tests (from build directory)
ctest --output-on-failure

# Verbose output
ctest -V
```

**Test suites registered with CTest:**

| Test | Description |
|------|-------------|
| `test_core` | 171 unit tests: buffer, pad, element, pipeline, queue, scheduler, caps, bus, plugins, logging, scaler, resampler, decoders, allocator, clock, text overlay, elements |
| `test_net_source` | Network source smoke test |
| `test_net_sink` | Network sink smoke test |
| `test_install` | Verifies `cmake --install` layout (headers, libs, pkg-config, plugins) |

---

## 📦 Installation

```bash
cd build
cmake --install . --prefix /usr/local
```

**Installed layout:**

```
/usr/local/
├── include/zstreamer/          # Public API headers
│   └── elements/               # Per-element convenience headers
├── lib/
│   ├── libzstreamer.a          # Core framework
│   ├── libzstreamer-elements.a # All built-in elements
│   ├── zstreamer/plugins/      # Dynamic plugin .so files
│   ├── pkgconfig/
│   │   ├── zstreamer.pc
│   │   └── zstreamer-elements.pc
│   └── cmake/zstreamer/        # CMake export files
│       ├── zstreamerConfig.cmake
│       ├── zstreamerConfigVersion.cmake
│       └── zstreamerTargets.cmake
```

**Using from another CMake project:**

```cmake
find_package(zstreamer REQUIRED)
target_link_libraries(my_app PRIVATE zstreamer::zstreamer zstreamer::zstreamer-elements)
```

**Using with pkg-config:**

```bash
gcc my_app.c $(pkg-config --cflags --libs zstreamer-elements) -o my_app
```

---

## 📖 Documentation

| Document | Description |
|----------|-------------|
| [`wiki/architecture.md`](wiki/architecture.md) | Detailed design — buffers, pads, elements, scheduler |
| [`wiki/implementation-plan.md`](wiki/implementation-plan.md) | Top-level roadmap index |
| [`wiki/pipeline-flow.md`](wiki/pipeline-flow.md) | Scheduler push/pull flow diagram |
| [`wiki/clock-slaving.md`](wiki/clock-slaving.md) | A/V sync and clock slaving details |
| [`wiki/rtsp-server-example.md`](wiki/rtsp-server-example.md) | Multi-session RTSP server usage |
| [`wiki/future.md`](wiki/future.md) | Planned features |

---

## 🔧 Development Conventions

| Convention | Detail |
|------------|--------|
| Language | C11 (`-std=c11`) |
| Naming | `zst_` prefix, `snake_case` |
| Error handling | `zst_result_t` — `ZST_OK` (0) on success, negative on error |
| Ownership | Buffers are ref-counted; elements own pads; pipeline owns elements |
| Thread safety | Queue is MT-safe; buffer refcount is atomic; element/pipeline ops serialised by scheduler |

---

## 📋 Project Status

| Area | Status |
|------|--------|
| Core framework (buffer, pad, element, pipeline, queue, scheduler) | ✅ Complete |
| Caps negotiation, event bus, dynamic plugins, logging | ✅ Complete |
| 53 element implementations | ✅ Complete |
| Clock, A/V sync, QoS | ✅ Complete |
| Allocator & buffer pool | ✅ Complete |
| Intel oneAPI video encoder | ✅ Complete |
| VA-API video encoder | ✅ Complete |
| Element public API (factory, descriptors, typed properties) | ✅ Complete |
| Installation (pkg-config, CMake export) | ✅ Complete |
| RTMP source / sink | ✅ Complete |
| Element bins | 📝 Planned |
| CI pipeline | 🔄 In Progress |

---

## License

This project is licensed under the GNU General Public License v2.0 (GPLv2). See the [`LICENSE`](LICENSE) file for details.
