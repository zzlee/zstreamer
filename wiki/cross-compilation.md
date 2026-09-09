# Cross-Compilation for ARM64 (Petalinux / Xilinx SC6f0)

This document describes how to cross-compile the `zstreamer` framework for ARM64 embedded platforms (specifically targeting Petalinux / Xilinx SC6f0 environments) using the cross-compilation toolchain.

## Cross-Compilation Toolchain

- **Base Docker Image:** `qcap-build:xlnk2_arm64-base` (containing an `amd64` development host with an `aarch64` cross-compiler SDK).
- **Environment Initialization:** Sourced via `/opt/qcap-dev-init` to set up paths to cross-compiler binaries (e.g., `aarch64-xilinx-linux-gcc`) and the target sysroot.
- **Build entry point:** `./scripts/build.sh xlnk2_arm64` (uses the base image directly; no zstreamer-specific xlnk2 Dockerfile is maintained).

## Handling Optional Dependencies

The SDK sysroot omits some optional dependencies, but the image includes prebuilt target packages under `/opt/qcap/qcap-3rdparty/xlnk2_arm64`. The cross Dockerfile adds its `lib/pkgconfig` directory to `PKG_CONFIG_PATH` and clears `PKG_CONFIG_SYSROOT_DIR`, so pkg-config retains the prebuilt packages' absolute `/opt/qcap` paths.

Available prebuilt packages include FFmpeg, x264, Freetype2, SRT, FDK-AAC, OpenSSL, Boost, and Allegro OMX. The target sysroot also provides ALSA and libv4l2. These multimedia archives are provided as **static-only** libraries (e.g. `libavcodec.a` exists, but there is **no** `libavcodec.so`). The archives are not PIC, which the linker confirms when pulling an archive member directly into a shared object (`dangerous relocation ... recompile with -fPIC`) — however the actual FFmpeg/x264 code linked into the plugins is embedded statically and the plugins run fine. FFmpeg/x264/SRT/Freetype plugins work either as part of `zstreamer-elements` (static) or as self-contained shared plugins. x265 and Vulkan remain unavailable in this SDK (json-c 0.17 is now provided).

### 1. Compile Guards and Defines

The build system automatically detects the availability of these packages via `pkg-config` and sets corresponding C preprocessor defines:

- `HAS_FFMPEG`: Set if FFmpeg libraries (libavformat, libavcodec, libavutil, libswscale, libswresample) are found.
- `HAS_X264`: Set if `x264` is found.
- `HAS_X265`: Set if `x265` is found.
- `HAS_ALSA`: Set if ALSA (`alsa`) is found.
- `HAS_V4L2`: Set if `libv4l2` is found.
- `HAS_FREETYPE`: Set if Freetype2 (`freetype2`) is found.
- `HAS_SRT`: Set if SRT (`srt` or `srt-gnutls`) is found.

### 2. Conditional Plugin Compilation

In `CMakeLists.txt`, plugin targets and source listings are dynamically appended only if their dependencies are met. Similarly, `src/zst_builtins.c` conditionally registers the element descriptors under the corresponding preprocessor guards.

### 3. Test Suite Adaptation

To ensure unit tests can compile and run on target configurations with missing libraries, unit tests in `tests/test_core.c` are guarded and skipped appropriately:

- **Optional elements integration:** Tests for optional plugins (e.g., MPEG-TS, MP4 demuxer, RTMP, RTSP, SRT, text overlay) are wrapped in their respective defines.
- **Skipped logs:** When dependencies are missing, the test suite runner outputs `[SKIP]` for the corresponding test groups.
- **Conditional binary building:** Binary examples (like `example_record` and `demo_colorbar_mp4`) are only built if all their required dependencies are available.

### 4. Dante Support

- `ENABLE_DANTE_DEP=ON` is enabled by `scripts/build.sh xlnk2_arm64` and cross-compiles on this AArch64 target. DEP uses only POSIX shared memory, semaphores, and the supported 64-bit ABI.
- `ENABLE_DANTE=ON` requires target-sysroot `json-c`. `json-c` 0.17 is now available under `/opt/qcap/qcap-3rdparty/xlnk2_arm64`, so **full Dante is cross-compiled on ARM64**: the DVR control/H.264 video coordinator, `demo_dante_av_tx`, and the `test_dante_*` mock tests all build and link successfully. json-c is statically embedded into the built binaries/plugins (0 undefined `json_*` at runtime).

## How to Build

Build the SDK directly against the cross-compilation base image:

```bash
./scripts/build.sh xlnk2_arm64
```

This produces user-owned, separate shared libraries in `build-xlnk2_arm64/`,
the directory qcap-demos links against.

