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
- [x] *Verification*: Write a loopback test (`test_webrtc_loopback.c`) where instance A sends video to instance B, and instance B decodes and sinks it to a `fake_sink`.

### Phase 5: Data Channels ✅
- [x] Add an API to create a WebRTC data channel (`zst_webrtc_create_data_channel(element, label)`).
- [x] Implement `on_data_channel` callback to handle incoming channels from remote peer.
- [x] Add `on_dc_open`, `on_dc_closed`, `on_dc_message` callbacks for channel lifecycle and message delivery.
- [x] Add `zst_webrtc_send_data(el, channel_id, data, size)` for sending messages.
- [x] Add `zst_webrtc_set_on_data_message_callback(el, fn, user_data)` for receiving messages.
- [x] *Verification*: `test_webrtc_data_channel.c` — creates channel, exchanges SDP, send API verified. ICE needed for full loopback.

### Phase 6: RTCP, QoS, and Advanced Features ✅
- [x] Chain `rtcChainPliHandler` and `rtcChainRembHandler` on outbound tracks for RTCP QoS.
- [x] Chain `rtcChainRtcpReceivingSession`, `rtcChainRtcpSrReporter`, `rtcChainRtcpNackResponder` for full RTCP support.
- [x] Post `ZST_EVENT_WEBRTC_PLI` and `ZST_EVENT_WEBRTC_REMB` events on the bus for upstream elements.
- [x] Implement `zst_webrtc_request_keyframe(el, track_index)` to send PLI to remote.
- [x] Implement `zst_webrtc_request_bitrate(el, track_index, bitrate)` to send REMB to remote.
- [x] *Verification*: `test_webrtc_rtcp.c` — RTCP handler chaining verified, request APIs tested, error handling confirmed.

---

## 3. Transport-Wide Congestion Control (TWCC) Survey

### 3.1. Background

WebRTC congestion control has evolved through two generations:

| Mechanism | Era | Estimation Side | Feedback | Bandwidth Estimate |
|-----------|-----|-----------------|----------|--------------------|
| **REMB** (RFC 5506) | Legacy | Receiver | RTCP REMB packet | Receiver-side estimate sent to sender |
| **TWCC/CCFB** (RFC 8888) | Modern | Sender | RTCP transport-cc feedback | Sender-side estimation using per-packet arrival times |

**REMB** (as implemented in Phase 6) tells the sender "I can receive X bps" — the receiver measures and the sender obeys. This is simple but has known problems: slow adaptation, no per-packet granularity, and the receiver lacks knowledge of network conditions between sender and receiver.

**TWCC** (Transport-Wide Congestion Control) flips the model: every outgoing RTP packet gets a transport-wide sequence number, and the receiver reports back the exact arrival time of each packet. The sender then runs the **GCC (Google Congestion Control)** algorithm locally, using both a **delay-based estimator** (inter-packet delay trends) and a **loss-based estimator** (packet loss rate) to compute the available bandwidth.

### 3.2. How TWCC Works

#### 3.2.1. RTP Header Extension
Each outgoing RTP packet carries a 16-bit transport-wide sequence number via the `transport-wide-cc-02` header extension:

