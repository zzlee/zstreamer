# Element Implementations — Phase 4  (✅ 33 implemented; 📝 planned additions)

Thirty-three production elements are implemented with real hardware/codec/protocol integrations and synthetic fallbacks where appropriate.
Additional planned elements cover future protocol/container expansion.
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

### 4b — H.264 Encoder  (✅ done)
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
- [x] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix)
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
- [x] Configurable `host`, `port`, `protocol` (tcp-client, tcp-server, unix)
- [x] Reconnection with exponential back-off on connection loss
- [x] Write timeout and buffer drain on disconnect
- [x] EOS passthrough: flush remaining data before closing connection

**Test deliverables:**
- [x] Property get/set for `host`, `port`, `protocol`, `path`, `write-timeout`
- [x] State transitions: NULL ↔ READY ↔ PLAYING
- [x] Caps negotiation: advertise `application/octet-stream` caps
- [x] TCP client mode test: connect to server, send data, verify reception
- [x] Socket error handling: reconnection with exponential back-off
- [x] EOS handling: graceful connection close

---

### 4l — Video Test Source  (✅ done)

Generates synthetic video test patterns without any real hardware input. Useful for pipeline testing, benchmarking, and demo scenarios where no camera is available.

- [x] `video_test_src` element with 1 src pad
- [x] Configurable resolution (`width` x `height`), framerate, pixel format
- [x] Test pattern options: colour bars (SMPTE/EBU), moving gradients, checkerboard, white noise, black/silent
- [x] Timestamp generation: `pts` set from pipeline clock at capture rate
- [x] EOS on `stop` state transition or configurable frame limit
- [x] Caps negotiation: advertise `video/x-raw` with configurable resolution/formats
- [ ] Optional YUV420P → NV12 / RGB conversion in software
- [x] Loop mode: restart pattern sequence on frame limit or EOS

### 4m — Audio Test Source  (✅ done)

Generates synthetic audio test signals without any real hardware input. Useful for pipeline testing, latency measurement, and audio chain verification.

- [x] `audio_test_src` / `audiotestsrc` element with 1 src pad
- [x] Configurable sample rate, channels, sample format (S16LE, F32LE)
- [x] Signal options: sine wave (configurable frequency), square wave, pink/white noise, silence
- [x] Timestamp generation: `pts` set from pipeline clock or sample count based on `nb_samples`
- [x] EOS on `stop` or configurable sample/buffer limit
- [x] Caps negotiation: advertise `audio/x-raw` with configurable format/channels/rate
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
- [x] Backend: x265 (`libx265`) or FFmpeg HEVC encoder (`AV_CODEC_ID_HEVC`)
- [x] Accept I420/YUV420P frames from `zst_video_frame_t` payload
- [x] Configurable preset/tune, CRF/bitrate, GOP/keyframe interval, profile/level
- [x] Output VPS/SPS/PPS headers and frame NAL units in Annex B format
- [x] PTS passthrough and monotonic DTS generation where needed
- [x] EOS flush: drain delayed encoder frames before propagating EOS
- [x] Caps negotiation: advertise `video/x-h265` with stream format/profile metadata

**Dependencies:** `libavcodec-dev`, `libavutil-dev`

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
- [ ] Multiple concurrent client support — use `rtsp_server` (4z) for full multi-client serving; `rtspsink` remains the simple FFmpeg-backed RTSP sink
- [x] RTP packetisation: H.264 (RFC 3984), AAC (RFC 3640), generic payload wrapping
- [x] RTCP sender reports: generated by the RTSP/RTP muxing backend; configurable sender-report interval exposed as `rtcp-interval-ms`
- [x] Configurable `listen_port`, `mount_point`, `max_clients`, `transport`, `rtcp-interval-ms`

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

**Dependencies:** `libsrt-gnutls-dev` (or `libsrt-openssl-dev` if not found)

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


