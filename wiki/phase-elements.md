# Element Implementations — Phase 4  (✅ 40 implemented/started; 📝 planned additions)

Forty production elements are implemented or underway with real hardware/codec/protocol/display integrations and synthetic or graceful fallbacks where appropriate.
Additional planned elements cover future protocol/container/display expansion.
Two implemented elements handle format conversion (scaling, resampling) — essential once caps negotiation (Phase 5) requires automatic conversion between mismatched formats.

### 4a — V4L2 Source  (✅ done)
- [x] Open `/dev/video0` with O_RDWR | O_NONBLOCK
- [x] Format negotiation: `VIDIOC_S_FMT` (YUYV, 640×480)
- [x] MMAP buffer setup: `VIDIOC_REQBUFS` / `QUERYBUF` / `QBUF`
- [x] `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
- [x] poll-based non-blocking capture with timeout
- [x] YUYV → YUV420P colour space conversion
- [x] **Synthetic fallback** when no camera: moving vertical bar pattern, 30 fps

**Dependencies:** `libv4l-dev` (in Docker)

### 4ae — V4L2 Sink  (✅ done)

Outputs raw video buffers to a Video4Linux2 (V4L2) loopback or output device. Useful for creating virtual cameras or feeding hardware endpoints.

- [x] `v4l2sink` element with 1 sink pad (`video/x-raw`)
- [x] Backend: Native V4L2 ioctl API (`VIDIOC_S_FMT`, `VIDIOC_REQBUFS`, `VIDIOC_QBUF`, `VIDIOC_DQBUF`)
- [x] Support properties: `device` (e.g., `/dev/video1`), `width`, `height`, `pixel-format` (e.g., `YUYV`, `YUV420P`)
- [x] Memory integration: support `memory-type` selection (`mmap`, `userptr`, `dmabuf`) to accept and pass buffers without copies where supported
- [x] Frame pacing: blocks in `process` waiting for device readiness or downstream synchronization
- [x] Fallback logic: mocks output and logs a warning if the device fails to open, preventing pipeline crash during testing
- [x] Tests: instantiation verification and mock buffer pushing tests in `test_core.c`

### 4b — H.264 Encoder (x264enc)  (✅ done)
- [x] x264 integration: `x264_param_default_preset("ultrafast", "zerolatency")`
- [x] CRF rate control (23)
- [x] Accept I420 YUV planes from `zst_video_frame_t` payload
- [x] NAL unit concatenation into `zst_buffer` packets
- [x] PTS passthrough
- [x] EOS passthrough
- [x] Lazy initialization on first frame (handles dynamic resolution)

**Dependencies:** `libx264-dev` (in Docker)

### 4c — MP4 Muxer  (✅ done)
- [x] FFmpeg `libavformat` integration
- [x] Custom AVIO write callback pushes buffers downstream (not to file)
- [x] Video stream (H.264) + audio stream (AAC)
- [x] Fragmented MP4: `frag_keyframe+empty_moov+default_base_moof`
- [x] Per-stream EOS tracking: muxer waits for both video + audio EOS before propagating
- [x] Proper `av_write_trailer()` on stop

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev` (in Docker)

### 4d — File Sink  (✅ done)
- [x] FILE* writer: `fopen`, `fwrite`, `fclose`
- [x] Writes buffer memory data to file
- [x] Proper `close` lifecycle hook

### 4e — ALSA Audio Source  (✅ done)
- [x] `snd_pcm_open("default", SND_PCM_STREAM_CAPTURE)`
- [x] Parameter setup: S16_LE, 44100Hz, stereo, 0.5s latency
- [x] `snd_pcm_readi()` for capture
- [x] Underrun / xrun recovery (`-EPIPE` → `snd_pcm_prepare`)
- [x] **Synthetic fallback**: 440Hz square wave, 44100Hz timing with nanosleep

**Dependencies:** `libasound2-dev`

### 4f — AAC Encoder  (✅ done)
- [x] FFmpeg `libavcodec` AAC encoder: `avcodec_find_encoder(AV_CODEC_ID_AAC)`
- [x] S16LE interleaved → FLTP float planar conversion
- [x] `avcodec_send_frame()` / `avcodec_receive_packet()` API
- [x] 128kbps bitrate
- [x] EOS passthrough

**Dependencies:** `libavcodec-dev` (in Docker)

### 4g — Video Scaler  (✅ done)

A conversion element that scales video frames and converts pixel formats. Deployed
when a source's output caps (e.g. 1080p NV12) don't match the next element's input
caps (e.g. 720p I420).

- [x] **Interface**: single sink pad, single src pad — accepts raw video, outputs raw video
- [x] **Backend**: `libswscale` from FFmpeg (`sws_getContext` / `sws_scale`)
- [x] **Auto-configuration**: on first frame, allocate the SWS context based on input resolution/format and configured output resolution/format
- [x] Configurable target: `width`, `height`, `pixel_format` — or passthrough if formats match
- [x] **Synthetic fallback**: naive nearest-neighbour scaling if `libswscale` unavailable
- [x] EOS passthrough

**Dependencies:** `libswscale-dev` (in Docker)

### 4h — Audio Resampler  (✅ done)

Converts audio sample rate and format. Needed when source sample rate (e.g. ALSA
at 48000Hz) differs from what the encoder expects (e.g. AAC at 44100Hz), or when
format mismatches (S16LE ↔ F32LE).

- [x] **Interface**: single sink pad, single src pad — accepts raw audio, outputs raw audio
- [x] **Backend**: `libswresample` from FFmpeg (`swr_alloc_set_opts` / `swr_convert`)
- [x] **Auto-configuration**: on first frame, allocate SWR context from input/output params
- [x] Configurable: `sample_rate`, `sample_format`, `channels` — passthrough if matching
- [x] **Synthetic fallback**: linear interpolation resampling if `libswresample` unavailable
- [x] EOS passthrough

**Dependencies:** `libswresample-dev` (in Docker)

---

### 4i — File Source  (✅ done)

Reads raw or containerised media data from a local file and pushes it into the pipeline as a sequence of buffers. Analogous to GStreamer's `filesrc`.

- [x] `file_source` element with 1 src pad — configurable `path` property
- [x] Open file with `fopen`/`open` (O_RDONLY) on state transition to READY
- [x] Read chunks into `zst_buffer` pool, push to src pad
- [x] Send `EOS` when `feof()` / `read()` returns 0
- [x] Configurable `chunk_size` and `loop` (restart from beginning on EOF)
- [x] Support for `offset` / `length` to read a subset of the file
- [x] Caps negotiation: advertise `text/plain`, `video/x-h264`, `audio/aac`, etc. based on file extension or probe

### 4j — Network Source  (✅ done)

Receives raw byte streams over TCP or Unix sockets and feeds them into the pipeline as buffers.

> **Protocol layering note:** This is a **raw transport** element. It reads bytes from a socket without understanding any application-layer protocol (RTSP, RTMP, HTTP, SRT, etc.). It outputs opaque byte buffers — not demuxed video/audio pads. For protocol-aware streaming with automatic demuxing and caps negotiation, use **4o (RTSP Source)** or **4q (RTMP Source)** instead.

