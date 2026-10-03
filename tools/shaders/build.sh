#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds app/shaders/*.pipe into app/engine/shaders/agc/<name>_pipe.h with
# amdllpc (gfx1013) in a container. The first run builds the compiler image
# (an LLVM-scale build, a long while under Rosetta); later runs take seconds.
#
#   tools/shaders/build.sh              all pipes
#   tools/shaders/build.sh liquid_glass one
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE=jelly5/amdllpc:v-2025.Q2.1

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "==> building the amdllpc image (once; this takes a long while)"
    docker build --platform linux/amd64 -t "${IMAGE}" "${ROOT}/tools/shaders"
fi

pipes=()
for n in "$@"; do pipes+=("app/shaders/${n}.pipe"); done
echo "==> compiling pipes"
docker run --rm --platform linux/amd64 -v "${ROOT}:/workspace" -w /workspace "${IMAGE}" \
    python3 tools/shaders/build_agc_pipes.py --readelf llvm-readelf-18 --objcopy llvm-objcopy-18 \
    --output-dir /workspace/build/agc_pipes ${pipes[@]+"${pipes[@]}"}