```
 0                   1                   2
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  ID   | L=1   |transport-wide sequence number |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

This sequence number is **shared across all tracks** on the same PeerConnection (unlike per-stream RTP sequence numbers). The sender maintains a single counter that increments for every outgoing packet.

#### 3.2.2. RTCP Feedback (RFC 8888 CCFB)
The receiver periodically sends an RTCP Congestion Control Feedback (CCFB) packet containing:
- A base sequence number (first packet in the feedback window)
- A list of packet statuses: received, lost, or duplicate
- Optional arrival timestamps (if timing info was requested)
- Per-packet delta times between consecutive arrivals

The feedback format is compact: statuses are bit-packed (2 bits per packet: 0=not received, 1=received/timing only, 2=received with delta, 3=received with both delta and timing).

#### 3.2.3. GCC Algorithm (Sender-Side)
The sender runs two parallel estimators:

**Delay-Based Estimator (AIMD):**
1. For each feedback packet, compute inter-packet delay deltas: `Δdelay(i) = (arrival(i) - arrival(i-1)) - (send(i) - send(i-1))`
2. Positive delays = queuing (congestion building); negative delays = queuing draining
3. Use an exponential moving average with a threshold to detect "overuse" (delay increasing) vs "underuse" (delay decreasing)
4. On overuse: decrease rate by factor β (e.g., 0.85)
5. On underuse/normal: increase rate additively (additive increase)
6. This is the classic AIMD (Additive Increase Multiplicative Decrease) pattern

**Loss-Based Estimator:**
1. Calculate packet loss fraction from the feedback
2. If loss > threshold (e.g., 2% or 10%): decrease target bitrate
3. If loss < threshold: allow increase
4. Combined with delay-based estimate using `min(delay_estimate, loss_estimate)`

#### 3.2.4. Advantages over REMB
- **Per-packet granularity**: TWCC reports arrival times for every packet, not just an aggregate estimate
- **Sender-side estimation**: The sender has full knowledge of send times and can compute network delay directly
- **Faster adaptation**: Reacts to congestion in ~100ms vs seconds for REMB
- **Better interop**: Standard in modern browsers (Chrome, Firefox, Safari all support TWCC)
- **No receiver capability estimation**: The receiver doesn't need to estimate bandwidth (which it often can't do accurately)

### 3.3. libdatachannel Status

**libdatachannel does NOT provide built-in TWCC or GCC support.** What it offers:
- `rtcChainPliHandler` / `rtcChainRembHandler` — REMB-based QoS only
- No `transport-wide-cc-02` header extension support
- No CCFB (RFC 8888) feedback parsing
- No sender-side GCC implementation

This means a full TWCC implementation must be built on top of libdatachannel at the `zstreamer` layer.

### 3.4. Implementation Strategy for zstreamer

Since libdatachannel handles the SRTP/SRTCP layer but doesn't expose TWCC, we have two options:

**Option A: Custom RTCP Interceptor Layer**
- Intercept outgoing RTP packets to add the `transport-wide-cc-02` header extension
- Parse incoming RTCP CCFB packets to extract per-packet arrival times
- Implement GCC (delay-based + loss-based) as a standalone C module
- Use `rtcSetRemoteDescription` / SDP negotiation to advertise TWCC support

**Option B: Leverage libdatachannel's Extension Points**
- libdatachannel's `rtcPacketizerInit` supports some customization but NOT header extensions
- Would still need custom RTCP parsing outside libdatachannel
- Less clean than Option A

**Recommendation: Option A** — Implement TWCC as a self-contained module (`zst_webrtc_twcc.c`) that:
1. Adds the transport-cc header extension to outgoing packets (intercepted before SRTP encryption)
2. Parses CCFB feedback from incoming RTCP
3. Runs the GCC algorithm to compute target bitrate
4. Feeds the target bitrate back to the encoder via `ZST_EVENT_BITRATE_CHANGED`

### 3.5. Proposed Module Architecture

```
zst_webrtc_twcc.c
├── twcc_sender()
│   ├── Assign transport-wide sequence numbers to outgoing packets
│   └── Add RTP header extension (transport-wide-cc-02)
├── twcc_receiver_feedback()
│   ├── Parse RTCP CCFB packets (RFC 8888)
│   ├── Extract per-packet arrival times and deltas
│   └── Feed into GCC estimators
├── twcc_estimator()
│   ├── Delay-based estimator (AIMD with overuse detector)
│   ├── Loss-based estimator (threshold-based)
│   └── Combined target bitrate = min(delay_est, loss_est)
└── twcc_get_target_bitrate()
    └── Returns current estimated bitrate for encoder adaptation
