# Dante TX/RX Video RTP Handoff

## Current status

Latest pushed commit:

```text
877de80 Fix Dante video RTP txrx integrity
```

Branch:

```text
main
```

Known local untracked item:

```text
build_new/
```

Do not commit `build_new/` unless explicitly needed.

## Target binary

ARM64 test binary rebuilt in NFS-mounted build tree:

```text
/home/zzlee/docker/zstreamer/build-arm64/tests/test_dante_av_txrx_integrity
```

Current md5 after rebuild:

```text
ba8dda3d7e31e88bcc9b5421332cad0f
```

Remote boards see the same build tree via NFS:

```text
/mnt/dev/zzlee/docker/zstreamer/build-arm64
```

## Remote test hosts

RX board:

```text
petalinux@sc6f0-060f00112336.local
hostname: dante-rx
eth1: 192.168.190.101/23
```

TX board:

```text
petalinux@sc6f0-060f00112334.local
hostname: dante-tx
eth1: 192.168.190.99/23
```

SSH works. `sudo` is passwordless, but most test runs do not require sudo. `tcpdump` is not installed on RX.

## Key fixes already made

### Dante protocol / DVC schema compatibility

Files:

- `src/dante_protocol.c`
- `src/dante_protocol.h`
- `src/dante_session.c`
- `include/zst_bus.h`
- `src/zst_bus.c`
- `tests/test_dante_session.c`

Implemented/fixed:

- Official DVR sends `createVideoUnicastTxFlow` **without** `transmitterAddress` and **with** `videoSubtype`.
- TX flow now treats `transmitterAddress` as optional.
- RX flow still requires `transmitterAddress`.
- `videoSubtype` parsed for video create-flow actions.
- `start` message no longer includes `bitrateLimitInMBitS`; DVC schema has `additionalProperties: false`.
- DVC `requestConfiguration` heartbeat is ignored after start; trace spam reduced.

Expected TX flow shape:

```text
flowIndex, channelIndex, port, receiverAddress, videoSubtype
NO transmitterAddress
```

Expected RX flow shape:

```text
flowIndex, channelIndex, port, receiverAddress, transmitterAddress, videoSubtype
```

### TX RTP send path

Files:

- `tests/test_dante_av_txrx_integrity.c`
- `src/dante_udp_sink.c`
- `src/dante_video_coordinator.c`
- `include/zstreamer/elements/zst_dante_video_coordinator.h`
- `src/x264_encoder.c`

Implemented/fixed:

- `test_dante_av_txrx_integrity` now links video TX as:

```text
videotestsrc -> x264enc -> dantevideocoordinator
```

Previously it only linked `x264enc -> coordinator`, so scheduler never pushed raw video frames into encoder.

- `danteudpsink` now accepts empty `transmitter-address` and binds `0.0.0.0`.
- `danteudpsink process(NULL)` returns `ZST_OK` idle instead of `ZST_ERROR`.
- `x264_encoder process(NULL)` returns `ZST_OK` idle instead of `ZST_ERROR`.
- Coordinator exposes debug API:

```c
zst_dante_video_coordinator_get_tx_udp_sink(coordinator, flow_index)
```

Used by the test to print TX RTP stats.

### RX RTP receive path

Files:

- `src/dante_udp_source.c`
- `include/zstreamer/elements/zst_dante_udp_source.h`
- `src/dante_video_coordinator.c`
- `include/zstreamer/elements/zst_dante_video_coordinator.h`
- `tests/test_dante_av_txrx_integrity.c`

Implemented/fixed:

- `danteudpsrc` is a source element and must be driven by scheduler using `process(NULL, &out)`. It now polls/receives UDP on NULL input.
- Closed fd during teardown returns idle `ZST_OK` to avoid BUS ERR.
- RX RTP stats added in `danteudpsrc`:

```text
rtp-packets
rtp-lost
rtp-out-of-order
rtp-loss-rate-ppm
```

- Coordinator exposes debug API:

```c
zst_dante_video_coordinator_get_rx_udp_source(coordinator, flow_index)
```

Used by the test to print RX receive/loss stats.

### Shutdown deadlock avoidance

File:

- `src/dante_video_coordinator.c`

Implemented/fixed:

- Coordinator route cleanup is deferred if close occurs during pipeline state transition to avoid `pipeline_set_state(NULL)` deadlock.

## Remote validation results

A successful two-board run showed:

TX:

```text
[RTP TX flow 0] dst=192.168.190.101:16490 sent=780 (+140) err=0 (+0)
```

RX:

