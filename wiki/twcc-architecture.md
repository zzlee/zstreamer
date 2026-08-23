# TWCC / GCC Architecture — zstreamer

Transport-Wide Congestion Control (TWCC) with Google Congestion Control (GCC) — implementation deep-dive.

---

## 1. Overview

WebRTC congestion control has evolved through two generations:

| Mechanism | Era | Estimator Side | Feedback | Granularity |
|-----------|-----|----------------|----------|-------------|
| **REMB** (RFC 5506) | Legacy | Receiver | RTCP REMB | Per-interval aggregate |
| **TWCC/CCFB** (RFC 8888) | Modern | **Sender** | RTCP Transport-CC Feedback | **Per-packet** |

TWCC gives the **sender** per-packet arrival information so it can run the **GCC (Google Congestion Control)** algorithm locally — a combined delay-based and loss-based AIMD estimator.

---

## 2. Data Flow

```
┌────────────────────────────────────────────────────────────────┐
│                        SENDER (zstreamer)                      │
│                                                                │
│  x264enc                                                       │
│      │ H264 frames                                             │
│      ▼                                                         │
│  webrtc_endpoint (outgoing RTP interceptor)                    │
│      │                                                         │
│      ├── zst_webrtc_twcc_process_outgoing()                    │
│      │       • Assign transport-wide seq_num (16-bit, global)  │
│      │       • Record send_time_us in history ring buffer      │
│      │       • Inject 0xBEDE extension into RTP header         │
│      │                                                         │
│      ▼ SRTP encrypt                                            │
│      │ UDP/SRTP packets ──────────────────────────────────────►│
│                                                                │
│  webrtc_endpoint (incoming RTCP interceptor)                   │
│      │ RTCP CCFB (decrypted by libdatachannel)                 │
│      ▼                                                         │
│  zst_webrtc_twcc_process_incoming()                            │
│      │                                                         │
│      └── twcc_parse_ccfb()                                     │
│              • Decode CCFB chunks (Run-Length / Status-Vector) │
│              • Reconstruct arrival times (ref_time + Σ deltas) │
│              • Compute Δdelay per packet                        │
│              • Run GCC estimators (delay + loss)               │
│              • Post ZST_EVENT_WEBRTC_REMB on bus               │
│                                                                │
│  bus_thread                                                    │
│      └── zst_element_set_property(venc, "bitrate", bps)        │
└────────────────────────────────────────────────────────────────┘
                              ▲
                   RTCP CCFB │ (UDP, ~200ms interval)
                              │
┌─────────────────────────────┴──────────────────────────────────┐
│                        RECEIVER (Chrome)                        │
│                                                                │
│  Receives RTP packets with transport-wide-cc-02 extension      │
│  Records: {seq_num → arrival_time_ms}                          │
│  Every ~200ms: sends RTCP Transport Feedback (PT=205, FMT=15)  │
│  containing arrival times for each received/lost packet        │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. RTP Header Extension Injection

Every outgoing RTP packet gets a one-byte-header extension block added by `zst_webrtc_twcc_process_outgoing()`:

```
Before injection (no extension):
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
│V=2│P│X│  CC   │M│     PT    │          Sequence Number          │
├─┴─┴─┴─┴─┴─┴─┴─┼─┴─┴─┴─┴─┴─┴─┴─┼─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┤
│                           Timestamp                             │
├───────────────────────────────────────────────────────────────┤
│                             SSRC                               │
├───────────────────────────────────────────────────────────────┤
│                          Payload...                            │
└───────────────────────────────────────────────────────────────┘

After injection (X=1, 8-byte extension block prepended before payload):
...header (X bit set)...
┌───────────────────────────────────────────────────────────────┐
│          0xBEDE          │       length = 1 (word)            │  ← RFC 5285 one-byte header magic
├───────────────────────────────────────────────────────────────┤
│  ID(4b)│len-1=1│  seq_hi (8b)  │  seq_lo (8b)  │  padding    │  ← TWCC ext
├───────────────────────────────────────────────────────────────┤
│                          Payload...                            │
└───────────────────────────────────────────────────────────────┘
```

- `ID` = the extmap ID negotiated in SDP (typically 3 or 5, from Chrome's offer)
- `seq` = 16-bit monotonic transport-wide sequence number (shared across all tracks)

---

## 4. RTCP CCFB Packet Format (RFC 8888)

Chrome sends Transport-Wide Feedback every ~200ms:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
│V=2│P│  FMT=15 │    PT=205   │              length               │  ← RTPFB header
├───────────────────────────────────────────────────────────────┤
│                       Sender SSRC                              │
├───────────────────────────────────────────────────────────────┤
│                       Media Source SSRC                        │
├─────────────────────────────┬─────────────────────────────────┤
│       Base Seq Number       │       Packet Status Count        │  ← byte 12–15
├─────────────────────────────┴──────────────┬──────────────────┤
│           Reference Time (24 bits)          │  FB Pkt Count   │  ← byte 16–19
├───────────────────────────────────────────────────────────────┤
│                   Packet Status Chunks                         │  ← 2 bytes each
│          (Run-Length Encoding or Status Vector)                │
├───────────────────────────────────────────────────────────────┤
│               Receive Delta Vector (variable)                  │  ← 1 or 2 bytes each
└───────────────────────────────────────────────────────────────┘
```

