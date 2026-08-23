# UDP Outbound Timestamp-Based Pacing Plan

## Goal

Introduce a reusable **Timestamp-based Pacing** algorithm for elements that emit packets over outbound UDP so live RTP/UDP streams are transmitted according to media timestamps instead of being burst as fast as upstream can push buffers.

Primary targets:

- `src/net_sink.c` for `protocol=udp` / `udp-client` and `udp-server` replies.
- `src/rtsp_server.c` for RTP over UDP unicast and multicast.
- `src/rtsp_sink.c` when FFmpeg RTSP output is configured for UDP transport.

Non-targets:

- TCP RTSP interleaving and TCP/Unix `netsink` modes.
- SRT (`srt_sink`) because libsrt already provides its own UDP congestion control and pacing semantics.
- `rtp_payloader` and `tsmux`; they should preserve timestamps but should not sleep because they are not transport sinks.

---

## Design Principles

1. **Pace by buffer timestamps, not packet count.** RTP fragments from one access unit must be sent back-to-back; pacing happens once per input buffer/access unit.
2. **Use monotonic clock time.** Use `zst_clock_t` from the element when available; otherwise fall back to an internal system monotonic clock.
3. **Delta-based scheduling.** First timestamp establishes `(base_ts, base_clock)`. Later buffers target `base_clock + (buf_ts - base_ts)`.
4. **Never sleep while holding RTSP server locks.** Pacing must happen before packetization/client iteration or via per-stream pacer state outside `srv->lock`.
5. **Do not drop by default.** Late buffers are sent immediately unless an explicit `max-lateness-ms` property is configured.
6. **Preserve backward compatibility.** Raw UDP `netsink` pacing should be opt-in; RTSP UDP pacing can default on because RTSP is a real-time transport.

---

## Shared Pacer Helper

Add an internal helper, for example:

```text
src/zst_timestamp_pacer.h
src/zst_timestamp_pacer.c
```

Suggested state:

```c
typedef struct {
    int enabled;
    int started;
    zst_time_t base_ts;
    zst_time_t base_clock;
    zst_time_t last_ts;
    zst_time_t tolerance_ns;
    zst_time_t reset_threshold_ns;
    zst_time_t max_lateness_ns; /* 0 = disabled */
    uint64_t paced_count;
    uint64_t dropped_count;
    uint64_t total_wait_ns;
    zst_clock_t* fallback_clock;
    pthread_mutex_t lock;
} zst_timestamp_pacer_t;
```

Suggested API:

```c
void zst_timestamp_pacer_init(zst_timestamp_pacer_t* p);
void zst_timestamp_pacer_reset(zst_timestamp_pacer_t* p);
void zst_timestamp_pacer_deinit(zst_timestamp_pacer_t* p);

/* Returns ZST_OK to send, ZST_AGAIN/ZST_TIMEOUT to skip/drop by policy. */
zst_result_t zst_timestamp_pacer_wait(
    zst_timestamp_pacer_t* p,
    zst_clock_t* clock,
    zst_time_t timestamp_ns,
    int* dropped_out);
```

Timestamp selection:

1. Prefer `buf->dts` if non-zero and valid for encoded streams.
2. Otherwise use `buf->pts`.
3. If both are zero on the first buffer, establish the base and send immediately. If every buffer is zero, pacing naturally becomes a no-op.

Core algorithm:

```text
now = clock_get_time(clock)
if disabled: send immediately
if first buffer or timestamp moved backwards or discontinuity > reset_threshold:
    base_ts = timestamp
    base_clock = now
    last_ts = timestamp
    send immediately

target = base_clock + (timestamp - base_ts)
if now > target and max_lateness_ns > 0 and now - target > max_lateness_ns:
    mark dropped
    return skip/drop
if target > now + tolerance_ns:
    sleep target - now, preferably in small chunks for responsive shutdown
send
```

Defaults:

| Setting | Suggested default | Rationale |
|---------|-------------------|-----------|
| `enabled` | element-specific | Preserve raw UDP behavior, enable RTSP UDP. |
| `tolerance_ns` | 2–5 ms | Avoid excessive tiny sleeps. |
| `reset_threshold_ns` | 2 s | Treat seeks/large discontinuities as new timeline. |
| `max_lateness_ns` | 0 | Do not drop unless explicitly requested. |

---

## Element Integration

### 1. `netsink` UDP modes

File: `src/net_sink.c`

Add fields to `net_sink_t`:

- `zst_timestamp_pacer_t pacer;`
- `int timestamp_pacing;`
- `uint64_t pacing_tolerance_ms;`
- `uint64_t pacing_reset_threshold_ms;`
- `uint64_t max_lateness_ms;`

Behavior:

- Apply pacing only for `NET_SINK_PROTOCOL_UDP_CLIENT` and `NET_SINK_PROTOCOL_UDP_SERVER` after the UDP destination/client is known and before `sendto()`.
- Do not pace TCP/Unix modes.
- Default `timestamp-pacing=false` for backward compatibility with generic datagram use.
- For RTP multicast demos, enable via property on the UDP `netsink` instances.

New properties:

- `timestamp-pacing` (`bool`, default `false`)
- `pacing-tolerance-ms` (`uint`, default `5`)
- `pacing-reset-threshold-ms` (`uint`, default `2000`)
- `max-lateness-ms` (`uint`, default `0`, disabled)

### 2. `rtsp_server` UDP RTP

File: `src/rtsp_server.c`

Add pacer state at the session/media level, not per RTP fragment:

```c
zst_timestamp_pacer_t video_udp_pacer;
zst_timestamp_pacer_t audio_udp_pacer;
```

Integration point:

