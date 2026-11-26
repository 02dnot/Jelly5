#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test of Client::home_sections() (home_sections_test.cpp) with ASan and
# UBSan, against canned answers. No server needed.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/jelly5-home-sections.XXXXXX")"
trap 'rm -rf -- "$OUT"' EXIT
SAN=(-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
clang "${SAN[@]}" -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/cJSON.o"
clang++ -std=c++17 -Wall "${SAN[@]}" -I"$APP/src/jf" -I"$APP/engine/addons/include" \
    "$APP/src/jf/jf_client.cpp" "$APP/tests/host/home_sections_test.cpp" "$OUT/cJSON.o" \
    -o "$OUT/home_sections_test"
"$OUT/home_sections_test"
