#!/usr/bin/env bash
# Convenience wrapper: build the jetson builder image.
exec "$(dirname "$0")/build-docker.sh" jetson "$@"
