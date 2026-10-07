#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The client's requests against a fake server (routes_test.cpp), no network:
# the Jellyfin requests must be the ones main's client makes (pass another
# git ref as $1 to compare with it), and the Emby ones are printed.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REF="${1:-main}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/jelly5-routes.XXXXXX")"
trap 'rm -rf -- "$OUT"' EXIT
mkdir -p "$OUT/base"
git -C "$APP" show "$REF:app/src/jf/jf_client.cpp" > "$OUT/base/jf_client.cpp"
git -C "$APP" show "$REF:app/src/jf/jf_client.h" > "$OUT/base/jf_client.h"
cp "$APP/src/jf/jf_http.h" "$OUT/base/"
clang -O1 -c "$APP/engine/addons/src/cJSON.c" -I"$APP/engine/addons/include" -o "$OUT/cJSON.o"
build() {   # $1: the client's source folder, $2: output, the rest: extra flags
    clang++ -std=c++17 -O1 -Wall -I"$1" -I"$APP/engine/addons/include" "${@:3}" \
        "$1/jf_client.cpp" "$APP/tests/host/routes_test.cpp" "$OUT/cJSON.o" -o "$2"
}
build "$OUT/base" "$OUT/base_routes"
build "$APP/src/jf" "$OUT/routes" -DROUTES_EMBY
"$OUT/base_routes" > "$OUT/base.txt"
"$OUT/routes" > "$OUT/now.txt"
sed -n '/^== jellyfin/,/^== emby/p' "$OUT/now.txt" | sed '/^== emby/d' > "$OUT/now_jellyfin.txt"
if diff -u "$OUT/base.txt" "$OUT/now_jellyfin.txt"; then
    echo "PASS: $(grep -vc '^==' "$OUT/base.txt") Jellyfin requests as on $REF"
else
    echo "FAIL: the Jellyfin requests differ from $REF (above)"
    exit 1
fi
sed -n '/^== emby/,$p' "$OUT/now.txt"