```

### 3.6. Open Challenges

1. **SRTP Interception**: libdatachannel encrypts RTP before it reaches our code. Adding the header extension requires intercepting packets BEFORE SRTP encryption. This may require a custom SRTP session or a proxy layer.
2. **RTCP Decryption**: Incoming RTCP CCFB packets are SRTCP-encrypted. We need to intercept them before decryption, or ensure libdatachannel exposes decrypted RTCP feedback.
3. **SDP Negotiation**: The `transport-wide-cc-02` extension must be negotiated in SDP (both offer and answer must agree). libdatachannel doesn't expose SDP modification hooks.
4. **GCC Complexity**: A full GCC implementation is ~2000-3000 lines of C. The delay-based estimator alone requires careful tuning of filter coefficients, thresholds, and rate change logic.
5. **Interop Testing**: Must work with Chrome/Firefox which use TWCC by default. Need to verify SDP negotiation and feedback format compatibility.

---

## 4. Implementation Plan (Continued)

### Phase 7: VP8/VP9 Codec Support ✅
- [x] **VP8 packetizer**: Implement RTP packetization for VP8 using libdatachannel's `rtcSetVP8Packetizer()`. VP8 uses a simpler payload format than H264 (no NAL unit fragmentation — single VP8 frame per RTP packet with VP8 payload descriptor header).
- [x] **VP9 packetizer**: Implement RTP packetization for VP9 using `rtcSetVP9Packetizer()`. VP9 has flexible partitioning; the packetizer should handle `VP9 payload descriptor` with picture ID, TL0PICIDX, and flexible mode bits.
- [x] **VP8/VP9 decoder elements**: Implement `vp8_decoder` and `vp9_decoder` elements using FFmpeg's `libavcodec` (`avcodec_find_decoder(AV_CODEC_ID_VP8/VP9)`). Follow the same pattern as `h264_decoder.c`.
- [x] **VP8/VP9 encoder elements**: Implement `vp8_encoder` and `vp9_encoder` elements using FFmpeg's `libavcodec` (`avcodec_find_encoder(AV_CODEC_ID_VP8/VP9)`). VP8 uses `libvpx`, VP9 uses `libvpx-vp9`.
- [x] **Codec auto-detection in `on_track`**: Extend `codec_from_track_sdp()` to reliably detect VP8/VP9 from SDP `a=rtpmap` lines (already partially implemented). Ensure the codec maps to the correct packetizer and decoder.
- [x] **SDP codec negotiation**: When creating offers, advertise VP8 and VP9 alongside H264. Use `a=rtpmap:96 VP8/90000` and `a=rtpmap:97 VP9/90000` in the SDP. Ensure `a=fmtp` lines include VP8/VP9-specific parameters.
- [x] **Fallback logic**: If the remote peer only supports VP8 (no H264), automatically select VP8. If the remote only supports H264, select H264. Prefer H264 when both are available (better hardware support).
- [x] *Verification*: `test_webrtc_vp8vp9.c` — element creation tests, property tests, codec enum tests, SDP detection tests. All 8 tests pass.

### Phase 8: Chrome/Firefox Browser Interoperability 📝

This phase is broken into sub-phases. **8a–8c are critical** — without them, Chrome cannot display zstreamer video. 8d–8h are important for a polished demo.

#### 8a. Multi-Track Pad Routing (Critical) ✅

**Problem**: The current `webrtc_endpoint` has a single static sink pad. `webrtc_process()` sends ALL incoming buffers to the FIRST active track. This means:
- `videotestsrc → webrtc_endpoint` works (video → video track).
- `audiotestsrc → webrtc_endpoint` sends audio to the video track (broken).
- Cannot send both audio AND video simultaneously.

**Fix**:
- [x] Add dynamic sink pads when tracks are created. When `zst_webrtc_add_video_track()` is called, create a `sink_video_%u` pad. When `zst_webrtc_add_audio_track()` is called, create a `sink_audio_%u` pad. Keep the original `sink` pad as a fallback for single-track use.
- [x] Route buffers in `webrtc_process()` by checking buffer content type. Video buffers (`video/x-h264`, `video/x-vp8`, etc.) go to the first active video track. Audio buffers (`audio/opus`, `audio/aac`, etc.) go to the first active audio track.
- [x] Update sink pad caps to reflect the specific track type (e.g., `sink_video_0` accepts `video/x-h264;video/x-vp8;video/x-vp9`).
- [x] Fire `ZST_EVENT_PAD_ADDED` when dynamic sink pads are created.
- [x] *Verification*: `test_webrtc_multitrack.c` — create both video and audio tracks, verify separate sink pads exist, verify buffers route to correct tracks.

#### 8b. TWCC SDP Filtering (Critical)

**Problem**: Chrome offers `a=extmap:3 urn:ietf:params:rtp-hdrext:transport-wide-cc-02` in its SDP. If zstreamer includes this in its answer, Chrome sends TWCC feedback packets that zstreamer ignores → **video freezes after ~2 seconds**.

**Fix**:
- [x] After receiving a remote SDP offer (in `zst_webrtc_set_remote_description()`), scan for `a=extmap:3` or `transport-wide-cc-02` and remove those lines before passing to libdatachannel.
- [x] Also filter other unsupported extensions (`a=extmap:4`, `a=extmap:5`, etc.) to prevent similar issues.
- [x] Log filtered extensions for debugging.
- [x] *Verification*: `test_webrtc_sdpclean.c` — pass Chrome's real SDP through the filter, verify TWCC lines removed, verify other extensions preserved.

#### 8c. WebSocket Signaling Server (Critical)

**Problem**: No way for Chrome to exchange SDP/ICE with zstreamer.

**Implementation**:
- [x] Create `src/zst_ws_server.c` — lightweight POSIX WebSocket server (~250 lines). Support:
  - RFC 6455 framing (text frames for JSON signaling).
  - Multiple concurrent clients (poll-based, no threads).
  - Callbacks: `on_connect`, `on_message`, `on_disconnect`.
  - Send function: `zst_ws_send(client_id, data, len)`.
- [x] Create `include/zst_ws_server.h` — public API:
  ```c
  zst_ws_server_t* zst_ws_server_create(int port);
  zst_result_t     zst_ws_server_start(zst_ws_server_t* srv);
  zst_result_t     zst_ws_server_stop(zst_ws_server_t* srv);
  zst_result_t     zst_ws_send(zst_ws_server_t* srv, int client_id, const char* data, size_t len);
  void             zst_ws_server_set_callbacks(zst_ws_server_t* srv,
      void (*on_connect)(int client_id, void* user_data),
      void (*on_message)(int client_id, const char* msg, size_t len, void* user_data),
      void (*on_disconnect)(int client_id, void* user_data),
      void* user_data);
  ```
- [x] Register WebSocket server elements in `zst_builtins.c` (`ws_server`).
- [x] *Verification*: `test_ws_server.c` — start server, connect with a client (or `websocat`), send/receive messages.

#### 8d. SDP Compatibility Layer

**Problem**: Chrome/Firefox SDPs contain extensions, bundles, and attributes that libdatachannel may not handle correctly.

**Fix**:
- [x] Post-process the answer SDP before sending to Chrome:
  - Add `a=group:BUNDLE video audio` when multiple media sections are present.
  - Ensure `a=msid` attributes are present for stream identification.
  - Verify `a=rtcp-mux` is always offered (required by modern browsers).
  - Add `a=ssrc` attributes with consistent `cname` across all tracks.
- [x] Parse Chrome/Firefox offer SDPs to extract supported codecs. Handle `a=extmap`, `a=rtcp-rsize`, `a=compound`, etc.
- [x] *Verification*: Compare SDP output with a working GStreamer `webrtcbin` example and verify clean parsing in `test_webrtc_sdpclean.c`.

#### 8e. ICE Restart Support

**Problem**: Chrome may request ICE restart if the connection fails. No API exists for this.

**Fix**:
- [x] Implement `zst_webrtc_restart_ice(el)` — generates a new offer with fresh ICE credentials (`ice-ufrag`, `ice-pwd`).
- [x] Handle `a=ice-lite` vs `a=ice-mismatch` in remote SDP.
- [x] *Verification*: `test_webrtc_restart.c` — simulate ICE failure, call restart, verify new connection.

#### 8f. Receiver-Side Codec Selection

**Problem**: When Chrome offers multiple codecs (VP8+H264), zstreamer should pick the best one it supports. Currently libdatachannel auto-answers, which may not pick optimally.

**Fix**:
- [x] When receiving a remote SDP offer, parse all offered codecs.
- [x] Select the best codec based on preference: H264 > VP8 > VP9 > Opus.
- [x] Modify the auto-generated answer to include only the selected codec(s).
- [x] Post the selected codec info via event or property.
- [x] *Verification*: `test_webrtc_codec_select.c` — Chrome offers VP8+H264, zstreamer selects H264.

#### 8g. Chrome Test Page & Demo Server

- [x] Create `examples/webrtc_chrome/index.html` — self-contained HTML page (~80 lines) that:
  - Connects to zstreamer via WebSocket.
  - Sends `recvonly` offer for video + audio.
  - Displays received video in `<video>` element.
  - Shows connection state and logs.
- [x] Create `examples/webrtc_chrome/server.c` — zstreamer app that:
  - Runs `videotestsrc → x264enc → webrtc_endpoint` pipeline.
  - Serves `index.html` via embedded HTTP server.
  - Relays SDP/ICE between browser and `webrtc_endpoint` via WebSocket.
- [x] Add `examples/webrtc_chrome/Makefile` or CMake target.

#### 8h. STUN/TURN Configuration & Documentation

- [x] Verify `stun-servers` and `turn-servers` properties are passed correctly to `rtcConfiguration`.
- [x] Add `turn-username` and `turn-password` properties for TURN authentication.
- [x] Document how to configure a TURN server (e.g., `coturn`) for production use behind symmetric NAT.
- [x] Add `--stun` and `--turn` CLI flags to the demo server.

#### Chrome Test Procedure

1. **Start zstreamer server**:
   ```bash
   ./webrtc_chrome_server --port 8080 --http-port 8000 --stun stun.l.google.com:19302
   ```
2. **Open Chrome** and navigate to `http://localhost:8000/index.html`.
3. **Open `chrome://webrtc-internals`** in a second tab (this shows all connection details).
4. **Click "Connect"** on the test page.
5. **Verify**:
   - Video appears in the `<video>` element (live video from `videotestsrc`).
   - `chrome://webrtc-internals` shows ICE state = `connected` or `completed`.
   - RTP statistics show packets sent/received increasing.
   - DTLS state = `connected`.
   - Negotiated codec = `H264` (or `VP8` if Phase 7 is complete).
   - No TWCC feedback packets in `chrome://webrtc-internals` (filtered).
