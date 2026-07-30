#!/usr/bin/env bash
set -e

# Detect if running in sudo/root
IS_ROOT=false
if [ "$EUID" -eq 0 ]; then
    IS_ROOT=true
fi

# Function to check if v4l2loopback is loaded
is_loaded() {
    lsmod | grep -q "^v4l2loopback\s"
}

# Try loading the module if not loaded
if ! is_loaded; then
    echo "v4l2loopback module not loaded."
    if [ "$IS_ROOT" = true ]; then
        echo "Loading v4l2loopback..."
        modprobe v4l2loopback devices=1 video_nr=10 card_label="ZStreamer-Loopback" exclusive_caps=1 || true
    else
        echo "Attempting to load v4l2loopback with sudo..."
        sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="ZStreamer-Loopback" exclusive_caps=1 || true
    fi
else
    echo "v4l2loopback is already loaded."
fi

# Ensure device /dev/video10 exists
if [ ! -c /dev/video10 ]; then
    echo "ERROR: /dev/video10 device node not found. Integration test cannot run without a loopback device node."
    exit 1
fi

echo "Starting V4L2 Producer container..."
# Run producer in background
docker run --rm -d \
  --name zstreamer-producer \
  --device /dev/video10:/dev/video10 \
  --cap-add=SYS_ADMIN \
  zstreamer \
  /workspace/build/test_v4l2_loopback --device /dev/video10 --mode write --frames 100 --duration 10

# Helper to stop producer on cleanup
cleanup() {
    echo "Cleaning up containers..."
    docker stop zstreamer-producer >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "Starting V4L2 Consumer container..."
# Run consumer in foreground
EXIT_CODE=0
docker run --rm \
  --name zstreamer-consumer \
  --device /dev/video10:/dev/video10 \
  zstreamer \
  bash -c "sleep 2 && /workspace/build/test_v4l2_loopback --device /dev/video10 --mode read --frames 50 --duration 10" || EXIT_CODE=$?

if [ $EXIT_CODE -eq 0 ]; then
    echo "Integration tests passed successfully!"
else
    echo "Integration tests failed with exit code $EXIT_CODE"
fi

exit $EXIT_CODE
