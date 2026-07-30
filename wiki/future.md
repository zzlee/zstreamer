# Future Work

This document lists features that are still planned across all phases.

## Element Implementations — Phase 4

| Item | Description | Status |
|------|-------------|--------|
| 4q — RTMP Source | RTMP/FLV pull client for receiving live streams | ✅ Done |
| 4r — RTMP Sink | RTMP push client for publishing live streams | ✅ Done |
| 4ag — Intel oneAPI Video Encoder | H.264/H.265 hardware encoder via oneVPL for Intel GPU pipelines | ✅ Done |
| 4ah — VA-API Video Encoder | H.264/H.265 hardware encoder via Linux VA-API for AMD/Intel GPU pipelines | ✅ Done |

## Allocator & Pool — Phase 8a

| Item | Description | Status |
|------|-------------|--------|
| DMABUF allocator | Linux dma-buf for zero-copy GPU interop | ✅ Done |
| CUDA/Vulkan allocators | Device memory allocators for GPU pipelines | ✅ Done |
| Intel oneAPI allocator | Device memory allocator for Intel GPU pipelines using SYCL | ✅ Done |
| Topology-aware pool sizing | Auto-adjust min_buffers based on queue count | ✅ Done |
| Pool stress tests | Acquire/recycle loop, timeout, flush tests | ✅ Done |

## Clock — Phase 8b

| Item | Description | Status |
|------|-------------|--------|
| A/V Sync (clock slaving) | Slave video/audio clocks to pipeline master clock | ✅ Done |

## Advanced Features — Phase 8c

| Item | Description | Status |
|------|-------------|--------|
| Element Bin | Composite sub-pipeline (nested elements, ghost pads) | ✅ Done |
| Pad Probes | Buffer interception callbacks on pads | ✅ Done |
| Segment Seeking | NPT-based seek within a stream | ✅ Done |

## Testing & CI — Phase 9

| Item | Description | Status |
|------|-------------|--------|
| CI Pipeline | GitHub Actions: build + test + static analysis | ✅ Done |

## Documentation — Phase 10

| Item | Description | Status |
|------|-------------|--------|
| Doxygen API Reference | Auto-generated API documentation | ✅ Done |
| Tutorials | Getting-started guides, pipeline examples | ✅ Done |
| Deep-Dives & Guides | Caps, bus, zero-copy, A/V sync, plugins | ✅ Done |

## SRT Transport Protocol

| Item | Description | Status |
|------|-------------|--------|
| SRT source/sink | Secure Reliable Transport protocol (UDT-based) | ✅ Done |

> **Note:** The `srt_parser` element (phase 4u) already implements SRT **subtitle** file parsing (`.srt`). The SRT **transport protocol** is a separate feature.
