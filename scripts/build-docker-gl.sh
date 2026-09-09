#!/usr/bin/env bash
# Convenience wrapper: build the gl builder image.
exec "$(dirname "$0")/build-docker.sh" gl "$@"
