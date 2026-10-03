#!/usr/bin/env bash
# Jelly5 — sets up the PS5 cross toolchain on macOS (Apple Silicon or Intel).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Result: toolchain/ps5-payload-sdk with the pacbrew homebrew sysroot merged
# into target/user/homebrew. Prints the env to source:
#   eval "$(scripts/setup-toolchain.sh --env)"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="${ROOT}/toolchain"
DL="${TC}/dl"
SDK="${TC}/ps5-payload-sdk"

SDK_URL="https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip"
SDK_SHA="a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb"
PB_URL="https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.39/ps5-payload-dev.tar.gz"
PB_SHA="14ac4113523ed61bc1d42a0d0b7b8981b2bc6aac35a68949c169b0ed0aaf5cdb"

print_env() {
    local llvm core
    llvm="$(brew --prefix llvm)"
    core="$(brew --prefix coreutils)"
    echo "export PS5_PAYLOAD_SDK='${SDK}'"
    echo "export LLVM_CONFIG='${llvm}/bin/llvm-config'"
    echo "export EVO_NATIVE_ZLIB='${TC}/host-zlib'"
    echo "export PS5_LLD='$(brew --prefix lld)/bin/ld.lld'"
    echo "export PATH='${core}/libexec/gnubin:${llvm}/bin:$(brew --prefix lld)/bin:${SDK}/bin:'\"\$PATH\""
}

if [[ "${1:-}" == "--env" ]]; then print_env; exit 0; fi

fetch() { # url sha file
    local url="$1" sha="$2" out="${DL}/$3"
    mkdir -p "${DL}"
    if [[ ! -f "${out}" ]] || ! echo "${sha}  ${out}" | shasum -a 256 -c - >/dev/null 2>&1; then
        echo "==> downloading $3"
        curl -fSL -o "${out}" "${url}"
    fi
    echo "${sha}  ${out}" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $3" >&2; exit 1; }
}

for t in llvm lld coreutils; do
    brew --prefix "$t" >/dev/null 2>&1 || { echo "missing: brew install $t" >&2; exit 2; }
done

fetch "${SDK_URL}" "${SDK_SHA}" ps5-payload-sdk.zip
fetch "${PB_URL}" "${PB_SHA}" pacbrew-v0.39.tar.gz

if [[ ! -x "${SDK}/bin/prospero-clang" ]]; then
    echo "==> extracting SDK"
    rm -rf "${SDK}" && mkdir -p "${TC}"
    unzip -q "${DL}/ps5-payload-sdk.zip" -d "${TC}"
fi

if [[ ! -f "${SDK}/target/user/homebrew/.pacbrew-v0.39" ]]; then
    echo "==> merging pacbrew sysroot"
    tmp="$(mktemp -d)"
    tar -xzf "${DL}/pacbrew-v0.39.tar.gz" -C "${tmp}" "opt/ps5-payload-sdk/target/user/homebrew"
    mkdir -p "${SDK}/target/user/homebrew"
    cp -R "${tmp}/opt/ps5-payload-sdk/target/user/homebrew/." "${SDK}/target/user/homebrew/"
    rm -rf "${tmp}"
    touch "${SDK}/target/user/homebrew/.pacbrew-v0.39"
fi

# Static host zlib for the native-app packaging tool (its own zlib bootstrap
# fails on macOS: zlib's configure emits libtool-style ar flags).
HZ="${TC}/host-zlib"
if [[ ! -f "${HZ}/lib/libz.a" ]]; then
    echo "==> building host zlib"
    ZV=1.3.2 ZS=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    fetch "https://zlib.net/fossils/zlib-${ZV}.tar.gz" "${ZS}" "zlib-${ZV}.tar.gz"
    tmp="$(mktemp -d)"
    tar -xzf "${DL}/zlib-${ZV}.tar.gz" -C "${tmp}"
    mkdir -p "${HZ}/lib" "${HZ}/include"
    for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil \
             compress uncompr gzclose gzlib gzread gzwrite; do
        /usr/bin/clang -O2 -c "${tmp}/zlib-${ZV}/${f}.c" -o "${tmp}/${f}.o"
    done
    /usr/bin/ar rcs "${HZ}/lib/libz.a" "${tmp}"/*.o
    cp "${tmp}/zlib-${ZV}/zlib.h" "${tmp}/zlib-${ZV}/zconf.h" "${HZ}/include/"
    rm -rf "${tmp}"
fi

echo "==> toolchain ready at ${SDK}"
echo "    eval \"\$(scripts/setup-toolchain.sh --env)\""
