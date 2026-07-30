#!/usr/bin/env bash
set -euo pipefail

IMAGE=${ZSTREAMER_IMAGE:-zstreamer:latest}
FRAMES=${ZST_V4L2_DMABUF_FRAMES:-10}
DURATION=${ZST_V4L2_DMABUF_DURATION:-5}
WIDTH=${ZST_V4L2_DMABUF_WIDTH:-640}
HEIGHT=${ZST_V4L2_DMABUF_HEIGHT:-480}
REQUIRE=${ZST_V4L2_DMABUF_REQUIRE:-0}

is_root() {
    [ "${EUID:-$(id -u)}" -eq 0 ]
}

run_modprobe() {
    if is_root; then
        modprobe "$@"
    else
        sudo modprobe "$@"
    fi
}

skip_or_fail() {
    local msg="$1"
    if [ "$REQUIRE" = "1" ]; then
        echo "ERROR: $msg" >&2
        exit 1
    fi
    echo "SKIP: $msg"
    exit 0
}

find_vivid_device() {
    for node in /sys/class/video4linux/video*; do
        [ -e "$node/name" ] || continue
        if grep -qi "vivid" "$node/name"; then
            basename "$node" | sed 's#^#/dev/#'
            return 0
        fi
    done
    return 1
}

DEVICE=$(find_vivid_device || true)
if [ -z "$DEVICE" ] || [ ! -c "$DEVICE" ]; then
    echo "Loading vivid virtual V4L2 capture driver..."
    run_modprobe vivid n_devs=1 node_types=0x1 || skip_or_fail "unable to load vivid kernel module"
    DEVICE=$(find_vivid_device || true)
fi

if [ -z "$DEVICE" ] || [ ! -c "$DEVICE" ]; then
    skip_or_fail "no vivid /dev/videoX capture device found"
fi

if [ ! -d /dev/dma_heap ] && [ ! -e /dev/udmabuf ]; then
    if [ "${ZST_V4L2_DMABUF_ALLOW_MEMFD:-0}" != "1" ]; then
        skip_or_fail "no /dev/dma_heap or /dev/udmabuf available for importable DMABUF allocation"
    fi
    echo "WARNING: using memfd-backed DMABUF simulation; some V4L2 drivers reject memfd fds."
fi

DOCKER_ARGS=(--rm --privileged --device "$DEVICE:$DEVICE")
if [ -d /dev/dma_heap ]; then
    DOCKER_ARGS+=(-v /dev/dma_heap:/dev/dma_heap:rw)
fi
if [ -e /dev/udmabuf ]; then
    DOCKER_ARGS+=(--device /dev/udmabuf:/dev/udmabuf)
fi

if [ -n "${ZST_DMABUF_HEAP:-}" ]; then
    DOCKER_ARGS+=(-e "ZST_DMABUF_HEAP=$ZST_DMABUF_HEAP")
fi

echo "Running V4L2 DMABUF simulation against $DEVICE in $IMAGE..."
docker run "${DOCKER_ARGS[@]}" "$IMAGE" \
    /workspace/build/tests/test_v4l2_dmabuf_sim \
    --device "$DEVICE" \
    --width "$WIDTH" \
    --height "$HEIGHT" \
    --frames "$FRAMES" \
    --duration "$DURATION"
