#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test of the subtitle symbol wrapping (sub_symbols.c) with ASan and
# UBSan: no console or server needed.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/jelly5-subsymbols.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/app/src" "$root/app/src/sub_symbols.c" "$root/app/tests/host/subsymbols_test.c" \
    -o "$build/subsymbols-test"
"$build/subsymbols-test"