### Status Chunk Types

**Run-Length Encoding Chunk** (`chunk[15] = 0`):
```
 0               1
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
│0│ status(2b) │    run length     │
└─┴─────────────────────────────────┘
status: 0=not received, 1=received (small delta), 2=received (large delta)
```

**Status Vector Chunk** (`chunk[15] = 1`):
```
 0               1
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
│1│S│  symbol list (14 or 7 syms) │
└─┴─────────────────────────────────┘
S=0: 1-bit per symbol (0=not recv, 1=recv)  → 14 packets per chunk
S=1: 2-bit per symbol (status code)         →  7 packets per chunk
```

### Receive Delta Encoding

Each received packet contributes one delta entry after the status chunks:
- **Small delta** (status=1): 1 byte, unit = 250µs, range [0, 63.75ms]
- **Large delta** (status=2): 2 bytes signed, unit = 250µs, range [−8192ms, +8191.75ms]

Arrival times are reconstructed as running sum: `t_i = ref_time + Σ delta_j`

---

## 5. GCC Algorithm

The GCC algorithm runs in two parallel estimators that are then combined.

### 5.1 Delay-Based Estimator (AIMD)

```
For each feedback packet:
  1. Compute Δdelay = (arrival_i − arrival_{i−1}) − (send_i − send_{i−1})
     • Positive → queuing build-up (congestion)
     • Negative → queue draining
     • Near zero → stable

  2. Smooth with EMA: gradient = γ·gradient + (1−γ)·Δdelay   (γ = 0.9)

  3. Update adaptive threshold:
     if |gradient| > threshold:
         threshold += α·(|gradient| − threshold)   (fast track-up)
     else:
         threshold −= α·0.1·threshold               (slow decay)
     clamp(threshold, 6ms, 600ms)

  4. Classify state:
     gradient >  threshold  → OVERUSE
     gradient < −threshold  → UNDERUSE
     else                   → NORMAL

  5. Apply AIMD rate control (every 100ms):
     OVERUSE  → delay_estimate *= β   (β = 0.85)
     NORMAL   → delay_estimate *= (1 + α_inc)   (α_inc = 0.08)
     UNDERUSE → hold (no change)
```

### 5.2 Loss-Based Estimator

```
For each feedback window:
  loss_rate = lost / (lost + received)
  smooth: loss = 0.5·loss + 0.5·loss_rate   (EMA)

  if loss > 10%:  loss_estimate *= 0.80   (−20%)
  if loss  2-10%: hold
  if loss < 2%:   loss_estimate *= 1.05   (+5%)
```

### 5.3 Combined GCC

```
current_bitrate = min(delay_estimate, loss_estimate)
clamp(current_bitrate, 100kbps, 8Mbps)

Post ZST_EVENT_WEBRTC_REMB only if |new − prev| / prev > 3%
```

### State Machine Diagram

```
                    ┌──────────────┐
         ┌─────────►│    NORMAL    │◄──────────┐
         │  loss<2% │ additive +8% │ queue drain│
         │          └──────┬───────┘           │
         │                 │ gradient           │
         │                 │ > threshold        │
         │                 ▼                   │
    ┌────┴─────────┐  ┌────────────┐  ┌────────┴─────┐
    │  UNDERUSE    │  │  OVERUSE   │  │ loss > 10%   │
    │   HOLD       │  │  ×0.85     │  │  × 0.80      │
    └──────────────┘  └────────────┘  └──────────────┘
```

---

## 6. SDP Negotiation

### Chrome Offer (contains TWCC extmap)

```sdp
v=0
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=rtpmap:96 H264/90000
a=rtpmap:97 VP8/90000
a=rtpmap:98 VP9/90000
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 goog-remb
```

### zstreamer Answer (TWCC injected)

```sdp
v=0
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42e01e;level-asymmetry-allowed=1;packetization-mode=1
a=rtcp-fb:96 goog-remb
```

`zst_webrtc_twcc_parse_offer()` extracts `extmap_id = 3` from the offer.
`zst_webrtc_twcc_inject_answer()` injects the matching line into the answer.

---

## 7. Module Structure

