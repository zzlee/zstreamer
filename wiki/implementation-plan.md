# Implementation Plan

All phases are now documented in separate files for easier maintenance.

| Phase | Document | Lines | Status |
|-------|----------|-------|--------|
| 0–3   | [Core Framework](phase-core.md) | ~190 | ✅ Complete |
| 4     | [Element Implementations](phase-elements.md) | ~450 | ✅ Done (34 implemented) + planned additions |
| 5–7   | [Infrastructure](phase-infrastructure.md) | ~58 | ✅ Complete |
| 8     | [Advanced Features](phase-advanced.md) | ~417 | 🔄 In Progress |
| 9–10  | [Future Work](phase-future.md) | ~34 | ⬜ Not Started |
| Post-P0 | [RTMP Hardening](phase-rtmp-hardening.md) | ~60 | ✅ Done |
| Post-P0 | [RTSP Media-On-Demand](phase-rtsp-mod.md) | ~70 | ⬜ Not Started |
| Bugfix  | [Clock Sync Debug](clock-sync-debug.md) | ~200 | ✅ Fixed — scheduler clock sync comparison was broken |

---

## Quick Status

| Area | Status | Notes |
|------|--------|-------|
| Scaffolding (0) | ✅ Done | CMake, Docker, git, AGENTS.md |
| Core Framework (1) | ✅ Done | All 8 core modules |
| Scheduler (2) | ✅ Done | Topological sort, push/pull, EOS |
| Queue Element (3) | ✅ Done | First-class queue with worker thread |
| Logging (3.5) | ✅ Done | Compile-time log levels, thread-safe |
| Elements (4) | ✅ Done + planned additions | 34 elements implemented; HTTP Source done |
| Caps Negotiation (5) | ✅ Done | Intersection, auto-negotiation |
| Event Bus (6) | ✅ Done | Error/state/EOS notifications |
| Dynamic Plugins (7) | ✅ Done | dlopen-based loading |
| Allocator API (8a) | ✅ Done | Pool + elements migration done; comprehensive pool unit tests completed |
| Clock (8b) | ✅ Done | System clock, pipeline integration |
| Testing & CI (9) | ⬜ Planned | CI pipeline, stress tests, static analysis |
| Documentation (10) | ⬜ Planned | Doxygen API ref, tutorials |
| Advanced Features (8c) | ✅ Done | Element bin, pad probes, segment seeking |
| Element Public API (8d) | ✅ Done | Descriptor ABI, plugin introspection, typed properties, official element metadata, convenience headers, library & installation layout |

---

## RTMP Source/Sink Hardening  (Post-P0)

Detailed tasks and checklist for RTMP Source/Sink Hardening have been moved to a separate document:
- [RTMP Hardening Plan & Status](phase-rtmp-hardening.md)


## RTSP Server Media-On-Demand (Post-P0)

Detailed tasks and checklist for RTSP Server Media-On-Demand refactoring have been moved to a separate document:
- [RTSP Media-On-Demand Refactoring Plan](phase-rtsp-mod.md)