- [x] `net_source` element with 1 src pad — outputs raw byte buffers
- [x] TCP client mode: connect to remote host:port, read stream into buffers
- [x] TCP server mode: accept incoming connections, read from first connected client
- [x] Unix socket support for local IPC
- [x] UDP listener mode (`udp` / `udp-server`): bind to local port, receive incoming datagrams
- [x] UDP client mode (`udp-client`): connect to remote host:port, send active-pull/registration packet, receive datagrams from connected remote peer
- [x] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix, udp, udp-client)
- [x] Reconnection with exponential back-off on connection loss
- [x] Buffer size / read timeout configuration
- [x] EOS on clean disconnect; error recovery on unexpected disconnect
- [x] Caps negotiation: fixed `text/plain` caps or none (passthrough)

### 4k — Network Sink  (✅ done)

Sends raw byte buffers over TCP or Unix sockets. Enables local IPC and custom binary protocol output.

> **Protocol layering note:** This is a **raw transport** element. It writes bytes to a socket without understanding any application-layer protocol (RTSP, RTMP, HTTP, SRT, etc.). It accepts a single raw byte buffer on its sink pad — not demuxed video/audio streams. For protocol-aware streaming output with proper muxing, use **4p (RTSP Sink)** or **4r (RTMP Sink)** instead.

- [x] `net_sink` element with 1 sink pad — accepts raw byte buffers
- [x] TCP client mode: connect to remote host:port and write buffers
- [x] TCP server mode: listen, accept, and stream to connected clients
- [x] Unix socket support for local IPC
- [x] UDP client mode (`udp-client` / `udp`): write buffers to remote host:port
- [x] UDP server mode (`udp-server`): bind to local port, listen for active-pull client registration packet, stream buffers to registered client
- [x] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix, udp, udp-server)
- [x] Reconnection with exponential back-off on connection loss
- [x] Write timeout and buffer drain on disconnect
- [x] EOS passthrough: flush remaining data before closing connection
- [x] Outbound UDP timestamp-based pacing (opt-in via `timestamp-pacing`, `pacing-tolerance-ms`, `pacing-reset-threshold-ms`, and `max-lateness-ms`)

**Test deliverables:**
- [x] Property get/set for `host`, `port`, `protocol`, `path`, `write-timeout`
- [x] State transitions: NULL ↔ READY ↔ PLAYING
- [x] Caps negotiation: advertise `application/octet-stream` caps
- [x] TCP client mode test: connect to server, send data, verify reception
- [x] Socket error handling: reconnection with exponential back-off
- [x] EOS handling: graceful connection close
- [x] UDP push-based test: netsink (`udp-client`) to netsrc (`udp` / `udp-server`)
- [x] UDP pull-based test: netsrc (`udp-client`) to netsink (`udp-server`) with hole-punching/registration

---

### 4l — Video Test Source  (✅ done)

Generates synthetic video test patterns without any real hardware input. Useful for pipeline testing, benchmarking, and demo scenarios where no camera is available.

- [x] `video_test_src` element with 1 src pad
- [x] Configurable resolution (`width` x `height`), framerate, pixel format, and real-time pacing (`real-time-pacing`)
- [x] Test pattern options: colour bars (SMPTE/EBU), moving gradients, checkerboard, white noise, black/silent
- [x] Timestamp generation: `pts` set from pipeline clock at capture rate
- [x] EOS on `stop` state transition or configurable frame limit
- [x] Caps negotiation: advertise `video/x-raw` with configurable resolution/formats
- [x] Real-time pacing support (`real-time-pacing`) to mathematically pace generated frames like a webcam
- [x] Optional YUV420P → NV12 / RGB conversion in software
- [x] Loop mode: restart pattern sequence on frame limit or EOS

### 4m — Audio Test Source  (✅ done)

Generates synthetic audio test signals without any real hardware input. Useful for pipeline testing, latency measurement, and audio chain verification.

- [x] `audio_test_src` / `audiotestsrc` element with 1 src pad
- [x] Configurable sample rate, channels, sample format (S16LE, F32LE), and real-time pacing (`real-time-pacing`)
- [x] Signal options: sine wave (configurable frequency), square wave, pink/white noise, silence
- [x] Timestamp generation: `pts` set from pipeline clock or sample count based on `nb_samples`
- [x] EOS on `stop` or configurable sample/buffer limit
- [x] Caps negotiation: advertise `audio/x-raw` with configurable format/channels/rate
- [x] Real-time pacing support (`real-time-pacing`) to emit audio buffers at generated audio duration
- [x] Loop mode: restart signal sequence on limit or EOS

### 4n — Fake Sink  (✅ done)

Consumes and immediately discards incoming buffers without any I/O or processing. Used for headless profiling, throughput testing, and pipeline termination without a real output.

> **Video / Audio distinction?** A single fake sink is sufficient — both video and audio buffers behave identically: `zst_buffer_unref()` discards the buffer (returns it to the pool or frees it). GStreamer also uses a single `fakesink` for all media types. If per-type statistics are needed later (e.g. video fps vs audio latency), the element can count internally by `buffer->type` — no need for separate elements.

- [x] `fake_sink` element with 1 sink pad
- [x] Accept any caps — passthrough negotiation, no format restrictions
- [x] On `sink_push`: accumulate stats (total buffers, bytes processed)
- [x] EOS passthrough: count and acknowledge
- [x] Optional stats: total buffers received, bytes processed, buffer rate (per second by media type) via `get_property`
- [x] Optional `drop-probability` setting: randomly drop packets to simulate packet loss
- [x] Zero-copy path: buffer is released without touching payload memory

---

### 4s — Text Overlay  (✅ done)

Composites text (subtitles, timestamps, labels) onto raw video frames. Follows the same element pattern as other processing elements: single sink pad (raw video in), single src pad (raw video with text out).

- [x] `text_overlay` element with 1 sink pad (video/x-raw) + 1 src pad (video/x-raw)
- [x] Configurable text string (via element property or secondary text sink pad)
- [x] Backend: `libfreetype` for font rasterization (glyph bitmap generation)
- [x] Text layout: multi-line support with word wrapping
- [x] Configurable font family, size, colour, outline/shadow
- [x] Configurable position: absolute (x, y) or relative (centre, top-left, bottom-right)
- [x] Alpha blending of text bitmap onto YUV420P / NV12 frames
- [x] PTS passthrough (text overlay preserves video timestamps)
- [x] EOS passthrough
- [x] Caps negotiation: accept/caps on sink pad, same caps on src pad (passthrough)

**Dependencies:** `libfreetype-dev`

**Test deliverables:**
- [x] Unit test: render text onto a known frame, verify pixels at expected positions
- [x] Unit test: multi-line text wrapping
- [x] Unit test: EOS passthrough
- [x] Unit test: caps negotiation
- [x] Unit test: property get/set for font size, colour, position
- [x] Integration test: `v4l2src → text_overlay → filesink` produces video with visible text

### 4t — Text Source  (✅ Done)

Generates video frames with rendered text (no video input). Useful for test patterns, title cards, and simple slideshows.

- [x] `text_source` element: generates video frames with rendered text (no video input)
- [x] Useful for test patterns, title cards, and simple slideshows
- [x] Configurable resolution, framerate, text content, background colour

### 4u — SRT Subtitle Parser  (✅ done)

Parse SRT subtitle format into timed text events and feed them to `text_overlay` at correct PTS.

