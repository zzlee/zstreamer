#!/usr/bin/env bash
# Convenience wrapper: build the st2110 builder image.
exec "$(dirname "$0")/build-docker.sh" st2110 "$@"
