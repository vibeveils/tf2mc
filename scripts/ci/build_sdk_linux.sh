#!/bin/bash
# Builds the TF2 mod for Linux with Docker + the Steam Runtime sniper SDK image (same toolchain as
# the SDK's ./buildallprojects). Usage: scripts/ci/build_sdk_linux.sh <path to source-sdk-2013> [release|debug]
set -euo pipefail
SDK=$(readlink -f "${1:?usage: build_sdk_linux.sh <sdk dir> [release|debug]}")
MODE=${2:-release}
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${SNIPER_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}
docker run --rm \
  -v "$SDK":/sdk \
  -v "$HERE":/ci:ro \
  -e CCACHE_DIR=/sdk/.ccache \
  "$IMAGE" bash /ci/build_sdk_linux_in_container.sh /sdk "$MODE"