- [x] Parse SRT subtitle format into timed text events
- [x] Feed parsed text segments to `text_overlay` via `text` sink pad (pts/duration)
- [ ] Support ASS/SSA format parsing (advanced styling)
- [x] **Test deliverables**: Unit tests verifying parsing of subtitle files, segment timing, and correct PTS/duration propagation (planned)

### 4v — H.264 Decoder  (✅ done)

Decodes H.264 elementary stream packets into raw video frames for processing, transcoding, preview, or analysis pipelines.

- [x] `h264dec` element with 1 sink pad (`video/x-h264`) and 1 src pad (`video/x-raw`)
- [x] FFmpeg `libavcodec` decoder integration (`AV_CODEC_ID_H264`)
- [x] Accept Annex B bytestream and AVCC/extradata forms where possible
- [x] Convert `AVFrame` output into `zst_video_frame_t` payloads
- [x] Preserve PTS/DTS/duration and handle B-frame reordering
- [x] Caps negotiation: advertise raw pixel format, width, height, framerate
- [x] EOS drain/flush: send NULL packet, emit delayed frames, then propagate EOS
- [x] Decoder reset on stream parameter changes or corruption recovery

**Dependencies:** `libavcodec-dev`, `libavutil-dev` (in Docker)

### 4w — H.265 Encoder  (✅ Done)

Encodes raw video frames to H.265/HEVC for lower bitrate streaming and storage profiles.

- [x] `h265enc` element with 1 sink pad (`video/x-raw`) and 1 src pad (`video/x-h265`)
- [x] `x265enc` element with 1 sink pad (`video/x-raw`) and 1 src pad (`video/x-h265`)
- [x] Backend: x265 (`libx265`) and FFmpeg HEVC encoder (`AV_CODEC_ID_HEVC`) natively supported via two distinct plugins
- [x] Accept I420/YUV420P frames from `zst_video_frame_t` payload
- [x] Configurable preset/tune, CRF/bitrate, GOP/keyframe interval, profile/level
- [x] Output VPS/SPS/PPS headers and frame NAL units in Annex B format
- [x] PTS passthrough and monotonic DTS generation where needed
- [x] EOS flush: drain delayed encoder frames before propagating EOS
- [x] Caps negotiation: advertise `video/x-h265` with stream format/profile metadata

**Dependencies:** `libavcodec-dev`, `libavutil-dev`, `libx265-dev`

### 4x — H.265 Decoder  (✅ Done)

Decodes H.265/HEVC packets into raw video frames for HEVC ingest, transcoding, or inspection pipelines.

- [x] `h265dec` element with 1 sink pad (`video/x-h265`) and 1 src pad (`video/x-raw`)
- [x] FFmpeg `libavcodec` decoder integration (`AV_CODEC_ID_HEVC`)
- [x] Accept Annex B bytestream and hvcC/extradata forms where possible
- [x] Convert `AVFrame` output into `zst_video_frame_t` payloads
- [x] Preserve PTS/DTS/duration and handle B-frame reordering
- [x] Caps negotiation: advertise raw pixel format, width, height, framerate
- [x] EOS drain/flush: send NULL packet, emit delayed frames, then propagate EOS
- [x] Decoder reset on parameter-set changes or corruption recovery

**Dependencies:** `libavcodec-dev`, `libavutil-dev` (in Docker)

### 4y — AAC Decoder  (✅ done)

Decodes AAC packets into raw audio frames for playback, transcoding, waveform analysis, or audio filtering.

- [x] `aacdec` element with 1 sink pad (`audio/aac`) and 1 src pad (`audio/x-raw`)
- [x] FFmpeg `libavcodec` decoder integration (`AV_CODEC_ID_AAC`)
- [x] Accept ADTS and AudioSpecificConfig/extradata forms where possible
- [x] Convert decoder output to `zst_audio_frame_t` payloads
- [x] Optional conversion to interleaved S16LE or F32LE for downstream compatibility
- [x] Preserve PTS/DTS/duration and calculate duration from decoded sample count
- [x] Caps negotiation: advertise sample rate, channels, and sample format
- [x] EOS drain/flush: send NULL packet, emit delayed frames, then propagate EOS

**Dependencies:** `libavcodec-dev`, `libavutil-dev` (in Docker)

---

### 4o — RTSP Source  (✅ Done)

Receives live or on-demand streaming media from an RTSP server (DESCRIBE/SETUP/PLAY), demuxes RTP streams into separate video/audio source pads, and feeds them into the pipeline. The de-facto standard for IP camera ingestion.

- [x] `rtsp_source` element with 2+ src pads (video, audio, metadata)
- [x] RTSP control: DESCRIBE (SDP parsing), SETUP (transport negotiation), PLAY/PAUSE/TEARDOWN
- [x] RTP/RTCP transport: UDP (unicast + multicast), TCP interleaved mode
- [x] SDP → caps negotiation: map payload types (PT) to `video/x-h264`, `audio/aac`, etc.
- [x] RTSP authentication: Basic and Digest (MD5, qop=auth) with URL or property credentials
- [x] Reconnection: automatic re-DESCRIBE/SETUP/PLAY on transport loss with configurable delay and attempt limit
- [x] NTP timestamp correlation: RTCP Sender Reports map RTP timestamps to wall-clock based PTS
- [x] Configurable `rtsp_url`, `username`, `password`, `transport` (udp/tcp), `buffer_size`
- [x] RTSP keep-alive: periodic OPTIONS pings to prevent server timeout
- [x] EOS on RTSP BYE/TEARDOWN or RTCP BYE

### 4p — RTSP Sink  (✅ Done)

Acts as an RTSP server element that accepts incoming RTP streams and makes them available for RTSP clients to connect and consume (pull model). Enables live relay scenarios where zstreamer is the streaming source.

- [x] `rtsp_sink` element with 2+ sink pads (video, audio)
- [x] Built-in lightweight RTSP server: listen on configurable port, handle DESCRIBE/SETUP/PLAY
- [x] SDP generation from input caps: generate SDP body from pad caps on all pads ready
- [x] RTP/RTCP transport: UDP unicast per connected client, TCP interleaved fallback
- [x] RTP packetisation: H.264 (RFC 3984), AAC (RFC 3640), generic payload wrapping
- [x] RTCP sender reports: generated by the RTSP/RTP muxing backend; configurable sender-report interval exposed as `rtcp-interval-ms`
- [x] Configurable `listen_port`, `mount_point`, `max_clients`, `transport`, `rtcp-interval-ms`
- [x] Outbound UDP timestamp-based pacing (enabled by default via `udp-timestamp-pacing`, `udp-pacing-tolerance-ms`, `udp-pacing-reset-threshold-ms`, and `udp-max-lateness-ms`)

### 4q — RTMP Source  (✅ Done)

Connects to an RTMP server (or receives RTMP pushes) and demuxes the FLV stream into video/audio buffers. Essential for consuming from live streaming platforms, OBS pushes, and legacy IP cameras.

- [x] `rtmp_source` element with 2 src pads (video, audio)
- [x] RTMP handshake + connect: `connect("rtmp://host/live/streamkey")`, `createStream`, `play`
- [x] FLV demuxing: parse FLV tag headers, extract video (H.264/HEVC/AV1) and audio (AAC/MP3)
- [x] AMF0/AMF3 metadata parsing: extract `onMetaData` (width, height, framerate, samplerate)
- [x] Timestamp mapping: FLV timestamps → pipeline clock PTS
- [x] Configurable `rtmp_url`, `live` (true/false for live vs VOD), `buffer_time`, `swf_url`
- [x] Authentication: `rtmp://user:pass@host/app/streamkey` URL form supported by FFmpeg RTMP input
- [x] Reconnection: auto-reconnect on stream loss with configurable delay and attempt limit
- [x] EOS on RTMP stream end or `deleteStream`

