# WebRTC Guide — zstreamer

This guide covers everything needed to stream media from a `zstreamer` pipeline to Chrome/Firefox and receive media from a browser.

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Architecture Overview](#2-architecture-overview)
3. [Building with WebRTC Support](#3-building-with-webrtc-support)
4. [Sending Media to a Browser (Sender Pipeline)](#4-sending-media-to-a-browser)
5. [Receiving Media from a Browser (Receiver Pipeline)](#5-receiving-media-from-a-browser)
6. [Bidirectional Video Call](#6-bidirectional-video-call)
7. [Codec Selection](#7-codec-selection)
8. [Congestion Control (TWCC/GCC)](#8-congestion-control-twccgcc)
9. [ICE, STUN, and TURN](#9-ice-stun-and-turn)
10. [Signaling Integration](#10-signaling-integration)
11. [Debugging with chrome://webrtc-internals](#11-debugging-with-chromewebrtc-internals)
12. [Known Limitations](#12-known-limitations)

---

## 1. Quick Start

```bash
# Build the Docker image (includes libdatachannel + TWCC patch)
docker build -t zstreamer .

# Run the Chrome demo server (HTTP on :8000, WebSocket signaling on :8080)
docker run --rm --network host -v $(pwd):/workspace \
    -w /workspace/build zstreamer ./webrtc_chrome_server

# Open Chrome → http://localhost:8000 → click "Connect"
```

You should see a color-bar video with timecode overlay and hear a 440 Hz sine tone.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                   zstreamer pipeline                     │
│                                                         │
│  videotestsrc ──► text_overlay ──► x264_encoder ──►     │
│                                          ▼              │
│  audiotestsrc ──────────────► opus_encoder ──►          │
│                                          ▼              │
│                               webrtc_endpoint           │
│                               │  sink_video_0           │
│                               │  sink_audio_1           │
│                               │                         │
│                               │  SRTP/ICE/DTLS          │
└───────────────────────────────┼─────────────────────────┘
                                │  UDP packets
                     ┌──────────▼──────────┐
                     │   Chrome / Firefox   │
                     │   <video> element    │
                     └─────────────────────┘
```

### Signaling Flow

```
Browser                  WebSocket Server           zstreamer Pipeline
   │                          │                          │
   ├─── createOffer ──────────►                          │
   │                          ├─── set_remote_sdp ──────►│
   │                          │                          ├── createAnswer
   │                          │◄── local_description ────┤
   │◄── answer ───────────────┤                          │
   │                          │                          │
   ├─── ICE candidate ────────►                          │
   │                          ├─── add_ice_candidate ───►│
   │◄── ICE candidate ────────┤◄─── ice_candidate ───────┤
   │                          │                          │
   │◄══════ SRTP/RTP media ═══════════════════════════════│
```

---

## 3. Building with WebRTC Support

WebRTC is enabled automatically when `libdatachannel` is found. The Dockerfile builds it from source with the TWCC interceptor patch applied.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_WEBRTC` | auto | Force-enable/disable WebRTC support |
| `ENABLE_PLUGINS` | ON | Build WebRTC as a shared plugin (`libzst_webrtc.so`) |

### Docker Build

```bash
# Standard build
docker build -t zstreamer .

# Verify WebRTC is included
docker run --rm zstreamer bash -c "ls build/plugins/libzst_webrtc.so"
```

### Native Build

```bash
# Install libdatachannel
git clone https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel && git apply ../zstreamer/scripts/libdatachannel-twcc.patch
cmake -B build -DUSE_GNUTLS=ON && cmake --build build -j$(nproc)
sudo cmake --install build

# Build zstreamer
cmake -B build && cmake --build build -j$(nproc)
```

---

## 4. Sending Media to a Browser

### Minimal Sender Pipeline (C API)

```c
#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pipeline.h"
#include "zst_bus.h"
#include "zst_clock.h"
#include "zst_scheduler.h"
#include "zstreamer/elements/zst_webrtc_endpoint.h"

// 1. Create elements
zst_element_t* vsrc  = zst_element_factory_create("video_test_src", "vsrc");
zst_element_t* venc  = zst_element_factory_create("x264_encoder",   "venc");
zst_element_t* webrtc = zst_element_factory_create("webrtc_endpoint","webrtc");

// 2. Configure video source (640×480 @ 30fps)
zst_element_set_property_int(vsrc, "width",  640);
zst_element_set_property_int(vsrc, "height", 480);
zst_element_set_property_int(vsrc, "fps",    30);
zst_element_set_property_bool(vsrc, "real-time-pacing", true);

// 3. Configure encoder for low-latency WebRTC
zst_element_set_property_string(venc, "profile", "baseline");
zst_element_set_property_int(venc, "gop-size", 30);

// 4. Configure WebRTC endpoint
zst_element_set_property_string(webrtc, "stun-servers", "stun:stun.l.google.com:19302");

// 5. Add video track → creates "sink_video_0" pad
zst_webrtc_add_video_track(webrtc, ZST_WEBRTC_CODEC_H264, 96);

// 6. Build pipeline and link
zst_pipeline_t* pipeline = zst_pipeline_create("sender");
zst_pipeline_add_element(pipeline, vsrc);
zst_pipeline_add_element(pipeline, venc);
zst_pipeline_add_element(pipeline, webrtc);

zst_pad_link(zst_element_get_pad(vsrc, "src"),
             zst_element_get_pad(venc, "sink"));
zst_pad_link(zst_element_get_pad(venc, "src"),
             zst_element_get_pad(webrtc, "sink_video_0"));

// 7. Start pipeline
zst_clock_t* clock = zst_clock_system_create();
zst_pipeline_set_clock(pipeline, clock);
zst_clock_unref(clock);

zst_scheduler_config_t cfg = { .mode = ZST_SCHEDULER_MULTI_THREAD, .worker_threads = 2 };
zst_scheduler_t* sched = zst_scheduler_create(&cfg);
zst_scheduler_add_pipeline(sched, pipeline);
zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING);
zst_scheduler_start(sched);

// 8. Handle signaling via event bus
// When Chrome sends an SDP offer:
zst_webrtc_set_remote_description(webrtc, "offer", offer_sdp);

// The element fires ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION on the bus
// Read it and send the answer SDP back to Chrome via WebSocket.
```

### Audio + Video Sender

```c
// Add both tracks before linking
zst_webrtc_add_video_track(webrtc, ZST_WEBRTC_CODEC_H264, 96);
zst_webrtc_add_audio_track(webrtc, ZST_WEBRTC_CODEC_OPUS, 111);

// Link video: vsrc → venc → sink_video_0
zst_pad_link(zst_element_get_pad(venc, "src"),
             zst_element_get_pad(webrtc, "sink_video_0"));

// Link audio: asrc → aenc → sink_audio_1
zst_pad_link(zst_element_get_pad(aenc, "src"),
             zst_element_get_pad(webrtc, "sink_audio_1"));
```

### Full Working Example

See [`examples/webrtc_chrome/server.c`](../examples/webrtc_chrome/server.c) — a complete sender with embedded HTTP server and WebSocket signaling.

```bash
# Run
docker run --rm --network host -v $(pwd):/workspace \
    -w /workspace/build zstreamer ./webrtc_chrome_server \
    --http-port 8000 --port 8080 \
    --stun stun:stun.l.google.com:19302
```

---

## 5. Receiving Media from a Browser

When a browser sends video to zstreamer, the `webrtc_endpoint` element dynamically creates source pads for each incoming track.

### Receiver Pipeline (C API)

```c
#include "zstreamer/elements/zst_webrtc_endpoint.h"

// Track arrival callback — called when browser's video track arrives
static void on_track_added(zst_element_t* el, zst_pad_t* src_pad, void* user_data) {
    printf("New track: %s\n", src_pad->name);

    // Create decoder and sink
    zst_element_t* dec  = zst_element_factory_create("h264_decoder", "dec");
    zst_element_t* sink = zst_element_factory_create("fake_sink",    "sink");

    zst_pipeline_add_element(g_pipeline, dec);
    zst_pipeline_add_element(g_pipeline, sink);

    // Link dynamically: webrtc src → h264_decoder → fake_sink
    zst_pad_link(src_pad, zst_element_get_pad(dec, "sink"));
    zst_pad_link(zst_element_get_pad(dec, "src"),
                 zst_element_get_pad(sink, "sink"));

    // Bring new elements to PLAYING state
    zst_element_set_state(dec,  ZST_STATE_PLAYING);
    zst_element_set_state(sink, ZST_STATE_PLAYING);
}

// Register the callback on the webrtc element
zst_webrtc_set_on_track_callback(webrtc, on_track_added, NULL);

// When browser sends an SDP offer containing sendonly video:
zst_webrtc_set_remote_description(webrtc, "offer", offer_sdp);
```

### Supported Inbound Codecs

| Codec | Decoder Element |
|-------|----------------|
| H.264 | `h264_decoder` |
| H.265 | `h265_decoder` |
| VP8   | `vp8_decoder`  |
| VP9   | `vp9_decoder`  |
| Opus  | `opus_decoder` → `audio_resampler` |
| AAC   | `aac_decoder`  |

---

## 6. Bidirectional Video Call

A bidirectional call combines sender + receiver in a single `webrtc_endpoint`:

```
                         webrtc_endpoint
                        ┌──────────────────────┐
local camera ──────────►│ sink_video_0         │
local mic ─────────────►│ sink_audio_1         │
                        │                      │
                        │ src_video_0 ─────────►── decoder ──► display
                        │ src_audio_1 ─────────►── decoder ──► speaker
                        └──────────────────────┘
```

Both send and receive happen through the same PeerConnection. The key is to add tracks AND register the `on_track` callback:

```c
// Send our camera
zst_webrtc_add_video_track(webrtc, ZST_WEBRTC_CODEC_H264, 96);
zst_webrtc_add_audio_track(webrtc, ZST_WEBRTC_CODEC_OPUS, 111);

// Receive remote tracks
zst_webrtc_set_on_track_callback(webrtc, on_track_added, NULL);

// Set Chrome's offer (which contains both sendrecv transceivers)
zst_webrtc_set_remote_description(webrtc, "offer", sdp);
```

---

## 7. Codec Selection

### Automatic Selection (Default)

zstreamer ranks codecs by default preference: **H264 > VP8 > VP9 > Opus**.

When Chrome sends an offer with multiple codecs, the endpoint picks the highest-ranked mutual codec and sets the `selected-video-codec` / `selected-audio-codec` properties.

```c
char codec[32];
zst_element_get_property(webrtc, "selected-video-codec", codec, sizeof(codec));
// "H264", "VP8", "VP9", or "H265"
```

### Override Preference

```c
// Prefer VP8 over H264
zst_element_set_property_string(webrtc, "codec-preference", "VP8,H264,VP9");
```

### Creating the Right Encoder

```c
const char* codec_name = ...; // from "selected-video-codec"
const char* enc_type =
    strcmp(codec_name, "VP8") == 0 ? "vp8_encoder" :
    strcmp(codec_name, "VP9") == 0 ? "vp9_encoder" :
    strcmp(codec_name, "H265") == 0 ? "h265_encoder" :
    "x264_encoder"; // default H264

zst_element_t* venc = zst_element_factory_create(enc_type, "venc");
```

---

## 8. Congestion Control (TWCC/GCC)

zstreamer implements **Transport-Wide Congestion Control (TWCC)** using Google Congestion Control (GCC). See [`wiki/twcc-architecture.md`](twcc-architecture.md) for the full algorithm documentation.

### Automatic Bitrate Adaptation

When Chrome connects, the TWCC module:
1. Injects transport-wide sequence numbers into every outgoing RTP packet
2. Receives RTCP CCFB feedback from Chrome (~200ms intervals)
3. Runs GCC to compute a target bitrate
4. Posts `ZST_EVENT_WEBRTC_REMB` to the pipeline bus

Your application handles the event to update the encoder:

```c
// In your bus polling loop:
if (ev->type == ZST_EVENT_WEBRTC_REMB) {
    unsigned int bps = ev->as.webrtc_remb.bitrate;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", bps);
    zst_element_set_property(video_encoder, "bitrate", buf);
}
```

### Bitrate Bounds

| Parameter | Value | Description |
|-----------|-------|-------------|
| `TWCC_MIN_BITRATE` | 100 kbps | Floor (congested network) |
| `TWCC_MAX_BITRATE` | 8 Mbps | Ceiling |
| `TWCC_INIT_BITRATE` | 2 Mbps | Starting estimate |
| Update interval | 100 ms | GCC computation frequency |
| REMB hysteresis | 3% | Minimum change to post bus event |

### Requesting a Keyframe (PLI)

When a decoder misses a keyframe (e.g., after packet loss), it can request one via PLI:

```c
zst_webrtc_request_keyframe(webrtc, 0); // track index 0 = video
```

---

## 9. ICE, STUN, and TURN

### STUN (NAT hole-punching)

```c
zst_element_set_property_string(webrtc, "stun-servers",
    "stun:stun.l.google.com:19302");
```

Multiple STUN servers (comma-separated):
```c
zst_element_set_property_string(webrtc, "stun-servers",
    "stun:stun1.l.google.com:19302,stun:stun2.l.google.com:19302");
```

### TURN (Relay for symmetric NAT)

```c
zst_element_set_property_string(webrtc, "turn-servers",
    "turn:your-turn-server.example.com:3478");
zst_element_set_property_string(webrtc, "turn-username", "user");
zst_element_set_property_string(webrtc, "turn-password", "password");
```

TURNS (TURN over TLS):
```c
zst_element_set_property_string(webrtc, "turn-servers",
    "turns:your-server.example.com:5349");
```

### CLI (webrtc_chrome_server)

```bash
./webrtc_chrome_server \
    --stun stun:stun.l.google.com:19302 \
    --turn turn:your-turn.example.com:3478 \
    --turn-user myuser \
    --turn-pass mypassword
```

### Production coturn Setup

See [`wiki/phase-webrtc.md § 4.5`](phase-webrtc.md#45-production-turn-coturn-setup--integration-phase-8h) for a complete coturn configuration guide.

---

## 10. Signaling Integration

zstreamer uses an event bus for signaling. Your signaling server (WebSocket, HTTP, etc.) bridges browser ↔ zstreamer.

### Events Posted to Bus

| Event | Description | Fields |
|-------|-------------|--------|
| `ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION` | Send this SDP to browser | `type` ("offer"/"answer"), `sdp` |
| `ZST_EVENT_WEBRTC_ICE_CANDIDATE` | Send to browser | `candidate`, `mid`, `mlineindex` |
| `ZST_EVENT_WEBRTC_REMB` | GCC bandwidth estimate | `track_id`, `bitrate` (bps) |
| `ZST_EVENT_WEBRTC_PLI` | Remote requests keyframe | `track_id` |

### Bus Polling Loop (Typical Pattern)

```c
while (running) {
    zst_event_t* ev = NULL;
    if (zst_bus_pop(pipeline->bus, &ev, 0) != ZST_OK || !ev) {
        usleep(10000); // 10ms sleep when no events
        continue;
    }

    switch (ev->type) {
    case ZST_EVENT_WEBRTC_LOCAL_DESCRIPTION:
        // Forward SDP answer to browser via WebSocket
        send_to_browser("{\"type\":\"%s\",\"sdp\":\"%s\"}",
            ev->as.webrtc_local_description.type,
            ev->as.webrtc_local_description.sdp);
        break;

    case ZST_EVENT_WEBRTC_ICE_CANDIDATE:
        send_to_browser("{\"type\":\"candidate\",\"candidate\":\"%s\"}",
            ev->as.webrtc_ice_candidate.candidate);
        break;

    case ZST_EVENT_WEBRTC_REMB:
        // Update encoder bitrate
        update_encoder_bitrate(ev->as.webrtc_remb.bitrate);
        break;

    case ZST_EVENT_WEBRTC_PLI:
        // Force keyframe
        zst_webrtc_request_keyframe(webrtc, ev->as.webrtc_pli.track_id);
        break;

    case ZST_EVENT_EOS:
        stop_pipeline();
        break;
    }
    zst_event_destroy(ev);
}
```

### Injecting Browser Signaling into zstreamer

```c
// When browser sends SDP offer
zst_webrtc_set_remote_description(webrtc, "offer", sdp_from_browser);

// When browser sends ICE candidate
zst_webrtc_add_ice_candidate(webrtc,
    candidate_str,     // "candidate:..."
    sdp_mid,           // "0", "1", etc.
    sdp_mline_index);  // 0, 1, ...
```

---

## 11. Debugging with chrome://webrtc-internals

Open `chrome://webrtc-internals` while connected to zstreamer.

### Key Stats to Check

| Field | Expected | Problem if Wrong |
|-------|----------|-----------------|
| ICE connection state | `connected` / `completed` | NAT traversal failed → need TURN |
| DTLS state | `connected` | Certificate mismatch or ICE failed |
| Negotiated codecs | `H264` or `VP8` | SDP negotiation failed |
| `packetsReceived` | Increasing | Media not flowing |
| `framesDecoded` | Increasing | Decoder not working |
| `jitter` | < 50ms | Network congestion |
| `packetsLost` | Near 0 | Packet loss → TWCC will reduce bitrate |
| BUNDLE group | `video audio` | Multi-track negotiation failed |
| Transport | `transport-wide-cc` extension present | TWCC active |

### Server-Side Logs

Enable verbose logging in the demo server by checking:
```bash
docker logs -f webrtc-server 2>&1 | grep -E "twcc|REMB|TWCC|GCC"
```

Healthy output looks like:
```
INFO [twcc] GCC: delay_est=2160000 loss_est=2100000 combined=2100000 bps [loss=0.0% delay_grad=0.00 state=NORMAL]
INFO [bus_thread] Updated video encoder bitrate to 2100000 bps
```

### SDP Inspection

The demo server logs the SDP offer and answer:
```bash
docker logs webrtc-server 2>&1 | grep -A 50 "LOCAL SDP ANSWER"
```

---

## 12. Known Limitations

| Limitation | Workaround / Future Work |
|-----------|--------------------------|
| **TWCC with pre-existing RTP extensions** | If libdatachannel injects its own RTP header extension before our interceptor, TWCC extension injection is skipped (passes through). In practice, libdatachannel packetizers don't add extensions by default. |
| **Simulcast not supported** | Single-quality stream only. Future work: send multiple SSRCs with `a=rid` / `a=simulcast`. |
| **SVC (Scalable Video Coding)** | Not implemented. VP9/AV1 SVC requires `a=scalability-mode` in SDP. |
| **AV1 codec** | libdatachannel has `rtcSetAV1Packetizer` but no AV1 encoder element yet. |
| **H.265 in Chrome** | Chrome does not support H.265 over WebRTC by default (requires flag). Use H.264 or VP8/VP9 for Chrome compatibility. |
| **Multiple PeerConnections** | Each call to `zst_webrtc_open()` creates one PeerConnection per `webrtc_endpoint` instance. Multiple instances are supported — create one element per peer. |
| **ICE restart** | `zst_webrtc_restart_ice()` is implemented but the full browser re-offer flow needs application-level support. |
