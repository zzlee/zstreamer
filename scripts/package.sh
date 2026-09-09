#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/package.sh [version] [native|xlnk2_arm64]
# The xlnk2_arm64 variant runs in the qcap toolchain container and follows the
# same defaults as build.sh: shared, separate core/elements, Dante enabled, and
# plugins disabled.
# Directories
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == "native" || "${1:-}" == "xlnk2_arm64" ]]; then
    VERSION="$("${PROJECT_ROOT}/scripts/version.sh" get)"
    VARIANT="$1"
else
    VERSION="${1:-$("${PROJECT_ROOT}/scripts/version.sh" get)}"
    VARIANT="${2:-${ZSTREAMER_VARIANT:-native}}"
fi
# Strip leading 'v' if present for debian package compatibility
DEB_VERSION="${VERSION#v}"

if [[ "$VARIANT" == "xlnk2_arm64" && -z "${ZSTREAMER_PACKAGE_IN_CONTAINER:-}" && -z "${SDKTARGETSYSROOT:-}" ]]; then
    exec docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME="${HOME}" \
        -e ZSTREAMER_PACKAGE_IN_CONTAINER=1 \
        -v "${PROJECT_ROOT}:/workspace" \
        -w /workspace \
        "${QCAP_BUILD_IMAGE:-qcap-build:xlnk2_arm64-base}" \
        bash -lc 'source /opt/qcap-dev-init && exec ./scripts/package.sh "$@"' \
        bash "$VERSION" "$VARIANT"
fi

# MONOLITHIC=1 is retained as a compatibility alias for the build.sh option.
if [[ -n "${MONOLITHIC:-}" && -z "${ENABLE_MONOLITHIC:-}" ]]; then
    ENABLE_MONOLITHIC="$([[ "$MONOLITHIC" == "1" ]] && printf ON || printf OFF)"
fi
ENABLE_MONOLITHIC="${ENABLE_MONOLITHIC:-OFF}"
ENABLE_DANTE="${ENABLE_DANTE:-ON}"
ENABLE_DANTE_DEP="${ENABLE_DANTE_DEP:-ON}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"

case "$VARIANT" in
    native)
        TARGET_ARCH="x86_64"
        DEB_ARCH="amd64"
        BUILD_STATIC="${PROJECT_ROOT}/build-package-static"
        BUILD_SHARED="${PROJECT_ROOT}/build-package-shared"
        ENABLE_PLUGINS="${ENABLE_PLUGINS:-ON}"
        PLATFORM_ARGS=()
        ;;
    xlnk2_arm64)
        TARGET_ARCH="arm64"
        DEB_ARCH="arm64"
        BUILD_STATIC="${PROJECT_ROOT}/build-xlnk2_arm64-static"
        BUILD_SHARED="${PROJECT_ROOT}/build-xlnk2_arm64"
        ENABLE_PLUGINS="${ENABLE_PLUGINS:-OFF}"
        unset PKG_CONFIG_SYSROOT_DIR
        export PKG_CONFIG_PATH="/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig:${SDKTARGETSYSROOT:?source /opt/qcap-dev-init first}/usr/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        PLATFORM_ARGS=(
            -DCMAKE_PREFIX_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64
            -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH
            -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
        )
        ;;
    *)
        echo "Error: unknown package variant '${VARIANT}' (expected native or xlnk2_arm64)" >&2
        exit 1
        ;;
esac

COMMON_ARGS=(
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    -DBUILD_TESTS=OFF
    -DENABLE_PLUGINS="${ENABLE_PLUGINS}"
    -DENABLE_MONOLITHIC="${ENABLE_MONOLITHIC}"
    -DENABLE_DANTE="${ENABLE_DANTE}"
    -DENABLE_DANTE_DEP="${ENABLE_DANTE_DEP}"
)

echo "=== Packaging zstreamer v${VERSION} (${VARIANT}, ${TARGET_ARCH}/${DEB_ARCH}) ==="
echo "    shared: ${BUILD_SHARED#$PROJECT_ROOT/}"
echo "    static: ${BUILD_STATIC#$PROJECT_ROOT/}"
echo "    BUILD_SHARED=ON ENABLE_MONOLITHIC=${ENABLE_MONOLITHIC} ENABLE_PLUGINS=${ENABLE_PLUGINS} ENABLE_DANTE=${ENABLE_DANTE} ENABLE_DANTE_DEP=${ENABLE_DANTE_DEP}"