### 4z — RTSP Server (Multi-Session)  (✅ Done)

Full RTSP server element that serves multiple mount points on a single port.
Each mount point exposes a named pair of sink pads (video/audio).
Connected clients can DESCRIBE/SETUP/PLAY and receive RTP-over-RTSP interleaved data.
Designed using patterns from ireader/media-server's librtsp.

- [x] `rtsp_server` element with 2×N sink pads (N sessions, each with video + audio)
- [x] Multi-session: `zst_rtsp_server_add_session(el, "session0")` → creates `session0_video` + `session0_audio` pads
- [x] Single-port listener (default 8554) with per-client thread
- [x] Full RTSP protocol: DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, OPTIONS
- [x] URI-based routing: `rtsp://host:8554/session0` → session0's stream
- [x] RTP packetization: H.264 (RFC 3984 single NAL + FU-A), AAC (RFC 3640 MPEG4-Generic)
- [x] RTP-over-RTSP TCP interleaved transport (`$` + channel + length + payload)
- [x] SDP generation per session with correct rtpmap/fmtp
- [x] RTCP Sender Reports with NTP/RTP timestamps (every 5s)
- [x] Concurrent client support — multiple clients can stream the same session
- [x] Per-client RTP state (SSRC, seq, timestamps)
- [x] H.264 NAL unit scan and fragmentation
- [x] Dynamic plugin build (`libzst_rtsp_server.so`)
- [x] Outbound UDP timestamp-based pacing (enabled by default via `udp-timestamp-pacing`, `udp-pacing-tolerance-ms`, `udp-pacing-reset-threshold-ms`, and `udp-max-lateness-ms`)

### 4r — RTMP Sink  (✅ Done)

Publishes pipeline output to an RTMP ingest endpoint — the standard way to push to YouTube Live, Twitch, Facebook Live, and most CDNs.

- [x] `rtmp_sink` element with 2 sink pads (video, audio)
- [x] RTMP handshake + publish: `connect(...)`, `publish("streamkey")`
- [x] FLV muxing: wrap incoming H.264/AAC buffers into FLV tags, maintain correct tag boundaries
- [x] AMF0 metadata injection: `@setDataFrame("onMetaData")` with `width`, `height`, `framerate`, `videocodecid`, `audiocodecid`, `duration`
- [x] Timestamp generation: pipeline clock → FLV timestamps (milliseconds, monotonically increasing)
- [x] Configurable `rtmp_url`, `live` (true = no buffer, low latency)
- [x] Authentication: `rtmp://user:pass@host/app/streamkey`
- [x] Reconnection: auto-reconnect on publish failure, exponential back-off
- [x] EOS passthrough: send `FCUnpublish` on stream end, clean disconnect

### 4aa — SRT Transport Protocols (Secure Reliable Transport Source & Sink)  (✅ Done)

Adds Secure Reliable Transport (SRT) ingest and egress elements for low-latency, loss-resilient contribution workflows. This is a transport protocol and is distinct from **4u SRT Subtitle Parser** (SubRip text subtitles).

- [x] `srtsrc` element with 1+ src pads for received MPEG-TS or raw byte-stream payloads
- [x] `srtsink` element with 1 sink pad for MPEG-TS or raw byte-stream payloads
- [x] Backend: `libsrt` socket API (`srt_create_socket`, `srt_connect`, `srt_bind`, `srt_listen`, `srt_accept`, `srt_sendmsg2`, `srt_recvmsg2`)
- [x] Modes: caller, listener, and rendezvous
- [x] Configurable URI/properties: `uri`, `host`, `port`, `mode`, `latency`, `passphrase`, `pbkeylen`, `streamid`, `payload-size`
- [x] AES encryption support via SRT passphrase/key length
- [x] Reconnection and exponential back-off on link loss
- [x] Timestamp mapping from SRT packet time / pipeline clock to buffer PTS
- [x] EOS on socket close or graceful shutdown
- [x] Caps negotiation: advertise `video/mp2t` for MPEG-TS mode and `application/octet-stream` for raw mode
- [x] Optional integrated MPEG-TS demux/mux handoff for H.264/H.265/AAC pipelines


**Dependencies:** `libsrt-dev`

> [!NOTE]
> The `srtsrc` and `srtsink` elements are compiled together into a single dynamic plugin shared library (`libzst_srt.so`). This avoids code duplication of the shared initialization logic in `srt_common.c` and resolves AddressSanitizer thread-specific data (TSD) lifecycle crashes by handling global `libsrt` lifecycle safely.

### 4ab — MPEG Transport Stream Muxer / Demuxer (`.ts`)  (✅ Done)

Adds MPEG-TS container support for broadcast, SRT contribution, HLS segmenting, and interoperability with common streaming tools. The muxer/demuxer should support encoded H.264, H.265/HEVC, and AAC payloads.

- [x] `tsmux` element with sink pads for `video/x-h264`, `video/x-h265`, and `audio/aac`, plus one `video/mp2t` src pad
- [x] `tsdemux` element with one `video/mp2t` sink pad and dynamic/static src pads for H.264, H.265, and AAC elementary streams
- [x] Backend: FFmpeg `libavformat` MPEG-TS muxer/demuxer or native TS packet writer/parser
- [x] PAT/PMT generation and parsing with stream type mapping for H.264 (`0x1b`), H.265 (`0x24`), and AAC (`0x0f`)
- [x] PES packetization/depacketization with correct PTS/DTS propagation
- [x] PCR generation, continuity counters, adaptation fields, and packet alignment to 188-byte TS packets
- [x] Annex B handling for H.264/H.265 payloads, including parameter-set propagation where needed
- [x] AAC ADTS / LATM handling strategy documented and implemented for mux/demux compatibility
- [x] EOS handling: flush partial PES/TS packets and propagate downstream EOS
- [x] Caps negotiation: `video/mp2t` container caps and elementary stream caps on demuxed pads
- [x] Tests: H.264/AAC and H.265/AAC mux → demux roundtrip preserves payload boundaries and timestamps