```text
[RTP RX flow 0] local=192.168.190.101:16490 from=192.168.190.99 received=1206 (+144) rejected=0 (+0) truncated=0 bytes=27488 ... rtp=1206 lost=0 ooo=0 loss=0.0000%
RX video sink:
  total-frames = 259
```

This confirms:

- TX RTP send works.
- RX RTP receive works.
- H.264 decode produces frames.
- RTP packet loss was zero in that run.

## Current test commands

Build ARM64 inside required Docker image:

```bash
docker run --entrypoint /bin/bash --rm \
  -e USER=root -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) \
  -v $(pwd):/workspace \
  qcap-build:xlnk2_arm64-base \
  -c 'source /opt/qcap-dev-init && cd /workspace/build-arm64 && make test_dante_av_txrx_integrity -j$(nproc)'
```

Manual remote run: start RX first, then TX.

RX:

```bash
ssh petalinux@sc6f0-060f00112336.local
cd /mnt/dev/zzlee/docker/zstreamer/build-arm64
./tests/test_dante_av_txrx_integrity --rx /var/run/dante/dvr DanteEP
```

TX:

```bash
ssh petalinux@sc6f0-060f00112334.local
cd /mnt/dev/zzlee/docker/zstreamer/build-arm64
./tests/test_dante_av_txrx_integrity --tx /var/run/dante/dvr DanteEP
```

Expected key TX lines:

```text
[BUS] TX video flow idx=0 ch=0 port=16490
[RTP TX] first packet sent!
[RTP TX flow 0] ... sent=<increasing> ... err=0
```

Expected key RX lines:

```text
[BUS] RX video flow idx=0 ch=0 port=16490
[RTP RX] first packet received!
[RTP RX flow 0] ... received=<increasing> rejected=0 truncated=0 ... lost=0 ... loss=0.0000%
RX video sink:
  total-frames = <increasing>
```

## Notes about logs/output

- Running through non-interactive SSH may buffer stdout. For live output, use `script`, e.g.:

```bash
ssh -x petalinux@sc6f0-060f00112336.local \
  "script -q -c 'cd /mnt/dev/zzlee/docker/zstreamer/build-arm64 && ./tests/test_dante_av_txrx_integrity --rx --log /tmp/rx.log /var/run/dante/dvr DanteEP' /tmp/rx.typescript"
```

- `timeout` command is not available on the Petalinux boards.
- `requestConfiguration` heartbeat from DVC arrives roughly once per second and is expected.

## Remaining work / next coding goal

Implement stronger code-level confirmation that RX media is correct and expose useful packet-loss statistics.

Current state:

- RX video correctness currently checks decoded frame count, geometry, keyframes, and mean luma.
- RX audio exact sine integrity is still present, but cross-device runs can start at arbitrary phase, so exact sample matching is not always a reliable pass/fail signal.
- Added audio signal metrics in test:

```text
signal-rms-pct
signal-peak-pct
signal-frequency-hz
signal-ok
```

Potential next improvements:

1. Improve RX audio frequency estimation.
   - Current positive zero-crossing estimation can be skewed by startup silence or buffering artifacts.
   - Consider windowed analysis after first non-silent sample, or autocorrelation/Goertzel around 440 Hz.

2. Improve RX video correctness.
   - `videotestsrc` pattern is deterministic. Add lightweight frame-content checks beyond mean luma, e.g. sample known Y plane points or hash selected regions.
   - Track decoded frame rate versus elapsed time.

3. Improve RTP loss stats.
   - `danteudpsrc` now tracks RTP seq gaps, out-of-order, and loss ppm.
   - Consider per-flow interval deltas and final summary in `test_dante_av_txrx_integrity`.

4. Add final pass/fail criteria for long-running TX/RX validation.
   - Video pass: frames decoded > threshold and dimensions match expected 640x480.
   - RTP pass: received > threshold, rejected/truncated == 0, loss below configured threshold.
   - Audio pass: RMS/peak/frequency within expected ranges.

## Important files

- `tests/test_dante_av_txrx_integrity.c` — main live two-board validation test.
- `src/dante_udp_source.c` — RX UDP receive and RTP loss stats.
- `src/dante_udp_sink.c` — TX UDP send and send stats.
- `src/dante_video_coordinator.c` — dynamic TX/RX video routes.
- `src/dante_protocol.c` — DVR JSON protocol parser.
- `src/dante_session.c` — DVR UNIX socket session and event posting.
- `include/zstreamer/elements/zst_dante_udp_source.h` — RX stat property names.
- `include/zstreamer/elements/zst_dante_video_coordinator.h` — coordinator debug APIs.
