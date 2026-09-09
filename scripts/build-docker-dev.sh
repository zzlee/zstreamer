#!/usr/bin/env bash
# Convenience wrapper: build the dev builder image.
exec "$(dirname "$0")/build-docker.sh" dev "$@"
