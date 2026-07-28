# ST2110-21 Implementation Plan (Compressed Video: H.264 / H.265)

## 🎯 Overview
SMPTE ST 2110-21 specifies the traffic shaping and delivery timing of uncompressed and compressed video. When combined with ST 2110-22 (Constant Bit Rate Compressed Video), it defines how compressed payloads should be transmitted over RTP. For H.264 and H.265 (HEVC), the payload mappings strictly follow RFC 6184 and RFC 7798, respectively, but with the added constraints of the ST 2110-21 timing model (CMAX, VRX) and extended sequence numbers.

This plan details the implementation of a dedicated ST2110-21 payloader and depayloader (or strict mode extensions to the existing generic RTP elements) to support hardware-accelerated H.264/H.265 encoding on embedded platforms like ARM64 (e.g., Jetson NVENC or Xilinx VCU) where SVT-JPEG-XS is not viable.

---

## 🏗️ Architecture Design

### 1. Element Design
We will introduce two new elements specifically tuned for ST2110 compliance, or extend the existing generic RTP payloaders:

**`st2110_21_payloader`**
* **Input Caps**: `video/x-h264` or `video/x-h265`
* **Output Caps**: `application/x-rtp`
* **Responsibilities**:
  * Parse NAL units (Network Abstraction Layer) from the compressed bitstream.
  * Implement strict MTU fragmentation per RFC 6184 (H.264) and RFC 7798 (H.265).
  * Apply ST2110-21 traffic shaping (Network Compatibility Model). Packets must be paced evenly across the frame interval to avoid network micro-bursts.
  * Inject 32-bit extended sequence numbers if required by the SDP.

**`st2110_21_depayloader`**
* **Input Caps**: `application/x-rtp`
* **Output Caps**: `video/x-h264` or `video/x-h265`
* **Responsibilities**:
  * Reorder out-of-order RTP packets using the extended sequence numbers.
  * Reassemble fragmented NAL units (FU-A) into complete frames.
  * Feed the reconstructed bitstream to the hardware decoder.

### 2. Traffic Shaping (The "Pacing" Engine)
The most critical part of ST2110-21 is the sender pacing. Generic RTP payloaders often blast all packets for a single video frame onto the network instantly. Under ST2110-21:
* The payloader must calculate the `T_R` (time between packets) based on the frame rate and payload size.
* We will integrate this with the `zst_scheduler_t`. Instead of a busy-wait, the payloader will attach a `pts` (presentation timestamp) and a network transmission timestamp to the buffer, allowing the network sink to pace the socket writes.

---

## 📦 Implementation Phases

### Phase 1: NAL Unit Packetization (RFC 6184 / 7798)
- [ ] Create `src/st2110_21_payloader.c` and `src/st2110_21_depayloader.c`.
- [ ] Implement Annex B byte-stream parsing (detecting `0x00 00 00 01` start codes).
- [ ] Implement H.264 Single NAL Unit packetization.
- [ ] Implement H.264 FU-A (Fragmentation Unit) packetization for large IFrames exceeding MTU (e.g., 1400 bytes).
- [ ] Implement H.265 FU packetization (RFC 7798).
- [ ] Implement generic RTP depayloader reconstruction (handling the FU-A Start/End bits).

### Phase 2: ST2110-21 Traffic Shaping
- [ ] Add `pacing-mode` property to the payloader (`enum`: `none`, `st2110-21-narrow`, `st2110-21-wide`).
- [ ] Implement a leaky bucket algorithm to calculate the transmission timestamp for each RTP packet.
- [ ] Update `net_sink.c` or the scheduler to respect fine-grained packet transmission times (using `zst_clock_get_time()` and `nanosleep()` for microsecond pacing).
- [ ] Ensure compliance with CMAX (maximum burst size) calculations.

### Phase 3: Integration & Performance Testing (CPU)
- [ ] Build test pipelines integrating built-in software encoders and decoders:
  * **H.264**: `videotestsrc ! x264enc ! st2110_21_payloader ! net_sink`
  * **H.265**: `videotestsrc ! x265enc ! st2110_21_payloader ! net_sink`
- [ ] Verify bitstream decoding on the receiving side using fakesink for pure pipeline evaluation:
  * **H.264**: `net_source ! st2110_21_depayloader ! h264dec ! fakesink`
  * **H.265**: `net_source ! st2110_21_depayloader ! h265dec ! fakesink`
- [ ] Capture network traffic with Wireshark and verify RFC 6184 / RFC 7798 structure and ST2110 RTP timestamps.
- [ ] Measure network jitter to ensure the pacing engine prevents switch buffer overflows.

---

## 🧪 Testing Strategy
1. **Unit Tests**:
   - Create `tests/test_st2110_21.c`.
   - Feed a synthetic H.264/H.265 bitstream into the payloader and verify the generated RTP packet sizes never exceed the MTU.
   - Re-feed those RTP packets into the depayloader and assert the output bitstream matches the input perfectly.
2. **Pacing Tests**:
   - Write a unit test that captures the output timestamps of generated RTP packets for a single frame and asserts that they are evenly distributed across the frame window (e.g., 16.6ms for 60fps).
3. **Integration & Performance**:
   - Run end-to-end loops entirely on CPU using `x264enc`/`x265enc` and `h264dec`/`h265dec`.
   - Use `videotestsrc` for video generation and `audiotestsrc` for supplementary audio tracks.
   - Terminate the pipeline with `fakesink` to isolate codec and network performance from display rendering bottlenecks.
   - Add explicit instrumentation to measure and report:
     * **Encoding/Decoding Performance (CPU Usage)**
     * **Frames Per Second (FPS)**
     * **Average Bit-rate (Mbps)**
     * **End-to-End Latency**
