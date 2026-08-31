#!/usr/bin/env bash
set -e

# Usage: ./package.sh <version>
# Set MONOLITHIC=1 to build a single libzstreamer.so (core + elements).
VERSION="${1:-0.1.0}"
# Strip leading 'v' if present for debian package compatibility
DEB_VERSION="${VERSION#v}"

# Monolithic mode: single .so with all elements (OFF by default)
MONOLITHIC="${MONOLITHIC:-0}"

# Detect target architecture
TARGET_ARCH="x86_64"
DEB_ARCH="amd64"
if [[ "$ARCH" == "arm64" || "$OECORE_TARGET_ARCH" == "aarch64" ]]; then
    TARGET_ARCH="arm64"
    DEB_ARCH="arm64"
fi

# ── Cross-compilation environment setup ──────────────────────────────────
# If SDKTARGETSYSROOT is already set (e.g. after sourcing /opt/qcap-dev-init),
# ensure PKG_CONFIG_PATH includes both qcap 3rd-party and sysroot packages.
# Unset PKG_CONFIG_SYSROOT_DIR because qcap .pc files use absolute paths;
# the cross-compiler's --sysroot flag handles library resolution.
if [ -n "${SDKTARGETSYSROOT:-}" ]; then
    unset PKG_CONFIG_SYSROOT_DIR
    QCAP_PKGCONFIG="/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig"
    SYSROOT_PKGCONFIG="${SDKTARGETSYSROOT}/usr/lib/pkgconfig"
    export PKG_CONFIG_PATH="${QCAP_PKGCONFIG}:${SYSROOT_PKGCONFIG}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    echo "Cross-compilation: PKG_CONFIG_PATH set for qcap + sysroot packages"
fi

# ── Monolithic mode setup ────────────────────────────────────────────────
if [ "$MONOLITHIC" = "1" ]; then
    echo ">>> MONOLITHIC MODE: building single libzstreamer.so (core + elements)"
    PLUGINS_FLAG="OFF"
    MONOLITHIC_FLAG="ON"
else
    echo ">>> PLUGIN MODE: building separate plugin .so files"
    PLUGINS_FLAG="ON"
    MONOLITHIC_FLAG="OFF"
fi

echo "=== Packaging zstreamer v${VERSION} (Debian version: ${DEB_VERSION}, Arch: ${TARGET_ARCH}/${DEB_ARCH}) ==="

# Directories
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_STATIC="${PROJECT_ROOT}/build-static"
BUILD_SHARED="${PROJECT_ROOT}/build-shared"
STAGE_ALL="${PROJECT_ROOT}/zstreamer-stage-all"
STAGE_ZSTREAMER="${PROJECT_ROOT}/zstreamer-release-stage"
STAGE_ELEMENTS="${PROJECT_ROOT}/zstreamer-elements-release-stage"
DEB_STAGE_ZSTREAMER="${PROJECT_ROOT}/zstreamer-deb-stage"
DEB_STAGE_ELEMENTS="${PROJECT_ROOT}/zstreamer-elements-deb-stage"
OUTPUT_DIR="${PROJECT_ROOT}/dist"

# Clean previous build/dist artifacts
rm -rf "$BUILD_STATIC" "$BUILD_SHARED" "$STAGE_ALL" "$STAGE_ZSTREAMER" "$STAGE_ELEMENTS" "$DEB_STAGE_ZSTREAMER" "$DEB_STAGE_ELEMENTS" "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# 1. Build Static Libraries
echo "--> Configuring and building static libraries..."
cmake -B "$BUILD_STATIC" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED=OFF \
    -DBUILD_TESTS=OFF \
    -DENABLE_PLUGINS="$PLUGINS_FLAG" \
    -DENABLE_MONOLITHIC="$MONOLITHIC_FLAG"
cmake --build "$BUILD_STATIC" -j$(nproc)

# 2. Build Shared Libraries
echo "--> Configuring and building shared libraries..."
cmake -B "$BUILD_SHARED" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED=ON \
    -DBUILD_TESTS=OFF \
    -DENABLE_PLUGINS="$PLUGINS_FLAG" \
    -DENABLE_MONOLITHIC="$MONOLITHIC_FLAG"
cmake --build "$BUILD_SHARED" -j$(nproc)