6. **Test disconnect**: Click "Disconnect", verify clean teardown.
7. **Test reconnect**: Click "Connect" again, verify new connection succeeds.
8. **Test multi-track**: Add both video and audio tracks, verify both arrive in Chrome.

#### Chrome Test Page (index.html)

```html
<!DOCTYPE html>
<html>
<head>
    <title>zstreamer WebRTC Test</title>
    <style>
        video { width: 640px; height: 480px; background: #000; }
        #log { font-family: monospace; white-space: pre; height: 200px;
               overflow-y: auto; background: #f5f5f5; padding: 10px; }
        button { padding: 10px 20px; font-size: 16px; margin: 5px; }
    </style>
</head>
<body>
    <h1>zstreamer ↔ Chrome WebRTC Test</h1>
    <video id="video" autoplay playsinline></video>
    <br>
    <button onclick="start()">Connect</button>
    <button onclick="stop()">Disconnect</button>
    <div id="log"></div>
    <script>
    let pc, ws;
    function log(msg) {
        document.getElementById('log').textContent +=
            new Date().toLocaleTimeString() + ' ' + msg + '\n';
    }
    async function start() {
        ws = new WebSocket('ws://localhost:8080');
        ws.onopen = async () => {
            log('Connected to signaling server');
            pc = new RTCPeerConnection({
                iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
            });
            pc.ontrack = (e) => {
                log('Got remote track: ' + e.track.kind);
                document.getElementById('video').srcObject = e.streams[0];
            };
            pc.onicecandidate = (e) => {
                if (e.candidate) {
                    ws.send(JSON.stringify({
                        type: 'candidate', candidate: e.candidate
                    }));
                }
            };
            pc.oniceconnectionstatechange = () => {
                log('ICE state: ' + pc.iceConnectionState);
            };
            pc.addTransceiver('video', { direction: 'recvonly' });
            pc.addTransceiver('audio', { direction: 'recvonly' });
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            log('Created offer, sending to zstreamer');
            ws.send(JSON.stringify({
                type: 'offer', sdp: pc.localDescription.sdp
            }));
        };
        ws.onmessage = async (msg) => {
            const data = JSON.parse(msg.data);
            log('Received: ' + data.type);
            if (data.type === 'answer') {
                await pc.setRemoteDescription({
                    type: 'answer', sdp: data.sdp
                });
            } else if (data.type === 'candidate') {
                await pc.addIceCandidate(data.candidate);
            }
        };
        ws.onerror = () => log('WebSocket error');
        ws.onclose = () => log('WebSocket closed');
    }
    function stop() {
        if (pc) pc.close();
        if (ws) ws.close();
        log('Disconnected');
    }
    </script>
</body>
</html>
```

