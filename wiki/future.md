# Future Work

This document lists features that are still planned across all phases.

## Element Implementations — Phase 4

| Item | Description | Status |
|------|-------------|--------|
| 4q — RTMP Source | RTMP/FLV pull client for receiving live streams | ✅ Done |
| 4r — RTMP Sink | RTMP push client for publishing live streams | ✅ Done |

## Allocator & Pool — Phase 8a

| Item | Description | Status |
|------|-------------|--------|
| DMABUF allocator | Linux dma-buf for zero-copy GPU interop | 📝 Planned |
| CUDA/Vulkan allocators | Device memory allocators for GPU pipelines | 📝 Planned |
| Topology-aware pool sizing | Auto-adjust min_buffers based on queue count | 📝 Planned |
| Pool stress tests | Acquire/recycle loop, timeout, flush tests | 📝 Planned |

## Clock — Phase 8b

| Item | Description | Status |
|------|-------------|--------|
| A/V Sync (clock slaving) | Slave video/audio clocks to pipeline master clock | ✅ Done |

## Advanced Features — Phase 8c

| Item | Description | Status |
|------|-------------|--------|
| Element Bin | Composite sub-pipeline (nested elements, ghost pads) | 📝 Planned |
| Pad Probes | Buffer interception callbacks on pads | ✅ Done |
| Segment Seeking | NPT-based seek within a stream | 📝 Planned |

## Pipeline & CI — Phases 9-10

| Item | Description | Status |
|------|-------------|--------|
| CI Pipeline | GitHub Actions: build + test + static analysis | 📝 Planned |
| Doxygen API Reference | Auto-generated API documentation | 📝 Planned |
| Tutorials | Getting-started guides, pipeline examples | 📝 Planned |

## SRT Transport Protocol

| Item | Description | Status |
|------|-------------|--------|
| SRT source/sink | Secure Reliable Transport protocol (UDT-based) | 📝 Planned |

> **Note:** The `srt_parser` element (phase 4u) already implements SRT **subtitle** file parsing (`.srt`). The SRT **transport protocol** is a separate feature.
