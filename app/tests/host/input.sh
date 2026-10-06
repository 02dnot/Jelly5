#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test of the controller and TV-remote input (nuvio_input.c) with ASan
# and UBSan: synthetic pads, no console or server needed.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/jelly5-input.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/app/engine/include" "$root/app/tests/host/input_test.c" \
    -pthread -lm -o "$build/input-test"
"$build/input-test"
