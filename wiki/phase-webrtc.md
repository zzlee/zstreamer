# Phase: WebRTC Integration

This document outlines the analysis and implementation plan for integrating WebRTC into the `zstreamer` C11 framework.

## 1. Architectural Analysis

WebRTC is a complex protocol suite involving ICE (STUN/TURN) for NAT traversal, DTLS for secure handshakes, SRTP for secure media transport, and SCTP for data channels. It requires an external signaling mechanism (e.g., WebSocket, HTTP) to exchange Session Description Protocol (SDP) messages and ICE candidates.

### 1.1. WebRTC Backend Selection
Since `zstreamer` is a lightweight C11 framework, linking against Google's massive C++ `libwebrtc` is highly discouraged due to build complexity and binary size. Instead, we should evaluate C-friendly and lightweight alternatives:
1. **libdatachannel** (`https://github.com/paullouisageneau/libdatachannel`): Written in C++ but exposes a clean C API (`rtc/rtc.h`). Supports ICE, DTLS, SRTP, SCTP, and media/data channels. Highly modular.
2. **Amazon KVS WebRTC C SDK** (`https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c`): Pure C implementation of WebRTC. Lightweight and designed for embedded devices.

*Recommendation*: Proceed with **libdatachannel (C API)** or **KVS WebRTC C SDK** as the underlying WebRTC engine. The backend should be encapsulated so the `zstreamer` API remains backend-agnostic.

### 1.2. Element Design
In WebRTC, a single peer connection can be both a sender and a receiver of multiple media tracks and data channels.
We propose a unified `webrtc_endpoint` element (similar to GStreamer's `webrtcbin`), or explicitly separated `webrtc_sink` (sender) and `webrtc_source` (receiver) for simpler unidirectional use-cases.

For maximal flexibility, a unified **`webrtc_endpoint`** element is preferred:
- **Sink Pads (Inputs)**: Dynamically requested (e.g., `sink_%u`). Takes encoded buffers (e.g., H.264 NALUs, Opus packets). The element wraps these in RTP/SRTP and sends them over the WebRTC transport.
- **Source Pads (Outputs)**: Dynamically created when remote tracks are received. Outputs encoded buffers (depayloaded from RTP) with proper Caps to allow downstream decoding.

### 1.3. Signaling Interface
WebRTC requires passing SDPs and ICE candidates between the element and the user application (which talks to a signaling server).
We will use:
- **Element specific API** (in `include/zstreamer/elements/webrtc.h`) to push external signaling data into the element:
  - `zst_webrtc_set_remote_description(element, type, sdp)`
  - `zst_webrtc_add_ice_candidate(element, mid, mlineindex, candidate)`
- **Event Bus / Callbacks** to emit local signaling data to the application:
  - `ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION` (Application should send this SDP to the remote peer)
  - `ZST_EVENT_WEBRTC_ICE_CANDIDATE` (Application should send this ICE candidate to the remote peer)
  - Alternatively, register callbacks explicitly on the element: `zst_webrtc_set_on_ice_candidate_callback(...)`

### 1.4. Media Pipeline & Caps
WebRTC transports media over RTP.
- **Outbound**: The pipeline will look like `v4l2_source -> x264_encoder -> webrtc_endpoint`. The `webrtc_endpoint` will internally handle RTP packetization (or optionally require an explicit `rtppay` element before it).
- **Inbound**: `webrtc_endpoint -> h264_decoder -> glsink`. The element will dynamically create a source pad upon receiving a new track, negotiate Caps (e.g., `video/x-h264`), and output depacketized frames.

---

## 2. Implementation Plan

The implementation is broken down into small, verifiable phases.

### Phase 1: WebRTC Library Integration & Scaffolding
- [ ] Evaluate and select the WebRTC C library (e.g., libdatachannel or KVS WebRTC C SDK).
- [ ] Add the selected library to the CMake build system (as an optional dependency, e.g., `-DENABLE_WEBRTC=ON`).
- [ ] Create the boilerplate for the `webrtc_endpoint` element in `src/webrtc_endpoint.c` and register it in `zst_builtins.c`.
- [ ] Create `include/zstreamer/elements/webrtc.h` defining the public API for the element (e.g., configuration structures for STUN/TURN servers).

### Phase 2: Signaling Setup & Peer Connection Lifecycle
- [ ] Implement PeerConnection initialization within the element's `open` or `start` state transition.
- [ ] Implement public API to inject remote SDP offers/answers (`zst_webrtc_set_remote_description`).
- [ ] Implement public API to inject remote ICE candidates (`zst_webrtc_add_ice_candidate`).
- [ ] Implement callbacks or events to surface local SDPs and local ICE candidates to the user application.
- [ ] *Verification*: Write a standalone C test (`tests/test_webrtc_signaling.c`) that negotiates a loopback WebRTC connection between two `webrtc_endpoint` instances without media.

### Phase 3: Media Sender (Outbound Tracks)
- [ ] Implement dynamic sink pad creation (e.g., when the application links to the element).
- [ ] Implement Caps negotiation on sink pads to identify the codec (e.g., H.264, VP8).
- [ ] Intercept buffers arriving at the sink pad's `process` function, packetize them into RTP (or leverage existing `rtppay` logic), and push them to the WebRTC backend.
- [ ] *Verification*: Create a test pipeline sending a synthetic video stream (`video_test_src -> x264_encoder -> webrtc_endpoint`), verify that RTP packets are flowing into the WebRTC stack.

### Phase 4: Media Receiver (Inbound Tracks)
- [ ] Implement callbacks in the WebRTC backend for when a new remote track is added.
- [ ] Dynamically create a source pad (`src_%u`), generate the appropriate Caps based on the track's payload type (e.g., H.264), and emit a `ZST_EVENT_PAD_ADDED` event.
- [ ] Depacketize incoming RTP media from the WebRTC backend and push `zst_buffer_t` frames out of the source pad.
- [ ] *Verification*: Write a loopback test (`test_webrtc_loopback.c`) where instance A sends video to instance B, and instance B decodes and sinks it to a `fake_sink`.

### Phase 5: Data Channels
- [ ] Add an API to create a WebRTC data channel (`zst_webrtc_create_data_channel(element, label)`).
- [ ] Provide mechanism to send and receive raw byte buffers over the data channel (could be handled via dedicated pads for data, or via a direct callback API on the element).
- [ ] *Verification*: Test bidirectional text string passing over a data channel between two endpoints.

### Phase 6: RTCP, QoS, and Advanced Features
- [ ] Handle incoming RTCP PLI (Picture Loss Indication) by forwarding a generic "force keyframe" event upstream to the video encoder (e.g., `x264_encoder`).
- [ ] Forward generic bandwidth estimation / bit-rate adaptation events upstream.
- [ ] Implement NACKs and retransmissions if supported by the backend.
- [ ] Document the WebRTC setup with examples.

---

## 3. Future Considerations
- Supporting GStreamer-style `webrtcbin` unified design where a single element manages everything vs `webrtcsink`/`webrtcsrc`. A unified endpoint is initially harder but more powerful for bidirectional calls.
- Browser compatibility: Ensure standard H.264 payload types and Opus are correctly negotiated with Chrome/Firefox.
