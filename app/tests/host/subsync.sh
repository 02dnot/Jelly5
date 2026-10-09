#!/usr/bin/env bash
# Subtitle auto-sync against a TEST server (reads only): LIVETV_URL/USER/PASS (the
# test Jellyfin) or, with "emby", EMBY_URL/USER/PASS, from ../../.env.local. The
# server needs the film "Synktest" (Testfiler): speech-like noise bursts, an
# embedded SRT 1.7 s late and an external .srt 3.2 s early, made by
# tests/host/subsync_film.py; SYNC_TITLE="Synktest Offset" or "Synktest MP4" runs one of its
# variants. Never the real server (JF_URL).
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${APP}/build/host"
mkdir -p "${OUT}"
set -a; source "${APP}/../.env.local"; set +a
FF_CFLAGS="$(pkg-config --cflags libavformat libavcodec libswresample libavutil)"
FF_LIBS="$(pkg-config --libs libavformat libavcodec libswresample libavutil)"
clang -O1 -c "${APP}/engine/addons/src/cJSON.c" -I"${APP}/engine/addons/include" -o "${OUT}/cJSON.o"
# shellcheck disable=SC2086
clang -O1 -c "${APP}/tests/host/subsync_glue.c" -I"${APP}/engine/media/include" -I"${APP}/engine/include" \
    ${FF_CFLAGS} -o "${OUT}/subsync_glue.o"
# shellcheck disable=SC2086
clang++ -std=c++17 -O1 -Wall -I"${APP}/src/jf" -I"${APP}/engine/addons/include" -I"${APP}/engine/media/include" \
    "${APP}/src/jf/jf_client.cpp" "${APP}/tests/host/jf_http_curl.cpp" "${APP}/tests/host/subsync_smoke.cpp" \
    "${OUT}/subsync_glue.o" "${OUT}/cJSON.o" ${FF_LIBS} -lcurl -o "${OUT}/subsync_smoke"
if [[ "${1:-}" == "emby" ]]; then
    SYNC_URL="${EMBY_URL:?}" SYNC_USER="${EMBY_USER:?}" SYNC_PASS="${EMBY_PASS?}" "${OUT}/subsync_smoke"
else
    SYNC_URL="${LIVETV_URL:?}" SYNC_USER="${LIVETV_USER:?}" SYNC_PASS="${LIVETV_PASS?}" "${OUT}/subsync_smoke"
fi
