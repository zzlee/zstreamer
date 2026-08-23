# Phase 11: HLS (HTTP Live Streaming) Integration Plan

## 1. Overview
The goal of this phase is to introduce **HTTP Live Streaming (HLS)** support to `zstreamer`, enabling it to generate Apple HLS compliant streams and serve them over HTTP. The implementation will focus on broad codec support (H.264, H.265, AAC, Opus, PCM) and modern HLS features such as **Fragmented MP4 (fMP4)** encapsulation for HEVC and next-gen audio.

## 2. Core Objectives
*   **Media Segmenting:** Slice continuous A/V streams into discrete segment files (`.ts` for MPEG-TS, `.m4s` for fMP4) based on keyframe boundaries.
*   **Playlist Generation:** Dynamically generate and update HLS Media Playlists (`.m3u8`) and Master Playlists.
*   **Broad Codec Support:** 
    *   **Video:** H.264 (AVC), H.265 (HEVC).
    *   **Audio:** AAC, Opus, PCM (uncompressed audio support via fMP4).
*   **Transport Flexibility:** Allow writing segments directly to a local filesystem directory (for Nginx/Apache to serve) or serving them directly from memory via a built-in lightweight `hls_server` element.

---

## 3. Architecture & New Elements

### 3.1 `hls_sink` (HLS Segmenter & Playlist Generator)
A new compound sink element responsible for receiving encoded/muxed streams and managing the HLS directory structure.
*   **Inputs:** Receives encoded streams (H.264/H.265 + Audio).
*   **Properties:** 
    *   `target-duration`: Target length of each segment in seconds (e.g., 2, 4, 6 seconds).
    *   `playlist-length`: Number of segments to keep in the live playlist (e.g., 5). Set to 0 for VOD (append-only).
    *   `format`: `ts` (MPEG-TS) or `fmp4` (Fragmented MP4).
    *   `location`: Output directory path (e.g., `/tmp/hls/`).
*   **Internal Mechanics:**
    *   Wraps the existing `tsmux` or `mp4mux` (configured for fragmentation).
    *   Forces segment boundaries exclusively on Video IDR (Key) frames to ensure independent decodability.
    *   Maintains the rotating `.m3u8` manifest file on disk.

### 3.2 `mp4mux` / `tsmux` Enhancements
*   **fMP4 Support:** The existing `mp4mux` (backed by libavformat) must be updated to support the `movflags=frag_keyframe+empty_moov` flags to generate valid `init.mp4` headers and `.m4s` segment bodies.
*   **Codecs:**
    *   **TS Encapsulation:** Used primarily for legacy compatibility (H.264 + AAC).
    *   **fMP4 Encapsulation:** Required by Apple for H.265 (HEVC) and modern audio codecs (Opus, PCM/FLAC).

### 3.3 `http_server` (Optional Built-in Server)
*   A lightweight, multi-threaded HTTP/1.1 server element built on standard POSIX sockets (similar to the existing RTSP server).
*   **Role:** Serves the `.m3u8` manifests and media segments directly from RAM or disk to HLS clients (Safari, VLC, hls.js, AVPlayer).

---

## 4. Implementation Roadmap

### Phase 11.1: Fragmented MP4 (fMP4) and Muxer Upgrades
1.  Upgrade the `mp4mux` element to support fragmented output modes.
2.  Add a `split_at_keyframe` property to the muxers, allowing the pipeline to gracefully reset the muxer context and close/open a new file descriptor without breaking the stream timebase.
3.  Ensure Opus and PCM audio formats are properly mapped to MP4/fMP4 atoms via FFmpeg.

### Phase 11.2: HLS Playlist Generation (`hls_sink`)
1.  Create `src/hls_sink.c`.
2.  Implement the playlist writer logic (creating `#EXTM3U`, `#EXT-X-VERSION: 7`, `#EXT-X-TARGETDURATION`, `#EXTINF`, etc.).
3.  Implement a rolling window algorithm to delete old segment files (`.ts` / `.m4s`) from the filesystem once they fall out of the live playlist window.
4.  Handle Master Playlist (`#EXT-X-STREAM-INF`) generation for multi-bitrate streams (e.g., 1080p, 720p, 480p variants).

### Phase 11.3: Built-in HTTP Delivery
1.  Create `src/http_server.c` element.
2.  Implement basic HTTP GET request handling.
3.  Add MIME type mapping (`application/vnd.apple.mpegurl` for m3u8, `video/mp2t` for TS, `video/mp4` for fMP4).
4.  Allow `hls_sink` to pass memory-mapped segments directly to `http_server` without writing to disk (Zero-copy RAM serving).

### Phase 11.4: End-to-End Testing
1.  **Test 1 (Legacy TS):** `videotestsrc` -> `x264enc` -> `aacenc` -> `hls_sink (format=ts)`. Validate with VLC.
2.  **Test 2 (Modern fMP4):** `videotestsrc` -> `x265enc` -> `opusenc` -> `hls_sink (format=fmp4)`. Validate with Safari.
3.  **Test 3 (PCM Audio):** `audiotestsrc` -> `hls_sink (format=fmp4)`. Validate audio-only HLS.
4.  Write integration tests in `tests/test_hls.c` that parse the output `.m3u8` to ensure segment durations mathematically match the target framerates.