#### Debugging with chrome://webrtc-internals

Open `chrome://webrtc-internals` while connected. Key fields to check:

| Field | Expected | Problem if Wrong |
|-------|----------|------------------|
| ICE connection state | `connected` / `completed` | NAT traversal failed — need TURN |
| DTLS state | `connected` | Certificate mismatch or ICE failed |
| Negotiated codecs | `H264` or `VP8` | SDP negotiation failed |
| Packets sent/received | Increasing numbers | Media not flowing |
| Available incoming bitrate | > 0 | TWCC feedback not working |
| Timestamps (ms) | Monotonically increasing | Clock sync issue |
| TWCC feedback packets | None (filtered) | Video will freeze after ~2s |
| BUNDLE group | `video audio` | Multi-track negotiation failed |

### Phase 9: Transport-Wide Congestion Control (TWCC) ✅

**Implementation**: `src/zst_webrtc_twcc.c` + `include/zstreamer/elements/zst_webrtc_twcc.h`
**Patch**: `scripts/libdatachannel-twcc.patch` (exposes `rtcSetTrackInterceptorCallback` / `rtcSetMediaInterceptorCallback` in the C API)
**Verification**: `tests/test_webrtc_twcc.c` — 6 tests, all pass

- [x] **Research & SRTP interception**: Used **Option B (libdatachannel patch)**. Applied `libdatachannel-twcc.patch` during Docker build to expose two new C API hooks:
  - `rtcSetTrackInterceptorCallback` — intercepts outgoing RTP per-track *before* SRTP encryption
  - `rtcSetMediaInterceptorCallback` — intercepts incoming RTCP *after* SRTCP decryption
