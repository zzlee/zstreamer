# ST2110 Implementation Notes

This document contains detailed notes regarding the implementation of SMPTE ST 2110 phases.

## Phase 1
Implemented foundational ST2110 elements:
- `st2110_20_payloader` and `st2110_20_depayloader` for uncompressed video.
- `st2110_30_payloader` and `st2110_30_depayloader` for uncompressed audio.
- SDP extensions for ST2110 support.

## Phase 2
Implemented timing and synchronization elements:
- `ptp_clock` element for IEEE 1588-2019 precision timing.
- Added PTP-aware scheduling logic in the main scheduler loop.

## Phase 3
Implemented redundancy and ancillary data features:
- `st2110_redundancy_mux` and `st2110_redundancy_demux` for ST 2022-7 dual-link redundancy.
- `st2110_40_payloader` and `st2110_40_depayloader` for ancillary data streaming.