#!/usr/bin/env bash
# Build the zstreamer SDK inside a builder Docker image.
#
# Usage:
#   ./scripts/build.sh <variant> [cmake args ...]
#
# Variants:
#   xlnk2_arm64   Cross-compile for Xilinx SC6f0 (Petalinux) using the
#                 qcap-build:xlnk2_arm64-base image. Output: build-xlnk2_arm64/
#                 (the directory qcap-demos links against).
#   dev/gl/vaapi/oneapi/jetson/st2110
#                 Build using the zstreamer-build:<variant> image that was
#                 created by scripts/build-docker.sh (native target).
#
# Env overrides:
#   ZSTREAMER_BUILD_DIR   output build directory (default: build-xlnk2_arm64 for
#                         xlnk2_arm64, build/ otherwise)
#   BUILD_SHARED          ON/OFF (default ON, separate libzstreamer.so +
#                         libzstreamer-elements.so; OFF produces static .a)
#   ENABLE_MONOLITHIC     ON/OFF (default OFF)
#   ENABLE_DANTE          ON/OFF (default ON)
#   ENABLE_DANTE_DEP      ON/OFF (default ON)
#   CMAKE_BUILD_TYPE      (default Release)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

VARIANT="${1:-}"
[[ -z "${VARIANT}" ]] && usage
shift || true

# ── Common build configuration ───────────────────────────────────────────
BUILD_DIR="${ZSTREAMER_BUILD_DIR:-}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
BUILD_SHARED="${BUILD_SHARED:-ON}"
ENABLE_MONOLITHIC="${ENABLE_MONOLITHIC:-OFF}"
ENABLE_DANTE="${ENABLE_DANTE:-ON}"
ENABLE_DANTE_DEP="${ENABLE_DANTE_DEP:-ON}"

COMMON_ARGS=(
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    -DBUILD_TESTS=OFF
    -DBUILD_SHARED="${BUILD_SHARED}"
    -DENABLE_MONOLITHIC="${ENABLE_MONOLITHIC}"
    -DENABLE_DANTE="${ENABLE_DANTE}"
    -DENABLE_DANTE_DEP="${ENABLE_DANTE_DEP}"
)

build_xlnk2_arm64() {
    BUILD_DIR="${BUILD_DIR:-build-xlnk2_arm64}"
    IMAGE="${QCAP_BUILD_IMAGE:-qcap-build:xlnk2_arm64-base}"

    EXTRA=(
        -DENABLE_PLUGINS=OFF
        -DCMAKE_PREFIX_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
    )

    # Cross-compile with the qcap toolchain; artifacts stay user-owned so a
    # subsequent incremental build does not require root.
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME="${HOME}" \
        -v "${REPO_ROOT}:/workspace" \
        -w /workspace \
        "${IMAGE}" \
        bash -lc "
            source /opt/qcap-dev-init &&
            unset PKG_CONFIG_SYSROOT_DIR &&
            export PKG_CONFIG_PATH=/opt/qcap/qcap-3rdparty/xlnk2_arm64/lib/pkgconfig:\${SDKTARGETSYSROOT}/usr/lib/pkgconfig &&
            cmake -B ${BUILD_DIR} -S . \
                ${COMMON_ARGS[*]} \
                ${EXTRA[*]} \
                \"\$@\" &&
            cmake --build ${BUILD_DIR} --parallel \$(nproc)
        " bash "$@"
}

build_native() {
    BUILD_DIR="${BUILD_DIR:-build}"
    IMAGE="zstreamer-build:${VARIANT}"

    docker run --rm \
        -v "${REPO_ROOT}:/workspace" \
        -w /workspace \
        "${IMAGE}" \
        bash -lc "
            cmake -B ${BUILD_DIR} -S . ${COMMON_ARGS[*]} \"\$@\" &&
            cmake --build ${BUILD_DIR} --parallel \$(nproc)
        " bash "$@"
}

case "${VARIANT}" in
    xlnk2_arm64)  build_xlnk2_arm64 ;;
    dev|gl|vaapi|oneapi|jetson|st2110)  build_native ;;
    *)
        echo "Error: unknown variant '${VARIANT}'" >&2
        usage
        ;;
esac