**Adaptive demuxing follow-up (Completed):** `tsdemux` has been refactored from fixed `video` / `audio` outputs to stream-table-backed dynamic pads (`video_%u`, `audio_%u`, `text_%u`, `data_%u`) with pad-added/removed, stream-status, caps-changed, and signal-lost/present events. See [Adaptive Stream Demuxing Plan](phase-adaptive-stream-demuxing.md).

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev` (if FFmpeg-backed)

### 4ac — MP4 File Demuxer  (✅ done)

Demuxes MP4/fragmented-MP4 files or byte streams into encoded elementary audio/video buffers for decode, remux, or streaming pipelines.

- [x] `mp4demux` element with one sink pad and video/audio src pads
- [x] Backend: FFmpeg `libavformat` demuxer (`avformat_open_input`, `av_read_frame`) with custom AVIO support for pipeline buffers
- [x] Track discovery: expose H.264, H.265/HEVC, AAC, and other tracks where present
- [x] Preserve PTS/DTS/duration and map MP4 track timescales to zstreamer nanosecond timestamps
- [x] Support fragmented MP4 (`moof`/`mdat`) via push-mode streaming with custom non-seekable AVIO
- [x] Direct-file mode: `location` property opens MP4 file directly via `avformat_open_input()`
- [x] EOS handling after final packet and error reporting
- [x] Caps negotiation: `video/x-h264`, `video/x-h265`, `audio/aac` (and more) output pads
- [x] Tests: MP4 mux→demux roundtrip with H.264/AAC; verify packet counts, timestamps; property/factory tests

**Adaptive demuxing follow-up (Completed):** `mp4demux` has been migrated to the stream-table and dynamic-pad model so fragmented MP4 / late track discovery can expose new pads and caps changes at runtime. See [Adaptive Stream Demuxing Plan](phase-adaptive-stream-demuxing.md).

**Dependencies:** `libavformat-dev`, `libavcodec-dev`, `libavutil-dev`

### 4ad — HTTP Source  (✅ done)

Fetches static files or progressive streams over HTTP or HTTPS and pushes the response body downstream as a sequence of buffers. Analogous to GStreamer's `souphttpsrc`.

- [x] `httpsrc` element with 1 src pad
- [x] Backend: FFmpeg libavformat network protocol handler or `libcurl`
- [x] Configurable `url` / `uri` property
- [x] Handle standard HTTP GET requests, redirection (301, 302, 307), and chunked transfer encoding
- [x] Support custom request headers (e.g. `User-Agent`, `Authorization`)
- [x] Connection timeout, receive timeout, and error recovery/retry logic
- [x] Emit `EOS` when Content-Length is reached or connection is closed by peer
- [x] Buffer pool integration for low-overhead memory recycling
- [x] Caps negotiation: advertise `application/octet-stream` or media-type based on `Content-Type` header (if known)
- [x] Tests: verify streaming from a mock HTTP server, check redirect handling, and verify EOS propagation

**Dependencies:** `libcurl4-openssl-dev` or `libavformat-dev`

### 4ae — Xilinx VCU Encoder (📝 Planned)

Hardware-accelerated video encoder leveraging the Xilinx Video Codec Unit (VCU) via the `vcu-ctrl-sw` API.
Reference: https://github.com/Xilinx/vcu-ctrl-sw.git

- [ ] Wrap `lib_encode` from `vcu-ctrl-sw`
- [ ] Support H.264 (AVC), HEVC, and VP9 hardware encoding
- [ ] Implement rate control and multi-channel configurations
- [ ] Map `zst_buffer_t` frames to `AL_TBuffer` using `zst_allocator_dmabuf_create`
- [ ] Achieve end-to-end zero-copy by importing DMABUF fds via `AL_Allocator_Import`

### 4af — Xilinx VCU Decoder (📝 Planned)

Hardware-accelerated video decoder leveraging the Xilinx Video Codec Unit (VCU) via the `vcu-ctrl-sw` API.
Reference: https://github.com/Xilinx/vcu-ctrl-sw.git

- [ ] Wrap `lib_decode` from `vcu-ctrl-sw`
- [ ] Support H.264 (AVC), HEVC, and VP9 hardware decoding
- [ ] Handle hardware-accelerated picture planes and alignment
- [ ] Decode directly into DMABUFs allocated by `zstreamer`'s pool for zero-copy output
- [ ] Map pixel plane metadata (`AL_TPixMapMetaData`) to `zst_video_frame_t` payloads

### 4ag — Intel oneAPI Video Encoder (✅ Done)

Hardware-accelerated video encoder for Intel GPU pipelines using oneAPI/SYCL memory integration and Intel media acceleration APIs.

- [x] Add `oneapi_video_encoder` / `oneapienc` element with 1 sink pad (`video/x-raw`) and 1 src pad (`video/x-h264` or `video/x-h265`)
- [x] Select an Intel backend: oneVPL for hardware encode control
- [x] Support H.264 (AVC) and H.265/HEVC hardware encoding
- [x] Implement bitrate, GOP/keyframe interval, FPS, profile, and level properties
- [ ] Accept `ZST_MEMORY_ONEAPI` buffers from `zst_allocator_oneapi_create()` without CPU copies when the backend supports shared device memory
- [x] Provide CPU YUV420P upload path via internal NV12 surfaces for raw frames from non-oneAPI sources
- [ ] Provide DMABUF upload/import path for raw frames from non-oneAPI sources
- [x] Preserve input PTS/duration and drain delayed encoder frames on EOS
- [x] Add runtime capability probing so the element is skipped gracefully when Intel GPU drivers or oneVPL are unavailable
- [x] Tests: property/factory coverage, unsupported-runtime skip path, and encode smoke test when hardware is available
- [x] Build and test only in a dedicated Intel oneAPI Docker image, separate from the default `zstreamer` image
- [x] Add `Dockerfile.oneapi` based on Intel's oneAPI development image with oneVPL/media dependencies installed
- [x] Add CMake option `ENABLE_ONEAPI_ENCODER` so oneAPI-specific code is opt-in and does not affect non-Intel builds
- [x] Add oneAPI CI/test command: `docker build -f Dockerfile.oneapi -t zstreamer-oneapi . && docker run --rm zstreamer-oneapi`

**Dependencies:** Intel oneAPI runtime, oneVPL dispatcher, Intel oneVPL GPU runtime (`libmfx-gen1`), Intel media VA driver, compatible Intel GPU driver

### 4ah — VA-API Video Encoder (✅ Done)

Hardware-accelerated video encoder for Linux GPUs exposing VA-API encode entrypoints, with initial focus on AMD Radeon GPUs via the Mesa `radeonsi` VA driver and optional Intel VA-API support where available.

- [x] Add `vaapi_video_encoder` / `vaapienc` element with 1 sink pad (`video/x-raw`) and 1 src pad (`video/x-h264` or `video/x-h265`)
- [x] Use a VA-API backend (`h264_vaapi` / `hevc_vaapi`) with DRM render node selection (`/dev/dri/renderD*`)
- [x] Runtime capability probing: skip gracefully when the VA-API device or encode backend is unavailable
- [x] Support H.264 (AVC) and H.265/HEVC where advertised by the VA driver
- [x] Implement `device`, `codec`, `bitrate`, `gop-size`, `profile`, `level`, `preset`, and `rate-control` properties
- [ ] Accept DMABUF-backed raw frames for zero-copy import when upstream allocators/exporters support it
- [x] Provide CPU/system-memory upload fallback for raw frames from non-VAAPI sources
- [x] Preserve input PTS/duration and drain delayed encoded frames on EOS
- [x] Output H.264/H.265 byte-stream packets suitable for downstream muxers/sinks
- [x] Tests: property/factory coverage, unsupported-runtime skip path, and encode smoke test when VA-API hardware encode is available
- [x] Add dedicated VA-API Docker image, separate from the default `zstreamer` image
- [x] Build/test command: `docker build -f Dockerfile.vaapi -t zstreamer-vaapi . && docker run --rm zstreamer-vaapi`
- [x] Add CMake option `ENABLE_VAAPI_ENCODER` once the element implementation lands

**Dependencies:** `libva-dev`, `libva-drm2`, `mesa-va-drivers` for AMD/radeonsi, a compatible DRM render node exposed via `/dev/dri`, and optionally `intel-media-va-driver` for Intel VA-API devices

### 4ai — VA-API Video Decoder (vaapidec) (📝 Planned)

Hardware-accelerated video decoder for Linux GPUs exposing VA-API decode entrypoints, supporting hardware-accelerated decoding of H.264/H.265 video streams directly into GPU surfaces/DMABUFs.

- [ ] Add `vaapi_video_decoder` / `vaapidec` element with 1 sink pad (`video/x-h264` or `video/x-h265`) and 1 src pad (`video/x-raw`)
- [ ] Use a VA-API backend (`h264_vaapi` / `hevc_vaapi` / `avcodec` VA-API decoding) with DRM render node selection (`/dev/dri/renderD*`)
- [ ] Decode compressed H.264/H.265 streams directly into GPU surfaces (VAAPI memory / NV12 surfaces)
- [ ] Support exporting decoded frames as DMABUFs for zero-copy downstream rendering or processing
- [ ] Provide CPU/system-memory fallback download path for raw frames to non-VAAPI sources
- [ ] Preserve input PTS/DTS and handle B-frame reordering
- [ ] Caps negotiation: advertise raw pixel formats, resolution, framerate, and supported profiles
- [ ] Add capability checking and graceful runtime skip if VA-API hardware/driver is missing
- [ ] Tests: property/factory coverage, unsupported-runtime skip path, and decode smoke test with synthetic compressed inputs

---

### 4aj — X11 Sink (x11sink)  (✅ Initial implementation; follow-ups tracked)

Display sink that renders CPU-backed raw video frames to an X11 window using Xlib. Provides a simple on-screen preview for pipelines without GPU/OpenGL dependencies and falls back to null-sink mode in headless environments.

- [x] `x11sink` element with 1 sink pad — accepts raw video frames for display
- [x] **Window management**: creates an X11 window via Xlib (`XCreateSimpleWindow` / `XMapWindow`), sets a configurable `window-title`, and sizes the window from negotiated caps or incoming frame metadata
- [ ] **Initial window geometry**: configurable width/height and position (`window-x`, `window-y`) properties are not implemented yet
- [x] **Frame upload**: draws converted frames with `XPutImage`
- [ ] **MIT-SHM fast path**: `XShmPutImage` / shared-memory transfer is not implemented yet
- [x] **Pixel format conversion**: accepts `YUV420P`, `RGB24`, and `BGR24`; converts to the display visual's RGB masks before upload
- [ ] **NV12 support**: not implemented yet
- [ ] **Aspect-ratio preservation**: letterbox/pillar-box resize handling and `force-aspect-ratio` are not implemented yet
- [ ] **Fullscreen toggle**: `_NET_WM_STATE_FULLSCREEN`, F11, and double-click fullscreen handling are not implemented yet
- [x] **Event handling**: processes `ClientMessage` / `WM_DELETE_WINDOW` and switches to null-sink mode when the display window is closed
- [ ] **Keyboard handling / EOS on close**: ESC/Q handling and downstream EOS propagation on window close are not implemented yet
- [ ] **Frame pacing / dropping**: timestamp-based `max-lateness` dropping and QoS events are not implemented yet
- [ ] **Window reconfiguration**: resize (`ConfigureNotify`) handling and dynamic image reallocation are not implemented yet
- [x] **Lifecycle handling**: frees XImage backing storage, GC, window, and display on close / state change to NULL
- [x] **Caps negotiation**: `get_caps` advertises `video/x-raw` formats `YUV420P`, `RGB24`, and `BGR24`
- [ ] **Descriptor metadata follow-up**: built-in registry pad metadata currently uses the generic sink template; tighten it to `video/x-raw` with supported formats
- [x] **Graceful fallback**: if `DISPLAY` is unset or `XOpenDisplay` fails, logs a warning and operates as a null sink (consume/discard frames) to avoid pipeline crashes in headless environments
- [x] **Properties**: `display`, `window-title`, read-only `frame-count`; `title` is accepted as an alias for `window-title`
- [x] **Tests**: `tests/test_x11sink.c` covers factory creation, property get/set, null-mode state transitions, direct buffer processing, frame count, and a `videotestsrc → x11sink` scheduler smoke test; also runs under Xvfb for real display-open coverage
- [ ] **Test follow-ups**: add pixel readback/assertions, resize handling tests, EOS-on-window-close tests, and keyboard event tests
- [x] **CMake integration**: guarded by `ENABLE_X11SINK` (default ON), uses CMake `find_package(X11)`, defines `HAS_X11SINK`, and links X11 libraries when available
- [x] **Plugin packaging**: built into `zstreamer-elements` and as dynamic plugin `libzst_x11sink.so`
- [x] **Docker test environment**: main `Dockerfile` includes `libx11-dev`, `libxext-dev`, and `xvfb`; CI `ctest` includes `test_x11sink`

**Dependencies:** `libx11-dev`, `libxext-dev`; test environment also uses `xvfb`

---

### 4ak — OpenGL Sink (glsink)  (✅ Initial implementation; follow-ups tracked)

Display sink that renders raw video frames using OpenGL. The current implementation is GLX/X11-backed and validated in a headless Docker environment using Xvfb + Mesa software rendering.

- [x] `glsink` element with 1 sink pad (`video/x-raw`) — accepts raw video frames for GPU-accelerated display
- [x] **GLX context creation**: OpenGL context via X11/GLX (`glXChooseVisual`, `glXCreateContext`, `glXMakeCurrent`)
- [ ] **EGL / Wayland / KMS backend**: not implemented yet; current backend is X11/GLX only
- [x] **Window management**: create an X11 window, set title, map window, attach GLX drawable
- [x] **Configurable properties**: `window-title`, `width`, `height`, `fullscreen`, `vsync`, `scaling`, `max-lateness`, `color-matrix`, `brightness`, `contrast`, `saturation`
- [ ] **Window positioning**: `window-x` / `window-y` not implemented yet
- [x] **GPU YUV→RGB conversion**: GLSL shaders for YUV420P (3-plane), NV12 (2-plane), and RGB input; configurable `color-matrix` property (`bt601`, `bt709`)
- [ ] **Shader correctness follow-up**: review found the current YUV matrix upload and NV12 chroma sampling need correction before production use
- [ ] **Custom shader loading**: `vertex-shader` / `fragment-shader` properties not implemented yet
- [x] **Runtime colour controls**: `brightness`, `contrast`, and `saturation` uniforms supported
- [ ] **Hue control**: not implemented yet
- [x] **V-Sync**: GLX swap interval support via `glXSwapIntervalEXT`; configurable `vsync` boolean (default ON)
- [x] **Scaling modes**: `fit`, `stretch`, and `crop`
- [ ] **DMABUF import**: `EGL_EXT_image_dma_buf` zero-copy path not implemented yet
- [ ] **Frame pacing / QoS**: `max-lateness` property exists, but late-frame dropping and QoS events are not yet implemented
- [x] **Event handling**: ESC/Q exits, F11 toggles fullscreen, window close returns EOS; keyboard & mouse events (clicks & motion) propagated onto the pipeline bus; default exit/fullscreen actions bypassable via `handle-events = false`
- [x] **EOS / lifecycle handling**: destroys GL programs, textures, GLX context, X11 window/display on close
- [x] **Graceful fallback**: if `$DISPLAY` is unset or GL initialization fails, element runs as a null sink and discards frames safely
- [x] **CMake integration**: `ENABLE_GLSINK` option (default ON), `HAS_GL` compile define, X11/OpenGL detection via CMake `find_package`
- [x] **Plugin packaging**: built into `zstreamer-elements` and as dynamic plugin `libzst_glsink.so`
- [x] **Docker test environment**: `Dockerfile.gl` builds with X11/OpenGL/Mesa/Xvfb dependencies and runs `test_gl_sink` + `test_core`
- [x] **Tests**: `tests/test_gl_sink.c` covers factory creation, typed properties, state transitions, null-mode processing, EOS handling, scaling property validation, and an Xvfb pipeline smoke test
- [ ] **Test follow-ups**: add pixel readback assertions, DMABUF import tests (when implemented), and avoid starting nested Xvfb when `$DISPLAY` is already set

**Known implementation follow-ups from review:**
- Create the X11 window using the GLX-selected visual/colormap before `glXMakeCurrent` for real-display portability.
- Detect pixel format from `frame->format`, not only plane count; reject or implement packed YUYV/BGR paths safely.
- Fix NV12 shader sampling for `GL_LUMINANCE_ALPHA` (`.ra` or switch to `GL_RG/GL_RG8`).
- Fix YUV matrix upload transpose (`GL_TRUE` or column-major matrix layout).
- Add built-in descriptor property metadata and `video/x-raw` sink pad template (dynamic plugin metadata already advertises these).
- Call `XInitThreads()` or constrain/marshal rendering to one thread for multi-thread scheduler safety.
- Apply initial fullscreen mode and implement actual `max-lateness` dropping.
- Generate installed pkg-config dependencies dynamically so GL-only installs do not inherit unrelated optional multimedia deps.

**Dependencies:** `libx11-dev`, `libxext-dev`, `libgl1-mesa-dev` / `libgl-dev`, `libglx-dev`, `libglu1-mesa-dev`; test environment also uses `xvfb` and Mesa software rendering (`LIBGL_ALWAYS_SOFTWARE=true`)

---

### 4al — OpenGL Compositor Sink (glcompsink)  (✅ Initial implementation)

Video compositor display sink that composites multiple raw video streams into one configurable canvas and renders the result with OpenGL. Unlike a transform compositor, `glcompsink` is a terminal sink: it owns the output window/canvas and consumes all input streams.

- [x] `glcompsink` element with dynamic request sink pads (`sink_%u`) accepting `video/x-raw` from multiple sources
- [x] **Canvas model**: configurable `canvas-width`, `canvas-height`, `background-color`, and optional `window-title`; render all inputs into the shared canvas
- [x] **Per-pad layout**: configure `x`, `y`, `width`, `height`, `z-order`, `alpha`, `visible`, and `scaling` (`fit`, `stretch`, `crop`) per input pad
- [x] **Composition backend**: GLX/X11 lifecycle and shader upload paths; draw each input as a textured quad into the canvas in z-order
- [x] **Format support**: accepts common raw video formats supported by `glsink` first (`YUV420P`, `NV12`, `RGB`), with safe rejection for unsupported layouts
- [x] **Synchronization**: align frames by PTS against the pipeline clock; retain the latest frame per pad and compose at a configured output/display rate via worker thread
- [x] **Frame pacing / QoS**: configurable `max-lateness`; drop or reuse late/missing per-pad frames without blocking unrelated inputs
- [x] **Dynamic pads**: supports adding input pads by `input-count`, `request-pad`, or `zst_gl_comp_sink_request_pad()`; releases per-pad textures/frame references on close/destroy
- [x] **Input pad removal**: added `zst_gl_comp_sink_release_pad()` and core `zst_element_remove_pad()` for dynamic graph changes
- [x] **Thread safety**: serializes compositor state and GL context access with an element mutex for multi-pad push paths; dedicated rendering worker thread
- [x] **Window/display controls**: vsync, window close/ESC handling, F11 fullscreen toggle, resize handling, and graceful null-sink fallback when `$DISPLAY` or GL initialization is unavailable
- [x] **Caps metadata**: sink pad template advertises `video/x-raw` for built-in and dynamic plugin introspection
- [x] **Properties API**: exposes canvas-level typed properties and pad-level layout properties via `sink_N::property` names; includes a public convenience API header
- [x] **Tests**: factory/property coverage, dynamic pad creation, request-pad API, null-mode multi-input processing, EOS handling per pad and global EOS behavior
- [x] **GL validation tests**: Xvfb/Mesa smoke test composing two synthetic inputs; pixel readback assertions (z-order, alpha, background, scaling) added via `test_gl_comp_sink.c`
- [x] **CMake integration**: guarded with `ENABLE_GLCOMPSINK` (default ON when OpenGL/X11 dependencies are available) and shares GL dependency detection with `glsink`
- [x] **Plugin packaging**: built into `zstreamer-elements` and as dynamic plugin `libzst_glcompsink.so`
- [x] **Docker test environment**: `Dockerfile.gl` enables `ENABLE_GLCOMPSINK` and runs `test_gl_comp_sink` alongside `test_gl_sink` and `test_core`

**Known implementation follow-ups:**
- [x] Add clock-driven composition at a configured display rate and align/reuse frames by PTS.
- [x] Add input pad removal/release API for READY/PLAYING, not just add/close cleanup.
- [x] Apply initial fullscreen mode and F11 fullscreen toggle parity with `glsink`.
- [x] Add stricter layout validation against canvas bounds and richer caps negotiation per pad.
- [x] Add pixel readback assertions for z-order, alpha, background, and scaling correctness via `zst_gl_comp_sink_capture()` API and `test_gl_comp_sink.c` tests.
- [x] Implement optional per-input border/background controls.
- [x] Configurable X11 keyboard event handling and key event bus propagation:
  - [x] Add `ZST_EVENT_KEY_PRESS` to `zst_event_type_t` in `include/zst_bus.h` and its structure/constructor in `src/zst_bus.c`.
  - [x] Add `handle-events` property (boolean, default `true`) to `glcompsink`.
  - [x] Update `comp_check_events` to always post a `ZST_EVENT_KEY_PRESS` event to the pipeline's bus on X11 key press.
  - [x] Make default key actions (exit on ESC/Q, fullscreen on F11) conditional on `handle-events == true`.
  - [x] Add unit/integration tests to verify event propagation and property configuration.
- [x] X11 mouse event handling and bus propagation:
  - [x] Add `ZST_EVENT_MOUSE_BUTTON` and `ZST_EVENT_MOUSE_MOTION` to `zst_event_type_t` in `include/zst_bus.h` and their structures/constructors in `src/zst_bus.c`.
  - [x] Update XSetWindowAttributes mask in `gl_comp_sink.c` to listen for `ButtonPress`, `ButtonRelease`, and `PointerMotion`.
  - [x] Update `comp_check_events` to post `ZST_EVENT_MOUSE_BUTTON` and `ZST_EVENT_MOUSE_MOTION` to the pipeline bus.
  - [x] Expand unit tests to verify creation, coordinates, and destruction of mouse events.

**Dependencies:** `libx11-dev`, `libxext-dev`, `libgl1-mesa-dev` / `libgl-dev`, `libglx-dev`, `libglu1-mesa-dev`; test environment reuses the `Dockerfile.gl` Xvfb/Mesa setup

---

### 4am — Audio Mixer (audiomixer)  (✅ Done)

Audio mixing element that accepts multiple synchronized audio frame streams and generates one mixed audio output stream. This is a transform/aggregator element, not a terminal sink: it has multiple input pads and one source pad for downstream encoders, sinks, or muxers.

- [x] `audiomixer` element with dynamic request sink pads (`sink_%u`) accepting `audio/x-raw` and one src pad producing mixed `audio/x-raw`
- [x] **Caps negotiation**: select a common output sample rate, channel count, and sample format; sink pads accept both S16LE and F32LE
- [x] **Supported formats**: interleaved S16LE and F32LE; mix internally in double-precision float to avoid clipping during accumulation
- [x] **Per-pad controls**: configurable `volume`, `mute` via pad-qualified property syntax (`pad_name::volume`, `pad_name::mute`)
- [x] **Mixing behavior**: sum active streams with per-pad gain, clamp to [−1.0, 1.0], output in configured format
- [x] **Dynamic pads**: request pads via `zst_audio_mixer_request_pad()`; up to 64 inputs
- [x] **EOS handling**: track EOS per sink pad; continue mixing remaining active pads; emit downstream EOS once all pads have ended
- [x] **Thread safety**: per-input queues protected by mutex + condvar for worker-thread mixer
- [x] **Property access**: set/get per-pad properties via `zst_element_set_property(el, "sink_0::volume", "0.5")`
- [x] **ASRC integration**: ASRC is handled by the separate `audioresampler` element (see [ASRC section](phase-advanced.md#asrc-drift-compensation-in-audioresampler)), not inside the mixer
- [x] **Buffer management**: fallback allocation when no pool is available; proper `buf->destroy` cleanup
- [x] **CMake integration**: built into `zstreamer-elements` and as dynamic plugin `libzst_audiomixer.so`
- [x] **Factory registration**: registered in `zst_builtins.c` under factory name `"audiomixer"`

**Dependencies:** core zstreamer audio frame APIs only.  ASRC for asynchronous inputs requires inserting `audioresampler asrc-mode=pts` upstream.

**Follow-ups (completed):**
- [x] Per-pad `pan`/balance
- [x] PTS-based input alignment with silence fill for missing/late inputs
- [x] Latency/QoS: `max-lateness` dropping
- [x] Dynamic pad removal during PLAYING
- [x] Dedicated integration tests with `audio_test_src`

---

### 4an — SDP Muxer / Generator (`sdpmuxer`)  (✅ Done)

Generates Session Description Protocol (SDP) text for RTP-oriented media sessions. This complements `sdpdemux`: `sdpmuxer` describes media streams, RTP payload mappings, and optional out-of-band codec configuration, while transport/RTP packetization remains the responsibility of RTSP/RTP-capable elements.

**Implementation status:** complete for the currently supported SDP-generator role. The element is implemented in core + dynamic-plugin form, has public API coverage, can generate in-memory and file-backed SDP, derives useful defaults from negotiated caps, extracts common codec setup data from observed buffers, and now has dedicated unit/plugin-introspection coverage. SDP generation for newer codecs is present, but end-to-end streaming for those codecs still depends on matching RTP packetizer/depacketizer support elsewhere in the pipeline.

- [x] `sdpmuxer` element with optional `video` and `audio` sink pads plus one `src` pad emitting `application/sdp`
- [x] Generate core SDP lines: `v=`, `o=`, `s=`, `c=`, `t=`, media sections, `rtpmap`, `fmtp`, and `control`
- [x] Configurable properties: address, session name, video/audio codec, enable/disable video/audio, RTP ports, payload types, AAC sample rate/channels, optional SDP file output path, and `emit-once`
- [x] H.264 SDP: `H264/90000`, `packetization-mode=1`, and optional `sprop-parameter-sets` extracted from Annex-B SPS/PPS observed on the video sink
- [x] H.265 SDP: `H265/90000` and optional `sprop-vps`, `sprop-sps`, `sprop-pps` extracted from Annex-B VPS/SPS/PPS
- [x] AAC SDP: `MPEG4-GENERIC/<rate>/<channels>` with AAC-hbr fmtp and generated/observed AudioSpecificConfig
- [x] Public header: `include/zstreamer/elements/zst_sdp_muxer.h`
- [x] CMake integration: built into `zstreamer-elements` and as dynamic plugin `libzst_sdpmuxer.so`
- [x] Factory registration: registered in `zst_builtins.c` under factory name `"sdpmuxer"` (`"sdpmux"` alias)
- [x] Caps-aware defaults: derives video/audio codec, AAC sample rate/channels, H.264 profile-level-id/sprop fields, H.265 sprop fields, and AAC codec data from negotiated sink-pad caps when present
- [x] Additional SDP payload descriptions: VP8, VP9, AV1, Opus, PCMU, PCMA, and L16/PCM can be generated when selected by properties or caps
- [x] Basic unit test for default H.264 + AAC SDP generation and configurable properties
- [x] Demo: `demo_sdp_multicast` shows `sdpmuxer` + test sources + H.264/AAC RTP multicast and `sdpdemuxer` + fakesinks receiving from the generated SDP

**Follow-ups (completed):**
- [x] Add dedicated unit tests for H.264/H.265 parameter-set extraction, AAC ADTS config extraction, and plugin introspection
- [x] Derive codec/track defaults from negotiated caps once caps metadata carries stream-format/profile-level details
- [x] Add optional SDP file output property for offline publishing workflows
- [x] Support additional RTP payload descriptions (Opus, PCMU/PCMA, VP8/VP9/AV1) when selected by properties/caps; matching RTP packetizer/depacketizer support remains the responsibility of the transport payload elements

---

### 4ao — RTP Depayloader (`rtpdepay`)  (✅ Done)

Adds the receive-side counterpart to `rtppay`: accepts complete `application/x-rtp` packet buffers from UDP/RTSP/file transports and emits codec access-unit buffers suitable for decoders, muxers, or tests.

**Implementation plan:**

- [x] Public API: add `include/zstreamer/elements/zst_rtp_depayloader.h` with `zst_rtp_depayloader_create()`.
- [x] Element shape: one `sink` pad accepting `application/x-rtp`, one `src` pad emitting `video/x-h264`, `video/x-h265`, `audio/x-aac`, `audio/aac`, or `audio/x-raw` based on the `codec` property.
- [x] RTP parsing: validate RTP version/header length, payload type, sequence continuity, RTP timestamp, marker bit, SSRC, CSRC count, and optional extension header.
- [x] Codec depayloading:
  - H.264 RFC 3984 single NAL, STAP-A, and FU-A reassembly into Annex-B access units.
  - H.265 RFC 7798 single NAL, aggregation packet, and FU reassembly into Annex-B access units.
  - AAC RFC 3640 AU-header parsing, emitting raw AAC access units.
  - PCM/raw audio payload concatenation for packets split by `rtppay` MTU, flushed on marker bit.
- [x] Properties and stats: mirror `rtppay` where useful (`codec`, `payload-type`, `clock-rate`/`sample-rate`, `channels`, `sample-size`) plus read-only packet/output/drop counters.
- [x] Integration: build into `zstreamer-elements`, expose as dynamic plugin `libzst_rtpdepay.so`, and register with the builtin factory under `"rtpdepay"` plus aliases.
- [x] Tests: add `rtppay ! rtpdepay` roundtrip coverage for fragmented H.264 Annex-B access units and AAC AU-header extraction.