# 3. Stage All Files Temporarily
echo "--> Staging all files..."
DESTDIR="" cmake --install "$BUILD_SHARED" --prefix "$STAGE_ALL"

if [ "$MONOLITHIC" = "1" ]; then
    # ── Monolithic staging ──────────────────────────────────────────────
    echo "--> Staging monolithic build..."

    # Single package: everything in zstreamer/
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/include/zstreamer"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/elements"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer"

    # The monolithic .so IS libzstreamer.so (OUTPUT_NAME=zstreamer)
    cp -a "$STAGE_ALL/lib"/libzstreamer.so* "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$BUILD_STATIC/libzstreamer.a" "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/pkgconfig/zstreamer.pc" "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig/" 2>/dev/null || true
    cp -a "$STAGE_ALL/include/zstreamer"/*.h "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/" 2>/dev/null || true
    cp -a "$STAGE_ALL/include/zstreamer/elements"/*.h "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/elements/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/cmake/zstreamer"/* "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer/" 2>/dev/null || true

    # 4. Generate Tarballs and Zips (monolithic: single archive)
    echo "--> Generating monolithic archives..."
    tar -czf "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}.tar.gz" -C "$STAGE_ZSTREAMER" zstreamer

    if command -v zip >/dev/null 2>&1; then
        (cd "$STAGE_ZSTREAMER" && zip -r "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}.zip" zstreamer)
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c "import shutil; shutil.make_archive('${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}', 'zip', '$STAGE_ZSTREAMER', 'zstreamer')"
    else
        echo "Warning: zip and python3 not found. Skipping zip archive."
    fi

    # 5. Create Debian Package (monolithic: single .deb)
    echo "--> Creating monolithic Debian package..."
    mkdir -p "$DEB_STAGE_ZSTREAMER/usr"
    cp -a "$STAGE_ZSTREAMER/zstreamer"/* "$DEB_STAGE_ZSTREAMER/usr/"

    mkdir -p "$DEB_STAGE_ZSTREAMER/DEBIAN"
    cat << EOF2 > "$DEB_STAGE_ZSTREAMER/DEBIAN/control"
Package: zstreamer-dev
Version: ${DEB_VERSION}
Section: devel
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: zzlee <zzlee@github.com>
Depends: libvulkan-dev
Description: Lightweight modular multimedia streaming framework (monolithic)
 zstreamer is a GStreamer-like C11 library featuring a pipeline architecture.
 This monolithic package contains core + all elements in a single libzstreamer.so.
EOF2

    dpkg-deb --build "$DEB_STAGE_ZSTREAMER" "${OUTPUT_DIR}/zstreamer-dev_${DEB_VERSION}_${DEB_ARCH}.deb"

else
    # ── Plugin staging (original behavior) ──────────────────────────────
    echo "--> Staging plugin build..."

    # 4. Split Tarball/Zip Staging
    echo "--> Splitting staging directory for archives..."
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/include/zstreamer"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer"

    mkdir -p "$STAGE_ELEMENTS/zstreamer-elements/lib/pkgconfig"
    mkdir -p "$STAGE_ELEMENTS/zstreamer-elements/include/zstreamer/elements"
    mkdir -p "$STAGE_ELEMENTS/zstreamer-elements/lib/zstreamer/plugins"

    # Core zstreamer
    cp -a "$STAGE_ALL/lib"/libzstreamer.so* "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$BUILD_STATIC/libzstreamer.a" "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/pkgconfig/zstreamer.pc" "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig/" 2>/dev/null || true
    cp -a "$STAGE_ALL/include/zstreamer"/*.h "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/cmake/zstreamer"/* "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer/" 2>/dev/null || true

    # zstreamer-elements
    cp -a "$STAGE_ALL/lib"/libzstreamer-elements.so* "$STAGE_ELEMENTS/zstreamer-elements/lib/" 2>/dev/null || true
    cp -a "$BUILD_STATIC/libzstreamer-elements.a" "$STAGE_ELEMENTS/zstreamer-elements/lib/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/pkgconfig/zstreamer-elements.pc" "$STAGE_ELEMENTS/zstreamer-elements/lib/pkgconfig/" 2>/dev/null || true
    cp -a "$STAGE_ALL/include/zstreamer/elements"/*.h "$STAGE_ELEMENTS/zstreamer-elements/include/zstreamer/elements/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/zstreamer/plugins" "$STAGE_ELEMENTS/zstreamer-elements/lib/zstreamer/" 2>/dev/null || true

    # 5. Generate Tarballs and Zips
    echo "--> Generating archives..."
    tar -czf "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}.tar.gz" -C "$STAGE_ZSTREAMER" zstreamer
    tar -czf "${OUTPUT_DIR}/zstreamer-elements-${VERSION}-linux-${TARGET_ARCH}.tar.gz" -C "$STAGE_ELEMENTS" zstreamer-elements

    if command -v zip >/dev/null 2>&1; then
        (cd "$STAGE_ZSTREAMER" && zip -r "${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}.zip" zstreamer)
        (cd "$STAGE_ELEMENTS" && zip -r "${OUTPUT_DIR}/zstreamer-elements-${VERSION}-linux-${TARGET_ARCH}.zip" zstreamer-elements)
    elif command -v python3 >/dev/null 2>&1; then
        echo "zip not found, using python3 to create zip archive..."
        python3 -c "import shutil; shutil.make_archive('${OUTPUT_DIR}/zstreamer-${VERSION}-linux-${TARGET_ARCH}', 'zip', '$STAGE_ZSTREAMER', 'zstreamer')"
        python3 -c "import shutil; shutil.make_archive('${OUTPUT_DIR}/zstreamer-elements-${VERSION}-linux-${TARGET_ARCH}', 'zip', '$STAGE_ELEMENTS', 'zstreamer-elements')"
    else
        echo "Warning: zip command and python3 not found. Skipping zip archive generation."
    fi

    # 6. Create Debian Package Staging
    echo "--> Creating staging directories for Debian packages..."
    mkdir -p "$DEB_STAGE_ZSTREAMER/usr"
    cp -a "$STAGE_ZSTREAMER/zstreamer"/* "$DEB_STAGE_ZSTREAMER/usr/"

    mkdir -p "$DEB_STAGE_ELEMENTS/usr"
    cp -a "$STAGE_ELEMENTS/zstreamer-elements"/* "$DEB_STAGE_ELEMENTS/usr/"

    # Create DEBIAN control file for zstreamer-dev
    mkdir -p "$DEB_STAGE_ZSTREAMER/DEBIAN"
    cat << EOF2 > "$DEB_STAGE_ZSTREAMER/DEBIAN/control"
Package: zstreamer-dev
Version: ${DEB_VERSION}
Section: devel
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: zzlee <zzlee@github.com>
Depends: libvulkan-dev
Description: Lightweight modular multimedia streaming framework - Core
 zstreamer is a GStreamer-like C11 library featuring a pipeline architecture,
 elements connected via pads, data flowing as reference-counted buffers through
 thread-safe queues, driven by a configurable scheduler.
 This package contains the core development headers, static libraries, and shared libraries.
EOF2

    # Create DEBIAN control file for zstreamer-elements-dev
    mkdir -p "$DEB_STAGE_ELEMENTS/DEBIAN"
    cat << EOF2 > "$DEB_STAGE_ELEMENTS/DEBIAN/control"
Package: zstreamer-elements-dev
Version: ${DEB_VERSION}
Section: devel
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: zzlee <zzlee@github.com>
Depends: zstreamer-dev (= ${DEB_VERSION}), libavformat-dev, libavcodec-dev, libavutil-dev, libx264-dev, libx265-dev, libasound2-dev, libv4l-dev, libswscale-dev, libswresample-dev, libfreetype-dev, libsrt-gnutls-dev, libx11-dev, libxext-dev, libgl1-mesa-dev, libglu1-mesa-dev, mesa-common-dev
Description: Lightweight modular multimedia streaming framework - Elements
 This package contains development headers, static libraries, shared libraries, and plugins
 for zstreamer elements.
EOF2

    # Build Debian packages
    echo "--> Generating Debian packages..."
    dpkg-deb --build "$DEB_STAGE_ZSTREAMER" "${OUTPUT_DIR}/zstreamer-dev_${DEB_VERSION}_${DEB_ARCH}.deb"
    dpkg-deb --build "$DEB_STAGE_ELEMENTS" "${OUTPUT_DIR}/zstreamer-elements-dev_${DEB_VERSION}_${DEB_ARCH}.deb"
fi

echo "=== Packaging Completed! Output files in ${OUTPUT_DIR}: ==="
ls -lh "$OUTPUT_DIR"
