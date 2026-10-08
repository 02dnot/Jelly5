#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test of the library's letter jump (ui/letters) with ASan and UBSan:
# every L2/R2 press from every title, against what the test servers answered
# (letters/*.txt: Jellyfin 12.2 and Emby 4.10, recorded). No server needed.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/jelly5-letters.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/app/src" "$root/app/tests/host/letters_test.cpp" "$root/app/src/ui/letters.cpp" \
    -o "$build/letters-test"
"$build/letters-test" "$root"/app/tests/host/letters/*.txt