- [x] **Transport-wide sequence number allocator**: Monotonic 16-bit counter (`twcc->seq_num`) shared across all tracks on a PeerConnection. Protected by `pthread_mutex`. Sent packet history stored in a ring buffer (`TWCC_HISTORY_SIZE=8192`) indexed by `seq % size`, recording `send_time_us` and `packet_size` per packet.
- [x] **RTP header extension injection**: When `extmap_id >= 0`, outgoing RTP packets get a one-byte-header `0xBEDE` extension block appended:
  ```
  0xBE 0xDE 0x00 0x01        ← magic + 1 word
  (id<<4)|0x01  seq_hi seq_lo 0x00  ← TWCC ext + padding
  ```
  Packet is re-created via `rtcCreateOpaqueMessage()`. Packets with pre-existing extensions are passed through unchanged (no-op, logged as warning).
- [x] **RTCP CCFB parser**: Implements RFC 8888 Transport-Wide Feedback (PT=205, FMT=15) parsing in `twcc_parse_ccfb()`:
  - Decodes Run-Length Encoding chunks (`chunk[15]=0`) and 1-bit/2-bit Status Vector chunks (`chunk[15]=1`)
  - Extracts packet statuses (0=lost, 1=recv-small-delta, 2=recv-large-delta)
  - Reconstructs per-packet receiver-side arrival times from reference time + cumulative 250µs deltas
  - Computes Δdelay = `(arrival_i − arrival_{i-1}) − (send_i − send_{i-1})` using history ring buffer
