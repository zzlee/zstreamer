# AAC SIMD regression test

`tests/test_aac_simd.c` checks PCM correctness, not merely successful decoding.
It requires FFmpeg and is registered with CTest as `test_aac_simd` when
`BUILD_TESTS=ON`. No RTSP server, external media, audio device, or root privileges
are needed.

## Method

- Generate 32 frames of deterministic stereo tones, at 48 kHz and 44.1 kHz.
- Encode once with scalar FFmpeg and frame the resulting AAC-LC packets as ADTS.
- Feed identical packets through zstreamer's AAC decoder using:
  - native `aac` with CPU flags disabled (reference);
  - native `aac` with automatic CPU flags (SIMD under test);
  - `aac_fixed` with automatic CPU flags (RX workaround);
  - on ARM with NEON available, native `aac` with only NEON disabled.
- Compare interleaved, normalized PCM using RMSE and peak absolute error.
  Also check sample counts, rate, channels, finite samples, and non-silent output.

Native/scalar tolerance is RMSE `1e-5`, peak error `1e-4`. Fixed/scalar tolerance
is RMSE `1e-4`, peak error `1e-3`, allowing fixed-point rounding differences.
Values are relative to full-scale normalized PCM, not integer sample units.

The encoder's priming/tail packets are not used; every retained packet must
produce a complete 1024-sample decoded frame in every backend.

CPU flags are process-global. This test changes them serially inside its own
process; never embed it in a running streaming application. A passing fixed-point
comparison does **not** turn a failing SIMD comparison into success.

## Build and run on SC6F0 RX

From the zstreamer repository, configure tests using the normal xlnk2_arm64
builder, then build only this target (does not build all tests):

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/workspace" -w /workspace \
  qcap-build:xlnk2_arm64-base bash -lc '
    source /opt/qcap-dev-init &&
    unset PKG_CONFIG_SYSROOT_DIR &&
    export PKG_CONFIG_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig:${SDKTARGETSYSROOT}/usr/lib/pkgconfig &&
    cmake -S . -B build-xlnk2_arm64 \
      -DBUILD_TESTS=ON -DBUILD_SHARED=ON -DENABLE_MONOLITHIC=OFF \
      -DENABLE_PLUGINS=OFF -DENABLE_DANTE=ON -DENABLE_DANTE_DEP=ON \
      -DCMAKE_PREFIX_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64 \
      -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
      -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
      -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH &&
    cmake --build build-xlnk2_arm64 --target test_aac_simd --parallel 4
  '

ssh petalinux@sc6f0-060f00112336.local '
  env LD_LIBRARY_PATH=/mnt/dev/zzlee/docker/zstreamer/build-xlnk2_arm64/src:/opt/qcap/lib \
    /mnt/dev/zzlee/docker/zstreamer/build-xlnk2_arm64/tests/test_aac_simd
'
```

For a native build inside the zstreamer Docker builder, run:

```bash
ctest --test-dir <native-build-directory> -R '^test_aac_simd$' -V
```

## Observed results

On SC6F0 RX with the current xlnk2_arm64 FFmpeg payload
(`avcodec=59.18.100`, `avutil=57.17.100`, auto CPU flags `0x68`):

| Rate | Auto vs scalar RMSE | Fixed vs scalar RMSE | No-NEON vs scalar RMSE |
| --- | --- | --- | --- |
| 48000 | 0.284744 (FAIL) | 0.00001527 (PASS) | 0 (PASS) |
| 44100 | 0.271331 (FAIL) | 0.00001527 (PASS) | 0 (PASS) |

Native x86 Docker validation passes. RX exits **1**, intentionally detecting the
unresolved NEON defect in the bundled FFmpeg. Setup, format, and structural
failures exit **2**. A corrected FFmpeg payload should make all comparisons pass
and exit **0** without changing test thresholds or marking it expected-failure.

This narrows the defect to the NEON-enabled decode path, but does not identify
the exact FFmpeg routine or prove a defect in all upstream FFmpeg versions.
