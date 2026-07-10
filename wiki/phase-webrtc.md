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

### Phase 1: WebRTC Library Integration & Scaffolding ✅
- [x] Evaluate and select the WebRTC C library → **libdatachannel** (C API via `rtc/rtc.h`).
- [x] Add the selected library to the CMake build system as `-DENABLE_WEBRTC=ON` (optional dependency, `find_package` + `pkg-config` fallback).
- [x] Create the boilerplate for the `webrtc_endpoint` element in `src/webrtc_endpoint.c` and register it in `zst_builtins.c`.
- [x] Create `include/zstreamer/elements/zst_webrtc_endpoint.h` defining the public API for the element.
- [x] Add libdatachannel build-from-source to `Dockerfile` (git, libssl-dev, cmake build).
- [x] Plugin build target `zst_webrtc` added to CMakeLists.txt.

### Phase 2: Signaling Setup & Peer Connection Lifecycle ✅
- [x] Implement PeerConnection initialization within the element's `open` state transition.
- [x] Implement public API to inject remote SDP offers/answers (`zst_webrtc_set_remote_description`).
- [x] Implement public API to inject remote ICE candidates (`zst_webrtc_add_ice_candidate`).
- [x] Implement callbacks or events to surface local SDPs and local ICE candidates to the user application.
- [x] *Verification*: Write a standalone C test (`tests/test_webrtc_signaling.c`) that negotiates a loopback WebRTC connection between two `webrtc_endpoint` instances without media.

### Phase 3: Media Sender (Outbound Tracks) ✅
- [x] Implement video/audio track creation API (`zst_webrtc_add_video_track`, `zst_webrtc_add_audio_track`).
- [x] Set up codec-specific packetizers (H264, Opus) via libdatachannel `rtcSet*Packetizer`.
- [x] Intercept buffers arriving at the element's `process` function and forward to the WebRTC backend via `rtcSendMessage`.
- [x] *Verification*: `test_webrtc_media_send.c` — creates H264 track, exchanges SDP with track, sends encoded frames. Track appears in SDP offer. ICE connectivity verified.

### Phase 4: Media Receiver (Inbound Tracks) ✅
- [x] Implement `on_track` callback to detect incoming tracks and their codec (via SDP parsing).
- [x] Dynamically create source pads (`src_%u`) for each received track with proper codec detection.
- [x] Set up `rtcSetFrameCallback` to receive decoded frames and push them via `zst_pad_push()`.
- [x] Fire `ZST_EVENT_PAD_ADDED` event and user-registered callback for incoming tracks.
- [x] *Verification*: `test_webrtc_media_recv.c` — sender adds H264 track, receiver fires on_track, creates source pad, all verified.
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
