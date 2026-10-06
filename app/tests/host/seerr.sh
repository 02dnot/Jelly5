#!/usr/bin/env bash
# Builds and runs the Seerr client's host test against the server in
# ../../.env.local (SEERR_URL; JF_URL, JF_USER, JF_PASS for Quick Connect).
# Options go to the test (see tests/host/seerr_smoke.cpp); nothing is
# requested unless --for-real is given.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
set -a; source "${APP}/../.env.local"; set +a
clang -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
clang++ -std=c++17 -O1 -Wall -Wno-unused-parameter -I"${APP}/src" -I"${APP}/src/jf" -I"${APP}/engine/addons/include" \
    "${APP}/src/seerr/seerr_client.cpp" "${APP}/src/jf/jf_client.cpp" "${APP}/tests/host/jf_http_curl.cpp" \
    "${APP}/tests/host/seerr_smoke.cpp" "${OUT}/cJSON.o" -lcurl -o "${OUT}/seerr_smoke"
"${OUT}/seerr_smoke" "$@"
