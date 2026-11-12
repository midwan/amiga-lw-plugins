#!/bin/bash
# Build LightWave plugins using the Docker cross-compiler
set -e

DOCKER_IMAGE="amigadev/crosstools:m68k-amigaos"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

docker run --rm \
    -v "${PROJECT_DIR}":/work \
    -w /work \
    "${DOCKER_IMAGE}" \
    make "$@"
