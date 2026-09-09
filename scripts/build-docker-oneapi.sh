#!/usr/bin/env bash
# Convenience wrapper: build the oneapi builder image.
exec "$(dirname "$0")/build-docker.sh" oneapi "$@"
