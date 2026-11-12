#!/bin/bash
# Runs INSIDE the Steam Runtime "sniper" SDK container (what ./buildallprojects does, minus the
# interactive podman wrapper that can't run in CI). Usage: build_sdk_linux_in_container.sh <sdk dir> [release|debug]
set -euo pipefail
SDK=${1:?sdk dir}
MODE=${2:-release}
cd "$SDK/src"
export VPC_NINJA_BUILD_MODE="$MODE"
out="_vpc_/ninja/sdk_tf_$MODE"
chmod +x devtools/bin/vpc devtools/bin/linux/* 2>/dev/null || true
devtools/bin/vpc /tf /linux64 /ninja /define:SOURCESDK +everything /mksln "$out"
ninja -f "$out.ninja" -j"$(nproc)"
echo "--- built binaries ---"
find "$SDK/game/mod_tf/bin" -name "*.so" 2>/dev/null || true
