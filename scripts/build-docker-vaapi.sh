#!/usr/bin/env bash
# Convenience wrapper: build the vaapi builder image.
exec "$(dirname "$0")/build-docker.sh" vaapi "$@"