> **Note:** `ENABLE_PLUGINS=ON` is fully supported in this SDK — the `.so` targets build, link, and (because FFmpeg/x264 are statically embedded) load on the device. It requires the vp8/vp9 encoder/decoder targets to be registered in `PLUGIN_TARGETS` so they receive the FFmpeg include paths. Only the ALSA/zlib/X11/libstdc++-dependent plugins require their respective runtime shared libraries, all of which the target sysroot provides. See [Packaging & Releasing](#packaging--releasing).

## How to Run Tests on Target Host

Since target binaries cannot be run directly inside the `amd64` container hosting the cross-compiler, they must be copied to the arm64 target device, or executed via `qemu-user-static` configuration on the host.

## Packaging & Releasing

When compiling with `ENABLE_PLUGINS=ON`, the shared `.so` plugin targets build and link successfully in this SDK. Because the linker is handed the static FFmpeg/x264 archives directly on the link line, the needed codecs are **statically embedded** into each FFmpeg/x264-backed plugin (e.g. a plugin `.so` that uses libavcodec is ~70 MB and defines symbols such as `avcodec_open2` in its own `.text`; x264 is embedded similarly). Consequently those FFmpeg/x264 plugins are **self-contained** and do **not** require a runtime `libavcodec.so`/`libx264.so` — they load and run as long as only glibc/math are needed.

The plugins that carry true external runtime dependencies are only the following, and each is satisfied by a shared library already present in the target sysroot:

| Plugin `.so` | External runtime dep | Present in sysroot |
|---|---|---|
| `libzst_alsasink.so`, `libzst_alsasource.so` | `libasound.so.2` (`snd_*`) | yes |
| `libzst_textoverlay.so`, `libzst_textsource.so` | zlib (`inflate*`) | yes (`libz.so`) |
| `libzst_x11sink.so` | `libX11.so.6` | yes |
| `libzst_srt.so` | libgcc_s + libstdc++ (C++ `_ZN*` symbols) | yes |

So with this SDK, `ENABLE_PLUGINS=ON` is buildable and its plugins load on the device (provided the above runtime libs are present, which the target sysroot provides). The fully-static `ENABLE_PLUGINS=OFF` configuration remains the simplest/safest for deployment, but it is no longer strictly required to work around PIC.

Package the SC6f0 SDK with the explicit `xlnk2_arm64` variant. The script
starts the toolchain container and configures the third-party pkg-config path
itself:

```bash
./scripts/package.sh <version> xlnk2_arm64
```

*(e.g., replacement for `<version>` could be `1.0.0`)*

### Output Artifacts
The script builds `build-xlnk2_arm64-static/` and the shared SDK in
`build-xlnk2_arm64/`, with `ENABLE_MONOLITHIC=OFF`, `ENABLE_PLUGINS=OFF`, and
Dante/DEP enabled by default. It separates core and element directories and
outputs the following files in `dist/`:
- `dist/zstreamer-<version>-linux-arm64.tar.gz` (and `.zip`)
- `dist/zstreamer-elements-<version>-linux-arm64.tar.gz` (and `.zip`)
- `dist/zstreamer-dev_<version>_arm64.deb`
- `dist/zstreamer-elements-dev_<version>_arm64.deb`
- `dist/zstreamer-debug-<version>-linux-arm64.tar.gz` (separate debug symbols; both packaging modes)

### P0: Stripped runtime libraries and separate debug symbols

`scripts/package.sh` installs unstripped files into temporary staging, then runs
`scripts/split-debug-symbols.sh` before creating runtime tar/zip/deb packages.
For each real ELF `.so` file (including versioned libraries and plugins), it:

1. Extracts symbols with `objcopy --only-keep-debug`.
2. Runs `strip --strip-unneeded` on the staged copy, not the build output.
3. Adds `.gnu_debuglink` linking the runtime library to its matching debug file.
4. Packages debug files separately, outside the runtime staging tree.

The tools come from `CMAKE_OBJCOPY` and `CMAKE_STRIP` in the build's
`CMakeCache.txt`. Missing tools abort packaging; there is no host-tool fallback
for cross builds. Shared-library symlinks and non-ELF linker scripts are preserved.
Static archives are outside this P0 change. The helper preserves debug information
already present in the build; it does not manufacture DWARF for builds without `-g`.

For the inspected xlnk2 ARM64 monolithic library, the original **122.44 MiB**
becomes approximately **23.60 MiB** with a debuglink. The SDK's inherited `-g`
and debug information in static third-party archives account for most of the
original size. This reduces deployed disk usage, not resident memory by the same
percentage; code/data and runtime dependencies are unchanged.

The debug archive mirrors the installation prefix, for example:

```text
lib/.debug/libzstreamer.so.0.1.0.debug
lib/zstreamer/plugins/.debug/libzst_h264decoder.so.debug
```

For debugging, extract the **matching build's** archive at the runtime library's
install prefix (or at the equivalent location in the host's target sysroot).
GDB can then find the adjacent `.debug/` files via `.gnu_debuglink` and its CRC.
For example, for libraries installed under `/usr/lib`:

```bash
tar -xzf zstreamer-debug-<version>-linux-arm64.tar.gz -C /usr
```

Keep the exact debug archive with every release; matching version strings alone
are insufficient when the same version is rebuilt. Debug symbols are not needed
for normal runtime deployment. Monolithic and plugin packages share one debug
archive, which covers all staged shared libraries.

#### Process an existing build without rebuilding

Use fresh, separate staging directories. Inside the SDK container:

```bash
mkdir -p /tmp/zst-runtime/lib /tmp/zst-debug
cp -a /workspace/build-xlnk2_arm64/src/libzstreamer.so* /tmp/zst-runtime/lib/
bash /workspace/scripts/split-debug-symbols.sh \
    /workspace/build-xlnk2_arm64 /tmp/zst-runtime /tmp/zst-debug
tar -czf /tmp/zstreamer-runtime-arm64.tar.gz -C /tmp/zst-runtime .
tar -czf /tmp/zstreamer-debug-arm64.tar.gz -C /tmp/zst-debug .
```

#### Validation (Docker only)

```bash
# Native fixture: load/call stripped .so, ELF sections, CRC, symlinks, plugins,
# debug archive layout, missing-tool failure, unchanged build artifact.
docker run --rm --entrypoint bash -v "$PWD":/workspace:ro zstreamer \
    /workspace/tests/test_split_debug_symbols.sh

# Real cross-compiled artifact: structural checks, no ARM execution.
docker run --rm --entrypoint bash -v "$PWD":/workspace:ro qcap-build:xlnk2_arm64 \
    /workspace/tests/test_split_debug_symbols.sh \
    /workspace/build-xlnk2_arm64 \
    /workspace/build-xlnk2_arm64/src/libzstreamer.so
```