```
src/zst_webrtc_twcc.c
│
├── zst_webrtc_twcc_create(pc_id, bus)          ← Init per-PeerConnection state
├── zst_webrtc_twcc_destroy(twcc)               ← Cleanup
│
├── zst_webrtc_twcc_parse_offer(twcc, sdp)      ← Extract extmap ID from offer
├── zst_webrtc_twcc_inject_answer(twcc, sdp)    ← Inject extmap into answer
│
├── zst_webrtc_twcc_process_outgoing(twcc, pkt) ← Intercept outgoing RTP
│   ├── Assign seq_num
│   ├── Record send_time_us in ring buffer
│   └── Inject 0xBEDE extension
│
└── zst_webrtc_twcc_process_incoming(twcc, pkt) ← Intercept incoming RTCP
    └── twcc_parse_ccfb()
        ├── Decode status chunks
        ├── Reconstruct arrival times
        ├── Compute Δdelay per packet
        ├── gcc_delay_update() + gcc_delay_adapt()
        ├── gcc_loss_adapt()
        └── Post ZST_EVENT_WEBRTC_REMB → bus
```

### Key Data Structure

```c
struct zst_webrtc_twcc {
    int           pc_id;             // PeerConnection ID (from libdatachannel)
    zst_bus_t*    bus;               // Pipeline event bus
    int           extmap_id;         // Negotiated RTP extension ID (-1 = disabled)
    uint16_t      seq_num;           // Next transport-wide sequence number
    pthread_mutex_t lock;            // Protects seq_num and history

    // Sent packet history (ring buffer, 8192 entries)
    twcc_sent_packet_t history[8192]; // {seq, send_time_us, packet_size}

    // Delay-based GCC state
    float delay_gradient;            // EMA of inter-packet delay gradient
    float delay_threshold;           // Adaptive overuse threshold (ms)
    twcc_bw_state_t bw_state;        // OVERUSE / UNDERUSE / NORMAL

    // Previous feedback reference
    uint64_t last_recv_time_us;      // Last reconstructed receiver arrival time
    uint64_t last_send_time_us;      // Matching sender send time
    int      have_prev_ref;          // Whether reference is valid

    // Loss-based GCC state
    float loss_rate;                 // Smoothed loss fraction [0..1]

    // Output
    uint64_t delay_estimate_bps;     // Delay-based bitrate target
    uint64_t loss_estimate_bps;      // Loss-based bitrate target
    uint64_t current_bitrate_bps;    // Combined min(delay, loss)
    uint64_t last_update_us;         // Last GCC run timestamp
    uint64_t last_emitted_bps;       // Last posted REMB bitrate
};
```

---

## 8. libdatachannel Patch

Since libdatachannel's C API does not expose RTP/RTCP interceptors, `scripts/libdatachannel-twcc.patch` adds two functions:

```c
// Intercept outgoing RTP before SRTP encryption (per-track)
RTC_C_EXPORT int rtcSetTrackInterceptorCallback(
    int tr,
    rtcInterceptorCallbackFunc cb,
    void* ptr);

// Intercept incoming RTCP/RTP after SRTCP decryption (per-PeerConnection)
RTC_C_EXPORT int rtcSetMediaInterceptorCallback(
    int pc,
    rtcInterceptorCallbackFunc cb,
    void* ptr);

// Callback signature
typedef void* (*rtcInterceptorCallbackFunc)(
    const char* message,
    int size,
    void* ptr);
// Return original pointer to pass through, or rtcCreateOpaqueMessage() for modified packet
```

The patch is applied during Docker build in the `Dockerfile`:
```dockerfile
RUN git apply /workspace/scripts/libdatachannel-twcc.patch && \
    cmake -B build ... && cmake --build build ...
```

---

## 9. Performance Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| CCFB parse latency | < 1µs | Pure C, no alloc |
| GCC update interval | 100ms | Throttled by `TWCC_UPDATE_INTERVAL_US` |
| Bitrate convergence (0% loss) | ~2–5s | Ramp from 2Mbps to max |
| Bitrate recovery (50% loss) | ~1–2s | Drops to floor quickly |
| History ring buffer | 8192 entries | ~192KB, covers ~3min at 30fps |
| Thread safety | pthread_mutex | seq_num and history are lock-protected |

---

## 10. Testing

`tests/test_webrtc_twcc.c` — 6 unit tests:

| Test | What it Verifies |
|------|-----------------|
| `twcc_create_destroy` | Lifecycle, no leak |
| `twcc_extmap_parse` | Chrome SDP extmap ID extraction (ID=3) |
| `twcc_inject_answer` | Extmap line injected into answer SDP |
| `twcc_seq_numbering` | 10 outgoing packets get unique seq numbers |
| `twcc_ccfb_loss_decrease` | 50% synthetic loss drives bitrate from 2Mbps → minimum |
| `twcc_ccfb_no_loss_increase` | 0% loss drives bitrate from 2Mbps → above 2Mbps |

Run:
```bash
docker run --rm -v $(pwd):/workspace -w /workspace/build \
    zstreamer ./test_webrtc_twcc
```