- [x] **Delay-based estimator (AIMD)**: Implemented in `gcc_delay_update()` + `gcc_delay_adapt()`:
  - Exponential moving average of delay gradient: `gradient = γ·gradient + (1−γ)·Δdelay_ms` (γ=0.9)
  - Adaptive threshold that tracks up fast and decays slowly (clamped 6ms–600ms)
  - State machine: **OVERUSE** (gradient > threshold) → multiplicative decrease β=0.85 / **NORMAL** → additive increase α=8% / **UNDERUSE** → hold
- [x] **Loss-based estimator**: Implemented in `gcc_loss_adapt()`:
  - Smoothed loss rate: `loss = 0.5·loss + 0.5·new_loss`
  - >10% loss → −20%; 2–10% loss → hold; <2% loss → +5%
- [x] **Combined GCC algorithm**: `current_bitrate = min(delay_estimate, loss_estimate)`. Updated every 100ms. `ZST_EVENT_WEBRTC_REMB` posted only when estimate changes by >3% (hysteresis to avoid event spam). Bitrate clamped 100kbps–8Mbps.
- [x] **Encoder adaptation integration**: `ZST_EVENT_WEBRTC_REMB` events are posted on the pipeline bus. The demo server (`examples/webrtc_chrome/server.c`) receives these in its bus thread and calls `zst_element_set_property(g_video_encoder, "bitrate", br_str)` to dynamically update the encoder. Verified live with Chrome.
- [x] **SDP negotiation**: `zst_webrtc_twcc_parse_offer()` scans the Chrome offer SDP for `a=extmap:N http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01` and extracts the extension ID. `zst_webrtc_twcc_inject_answer()` injects the matching `a=extmap` line into the answer SDP after each `c=IN` line.
- [x] **Fallback to REMB**: If the offer SDP does not contain the TWCC extension URI, `extmap_id` is set to -1 and outgoing packet interception is disabled (no extension injected). libdatachannel's native `on_rtcp_remb` callback (Phase 6) continues to handle REMB-based rate control as a fallback.
- [x] *Verification*: `tests/test_webrtc_twcc.c` — 6 unit tests:
  1. `twcc_create_destroy` — lifecycle (no crash/leak)
  2. `twcc_extmap_parse` — Chrome SDP extmap ID extracted correctly (ID=3)
  3. `twcc_inject_answer` — extmap line injected into answer SDP
  4. `twcc_seq_numbering` — 10 outgoing packets stamped without crash
  5. `twcc_ccfb_loss_decrease` — 50% synthetic loss drives bitrate down from 2 Mbps
  6. `twcc_ccfb_no_loss_increase` — zero loss drives bitrate up above 2 Mbps

#### Live Verification (Chrome)

With `webrtc_chrome_server` running (Docker container `webrtc-server`) and Chrome connected, the server log confirms end-to-end operation:

```
INFO  [webrtc_endpoint] on_local_description: Injected TWCC into answer
INFO  [twcc] GCC: delay_est=2160000 loss_est=2100000 combined=2100000 bps [loss=0.0% delay_grad=0.00 state=NORMAL]
INFO  [bus_thread] Received TWCC REMB estimate: 2100000 bps
INFO  [bus_thread] Updated video encoder bitrate to 2100000 bps
```

Both TWCC CCFB (our GCC) and native libdatachannel REMB run simultaneously, both feeding into the same bitrate update path.