STAGE_ALL="${PROJECT_ROOT}/zstreamer-stage-all"
STAGE_ZSTREAMER="${PROJECT_ROOT}/zstreamer-release-stage"
STAGE_ELEMENTS="${PROJECT_ROOT}/zstreamer-elements-release-stage"
DEB_STAGE_ZSTREAMER="${PROJECT_ROOT}/zstreamer-deb-stage"
DEB_STAGE_ELEMENTS="${PROJECT_ROOT}/zstreamer-elements-deb-stage"
OUTPUT_DIR="${PROJECT_ROOT}/dist"
STAGE_DEBUG="${PROJECT_ROOT}/zstreamer-debug-stage"

# Clean previous build/dist artifacts
rm -rf "$BUILD_STATIC" "$BUILD_SHARED" "$STAGE_ALL" "$STAGE_ZSTREAMER" "$STAGE_ELEMENTS" "$DEB_STAGE_ZSTREAMER" "$DEB_STAGE_ELEMENTS" "$OUTPUT_DIR" "$STAGE_DEBUG"
mkdir -p "$OUTPUT_DIR"

# 1. Build Static Libraries
echo "--> Configuring and building static libraries..."
cmake -B "$BUILD_STATIC" -S "$PROJECT_ROOT" \
    -DBUILD_SHARED=OFF \
    "${COMMON_ARGS[@]}" \
    "${PLATFORM_ARGS[@]}"
cmake --build "$BUILD_STATIC" --parallel "$(nproc)"

# 2. Build Shared Libraries
echo "--> Configuring and building shared libraries..."
cmake -B "$BUILD_SHARED" -S "$PROJECT_ROOT" \
    -DBUILD_SHARED=ON \
    "${COMMON_ARGS[@]}" \
    "${PLATFORM_ARGS[@]}"
cmake --build "$BUILD_SHARED" --parallel "$(nproc)"

# 3. Stage All Files Temporarily
echo "--> Staging all files..."
# Install unstripped first: debug symbols must be extracted before stripping.
DESTDIR="" cmake --install "$BUILD_SHARED" --prefix "$STAGE_ALL"
bash "$PROJECT_ROOT/scripts/split-debug-symbols.sh" "$BUILD_SHARED" "$STAGE_ALL" "$STAGE_DEBUG"
# Separate archive for both monolithic and plugin modes. Extract at the same
# install prefix as the runtime libraries to place symbols in adjacent .debug/.
tar -czf "${OUTPUT_DIR}/zstreamer-debug-${VERSION}-linux-${TARGET_ARCH}.tar.gz" -C "$STAGE_DEBUG" .

if [ "$ENABLE_MONOLITHIC" = "ON" ]; then
    # ── Monolithic staging ──────────────────────────────────────────────
    echo "--> Staging monolithic build..."

    # Single package: everything in zstreamer/
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/include/zstreamer"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/elements"
    mkdir -p "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer"

    # The monolithic .so IS libzstreamer.so (OUTPUT_NAME=zstreamer)
    cp -a "$STAGE_ALL/lib"/libzstreamer.so* "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$BUILD_STATIC/src/libzstreamer.a" "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
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
    # ── Separate core/elements staging ───────────────────────────────────
    echo "--> Staging separate core/elements libraries..."

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
    cp -a "$BUILD_STATIC/src/libzstreamer.a" "$STAGE_ZSTREAMER/zstreamer/lib/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/pkgconfig/zstreamer.pc" "$STAGE_ZSTREAMER/zstreamer/lib/pkgconfig/" 2>/dev/null || true
    cp -a "$STAGE_ALL/include/zstreamer"/*.h "$STAGE_ZSTREAMER/zstreamer/include/zstreamer/" 2>/dev/null || true
    cp -a "$STAGE_ALL/lib/cmake/zstreamer"/* "$STAGE_ZSTREAMER/zstreamer/lib/cmake/zstreamer/" 2>/dev/null || true

    # zstreamer-elements
    cp -a "$STAGE_ALL/lib"/libzstreamer-elements.so* "$STAGE_ELEMENTS/zstreamer-elements/lib/" 2>/dev/null || true
    cp -a "$BUILD_STATIC/src/libzstreamer-elements.a" "$STAGE_ELEMENTS/zstreamer-elements/lib/" 2>/dev/null || true
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