- At the start of `session_deliver()`, before `pthread_mutex_lock(&srv->lock)`, determine if any playing client for the session uses `RTSP_TRANSPORT_UDP` or `RTSP_TRANSPORT_MULTICAST`.
- If yes, call the corresponding session/media pacer once for the buffer timestamp.
- Then packetize and send all RTP fragments for that access unit immediately.

Important constraints:

- Do not sleep inside `write_rtp_packet()`; that would incorrectly pace every RTP fragment.
- Do not sleep under `srv->lock`; this would block RTSP control handling and unrelated sessions.
- RTCP Sender Reports remain controlled by `RTCP_INTERVAL_MS` and should not use media pacing.
- Multicast de-duplication remains unchanged; pacing occurs once before the first multicast send for a media buffer.

Properties on `rtsp_server`:

- `udp-timestamp-pacing` (`bool`, default `true`)
- `udp-pacing-tolerance-ms` (`uint`, default `5`)
- `udp-pacing-reset-threshold-ms` (`uint`, default `2000`)
- `udp-max-lateness-ms` (`uint`, default `0`)

### 3. `rtspsink` UDP transport

File: `src/rtsp_sink.c`

Add one pacer per sink pad/stream:

- `video_pacer`
- `audio_pacer`

Behavior:

- In `rtsp_sink_write_packet()`, if `transport` is UDP and pacing is enabled, pace once before `av_interleaved_write_frame()` using the selected stream timestamp.
- Do not pace TCP transport by default.
- Default can be `true` for UDP transport because RTSP sink acts as a real-time sender.

Properties:

- `udp-timestamp-pacing` (`bool`, default `true`)
- `udp-pacing-tolerance-ms` (`uint`, default `5`)
- `udp-pacing-reset-threshold-ms` (`uint`, default `2000`)
- `udp-max-lateness-ms` (`uint`, default `0`)

---

## Implementation Phases

### Phase A — Internal helper

- [x] Add `src/zst_timestamp_pacer.{c,h}`.
- [x] Add helper source to `CMakeLists.txt` core/elements sources as needed.
- [x] Implement init/reset/deinit and delta-based wait.
- [x] Use safe unsigned `zst_time_t` comparisons; never subtract unless `a > b` is checked.
- [x] Add lightweight stats counters for paced, dropped, reset count, and total wait time.

### Phase B — `netsink` integration

- [x] Add pacer fields and properties.
- [x] Reset pacer in `start/stop` and deinit in `close` path as appropriate.
- [x] Call pacer only in UDP send paths before `sendto()`.
- [x] Update plugin and built-in property descriptors.
- [x] Update multicast demo to enable `timestamp-pacing=true` where RTP over UDP is expected to be real-time.

### Phase C — `rtsp_server` integration

- [x] Add per-session video/audio UDP pacers.
- [x] Initialize/reset pacers when sessions are created, PLAY starts, PAUSE/TEARDOWN occurs, or session state is reset.
- [x] Add server-level UDP pacing properties and apply them to all session pacers.
- [x] Pace once per incoming video/audio buffer before taking `srv->lock`.
- [x] Preserve RTP timestamp calculation and packetization behavior.

### Phase D — `rtspsink` integration

- [x] Add video/audio pacers and UDP pacing properties.
- [x] Pace before `av_interleaved_write_frame()` only when `transport=udp` or UDP multicast.
- [x] Verify FFmpeg muxer options do not already introduce conflicting pacing; existing RTSP options remain limited to transport/listen/RTCP interval.

### Phase E — Tests

- [x] Unit-test the pacer with a fake/manual `zst_clock_t` so tests do not rely on wall-clock sleeps.
- [x] Add `netsink` UDP loopback timing test with 3–5 buffers spaced 10–20 ms apart; verify receive deltas are not burst when pacing is enabled.
- [x] Add a disabled-pacing regression test verifying old burst behavior remains available for raw UDP.
- [x] Add RTSP UDP smoke timing coverage by recording RTP packet arrival deltas from `rtsp_server` UDP transport.
- [x] Confirm existing UDP tests still pass: `test_net_sink`, `test_rtsp_source_bunny_udp_verification`, multicast SDP demo if covered.

### Phase F — Documentation

- [x] Update `wiki/clock-sync-debug.md` to cross-reference UDP sender pacing.
- [x] Update element docs/property descriptors for `netsink`, `rtsp_server`, and `rtspsink`.
- [x] Document recommended pipelines: `rtppay ! netsink protocol=udp timestamp-pacing=true`.

---

## Acceptance Criteria

- RTP/UDP packet bursts are paced according to media PTS/DTS for `rtsp_server` UDP and opt-in `netsink` UDP.
- RTP fragments belonging to the same access unit remain contiguous and are not individually delayed.
- TCP behavior is unchanged.
- Existing tests pass in Docker.
- New timing tests demonstrate that 20 ms timestamp spacing results in approximately paced UDP receive intervals, within test tolerance.
- No blocking sleeps occur while holding the RTSP server global mutex.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| UDP sender blocks shutdown during a long sleep | Sleep in small quanta and reset/disable pacer on stop. |
| Pacing every RTP fragment reduces throughput and breaks decoder assumptions | Pace once per input buffer/access unit only. |
| Multiple clients with different PLAY times share a session pacer | Pacing is session/media-level; RTP timestamp base remains per-client. On PLAY reset or first active UDP client, reset the session/media pacer. |
| Raw UDP users expect immediate send | Keep `netsink` pacing disabled by default. |
| Timestamp discontinuities after seek/flush cause long waits | Reset baseline on backward timestamps or large gaps. |
| Unsigned timestamp underflow | Always compare before subtracting `zst_time_t`. |