### Phase 10: Documentation and Examples ✅
- [x] Document the WebRTC setup with examples (`wiki/webrtc-guide.md`).
- [x] Create a complete bidirectional video call example pipeline.
- [x] Add TWCC architecture documentation with diagrams (`wiki/twcc-architecture.md`).
- [x] Document Chrome/Firefox browser interop with code examples.
- [x] Create a `webrtc_sender.c` example: `videotestsrc → x264enc → webrtc_endpoint` sending to a browser.
- [x] Create a `webrtc_receiver.c` example: `webrtc_endpoint → h264dec → glsink` receiving from a browser.

---

## 4.5. Production TURN (coturn) Setup & Integration (Phase 8h)

WebRTC relies on ICE (Interactive Connectivity Establishment) to establish peer-to-peer media paths. In production environments where one or both peers are located behind symmetric NATs or restrictive firewalls, direct connections or STUN-assisted connections (hole punching) will fail. In these scenarios, a TURN (Traversal Using Relays around NAT) server is required to relay media traffic.

### 1. Configuring `coturn` TURN Server
For production use, `coturn` is the recommended open-source TURN server.

#### Installation
On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y coturn
```

#### Minimal Production Configuration (`/etc/turnserver.conf`)
Create or edit the configuration file with the following settings:
```ini
# Ports
listening-port=3478
tls-listening-port=5349

# Network interfaces (replace with server's public IP)
external-ip=YOUR_SERVER_PUBLIC_IP
listening-ip=0.0.0.0

# Security and authentication
fingerprint
lt-cred-mech
realm=zstreamer.org

# Define dynamic or static users (format: username:password)
user=zst-user:super-secure-password-123

# Logging
log-file=/var/log/turnserver/turnserver.log
simple-log
```

#### Starting the service
```bash
sudo systemctl enable turnserver
sudo systemctl start turnserver
```

### 2. Using TURN in `zstreamer`

To configure a `webrtc_endpoint` instance to authenticate and relay traffic via your TURN server:

#### A. Command Line Interface (CLI) flags on Demo Server
Start the demo server with the STUN and authenticated TURN server properties:
```bash
./webrtc_chrome_server \
    --stun stun:stun.l.google.com:19302 \
    --turn turn:YOUR_SERVER_PUBLIC_IP:3478 \
    --turn-user zst-user \
    --turn-pass super-secure-password-123
```

#### B. Programmatic configuration (C API)
Set the properties directly on the element prior to transitioning to `READY` state:
```c
// Define the TURN servers and authentication credentials
zst_element_set_property(webrtc_el, "turn-servers", "turn:YOUR_SERVER_PUBLIC_IP:3478");
zst_element_set_property(webrtc_el, "turn-username", "zst-user");
zst_element_set_property(webrtc_el, "turn-password", "super-secure-password-123");
```

---

## 5. Future Considerations
- **AV1 codec support**: AV1 is the next-generation codec with superior compression. libdatachannel has `rtcSetAV1Packetizer`. Add `av1_encoder`/`av1_decoder` elements using FFmpeg's `libsvtav1` or `libaom`.
- **Simulcast**: Send multiple resolution/quality layers simultaneously. Chrome supports receiving simulcast via `a=rid` and `a=simulcast` SDP attributes. Requires multiple `rtcSendMessage` calls per frame with different SSRCs.
- **Scalable Video Coding (SVC)**: VP9 and AV1 support temporal/spatial scalability. Chrome can negotiate SVC with `a=scalability-mode` in SDP. Requires SVC-aware encoder elements.
- **Separate `webrtcsink` / `webrtcsrc` elements**: For simpler unidirectional pipelines (e.g., `v4l2src → h264enc → webrtcsink`), dedicated elements could hide the full PeerConnection complexity. The current unified `webrtc_endpoint` handles both directions but is more complex for simple use cases.
- **Full multi-stream routing**: Phase 8a handles basic audio/video routing by caps. Advanced multi-stream (multiple independent video streams with separate SSRCs and MID-based routing, per-stream bandwidth allocation) is a future enhancement.
