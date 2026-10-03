#!/usr/bin/env bash
# Builds and runs the host smoke test against the server in ../../.env.local.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
set -a; source "${APP}/../.env.local"; set +a
clang -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
clang++ -std=c++17 -O1 -Wall -I"${APP}/src/jf" -I"${APP}/engine/addons/include" \
    "${APP}/src/jf/jf_client.cpp" "${APP}/tests/host/jf_http_curl.cpp" "${APP}/tests/host/jf_smoke.cpp" \
    "${OUT}/cJSON.o" -lcurl -o "${OUT}/jf_smoke"
"${OUT}/jf_smoke"
