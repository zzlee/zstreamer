#!/usr/bin/env bash
# Build a zstreamer builder Docker image.
#
# Usage: ./scripts/build-docker.sh <variant> [docker build args ...]
#
# Variants map to a Dockerfile in docker/ and an image tag of the form
# zstreamer-build:<variant>:
#   dev            docker/Dockerfile           zstreamer-build:dev
#   gl             docker/Dockerfile.gl        zstreamer-build:gl
#   vaapi          docker/Dockerfile.vaapi     zstreamer-build:vaapi
#   oneapi         docker/Dockerfile.oneapi    zstreamer-build:oneapi
#   jetson         docker/Dockerfile.jetson    zstreamer-build:jetson
#   st2110         docker/Dockerfile.st2110    zstreamer-build:st2110
#
# Build context is always the repository root, because the Dockerfiles
# COPY the whole tree into the image.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

VARIANT="${1:-}"
[[ -z "${VARIANT}" ]] && usage
shift || true

case "${VARIANT}" in
    dev)            DOCKERFILE="docker/Dockerfile"                  IMAGE="zstreamer-build:dev" ;;
    gl)             DOCKERFILE="docker/Dockerfile.gl"               IMAGE="zstreamer-build:gl" ;;
    vaapi)          DOCKERFILE="docker/Dockerfile.vaapi"            IMAGE="zstreamer-build:vaapi" ;;
    oneapi)         DOCKERFILE="docker/Dockerfile.oneapi"           IMAGE="zstreamer-build:oneapi" ;;
    jetson)         DOCKERFILE="docker/Dockerfile.jetson"           IMAGE="zstreamer-build:jetson" ;;
    st2110)         DOCKERFILE="docker/Dockerfile.st2110"           IMAGE="zstreamer-build:st2110" ;;
    *)
        echo "Error: unknown variant '${VARIANT}'" >&2
        usage
        ;;
esac

echo "Building ${IMAGE} from ${DOCKERFILE}"
exec docker build -f "${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}" "$@"
