#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and runs the Live TV host test against the test Jellyfin with the IPTV
# test channels (LIVETV_URL/LIVETV_USER/LIVETV_PASS in ../../.env.local; "emby":
# EMBY_URL..., which shows channels only with Emby Premiere). It opens live streams
# and closes them again; it sets no recordings unless given --record (a write).
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
set -a; source "${APP}/../.env.local"; set +a
clang -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
clang++ -std=c++17 -O1 -g -Wall -fsanitize=address,undefined -I"${APP}/src/jf" -I"${APP}/engine/addons/include" \
    "${APP}/src/jf/jf_client.cpp" "${APP}/tests/host/jf_http_curl.cpp" "${APP}/tests/host/livetv_smoke.cpp" \
    "${OUT}/cJSON.o" -lcurl -fsanitize=address,undefined -o "${OUT}/livetv_smoke"
if [[ "${1:-}" == "emby" ]]; then
    shift
    JF_URL="${EMBY_URL}" JF_USER="${EMBY_USER}" JF_PASS="${EMBY_PASS}" "${OUT}/livetv_smoke" "$@"
else
    : "${LIVETV_URL:?LIVETV_URL (and LIVETV_USER, LIVETV_PASS) in .env.local}"
    JF_URL="${LIVETV_URL}" JF_USER="${LIVETV_USER}" JF_PASS="${LIVETV_PASS}" "${OUT}/livetv_smoke" "$@"
fi
