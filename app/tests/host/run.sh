#!/usr/bin/env bash
# Builds and runs the host smoke test against the server in ../../.env.local:
# JF_URL/JF_USER/JF_PASS, or with "emby" EMBY_URL/EMBY_USER/EMBY_PASS.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
set -a; source "${APP}/../.env.local"; set +a
clang -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
clang++ -std=c++17 -O1 -Wall -I"${APP}/src/jf" -I"${APP}/engine/addons/include" \
    "${APP}/src/jf/jf_client.cpp" "${APP}/tests/host/jf_http_curl.cpp" "${APP}/tests/host/jf_smoke.cpp" \
    "${OUT}/cJSON.o" -lcurl -o "${OUT}/jf_smoke"
if [[ "${1:-}" == "emby" ]]; then
    : "${EMBY_URL:?EMBY_URL (and EMBY_USER, EMBY_PASS) in .env.local}"
    JF_URL="${EMBY_URL}" JF_USER="${EMBY_USER}" JF_PASS="${EMBY_PASS}" "${OUT}/jf_smoke"
else
    "${OUT}/jf_smoke"
fi
