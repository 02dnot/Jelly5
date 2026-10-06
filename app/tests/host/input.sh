#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/jelly5-input.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$root/app/engine/include" "$root/app/tests/input_test.c" \
    -pthread -lm -o "$build/input-test"
"$build/input-test"
