#!/usr/bin/env bash
# Jelly5 — sets up the PS5 cross toolchain on macOS (Apple Silicon or Intel,
# with Homebrew) or Linux (the distribution's LLVM: clang, lld, llvm).
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
OS="$(uname -s)"

SDK_URL="https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip"
SDK_SHA="a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb"
PB_URL="https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.39/ps5-payload-dev.tar.gz"
PB_SHA="14ac4113523ed61bc1d42a0d0b7b8981b2bc6aac35a68949c169b0ed0aaf5cdb"

# Linux: the newest llvm-config there is (LLVM_CONFIG wins when set). clang and
# ld.lld must come from the same LLVM: the SDK's wrappers run them from its bindir.
linux_llvm_config() {
    if [[ -n "${LLVM_CONFIG:-}" && -x "$(command -v "${LLVM_CONFIG}")" ]]; then
        command -v "${LLVM_CONFIG}"
        return
    fi
    local v
    for v in 22 21 20 19 18 17; do
        command -v "llvm-config-${v}" 2>/dev/null && return
    done
    command -v llvm-config 2>/dev/null || true
}

print_env() {
    if [[ "${OS}" == "Darwin" ]]; then
        local llvm core
        llvm="$(brew --prefix llvm)"
        core="$(brew --prefix coreutils)"
        echo "export PS5_PAYLOAD_SDK='${SDK}'"
        echo "export LLVM_CONFIG='${llvm}/bin/llvm-config'"
        echo "export EVO_NATIVE_ZLIB='${TC}/host-zlib'"
        echo "export PS5_LLD='$(brew --prefix lld)/bin/ld.lld'"
        echo "export PATH='${core}/libexec/gnubin:${llvm}/bin:$(brew --prefix lld)/bin:${SDK}/bin:'\"\$PATH\""
        return
    fi
    local config bindir
    config="$(linux_llvm_config)"
    [[ -n "${config}" ]] || { echo "echo 'llvm-config not found: run scripts/setup-toolchain.sh' >&2"; return; }
    bindir="$("${config}" --bindir)"
    echo "export PS5_PAYLOAD_SDK='${SDK}'"
    echo "export LLVM_CONFIG='${config}'"
    echo "export EVO_NATIVE_ZLIB='${TC}/host-zlib'"
    echo "export PS5_LLD='${bindir}/ld.lld'"
    echo "export PATH='${bindir}:${SDK}/bin:'\"\$PATH\""
}

if [[ "${1:-}" == "--env" ]]; then print_env; exit 0; fi

sha256_ok() { # sha file
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$1  $2" | sha256sum -c - >/dev/null 2>&1
    else
        echo "$1  $2" | shasum -a 256 -c - >/dev/null 2>&1
    fi
}

fetch() { # url sha file
    local url="$1" sha="$2" out="${DL}/$3"
    mkdir -p "${DL}"
    if [[ ! -f "${out}" ]] || ! sha256_ok "${sha}" "${out}"; then
        echo "==> downloading $3"
        curl -fSL -o "${out}" "${url}"
    fi
    sha256_ok "${sha}" "${out}" || { echo "checksum mismatch: $3" >&2; exit 1; }
}

if [[ "${OS}" == "Darwin" ]]; then
    for t in llvm lld coreutils; do
        brew --prefix "$t" >/dev/null 2>&1 || { echo "missing: brew install $t" >&2; exit 2; }
    done
    HOST_CC=/usr/bin/clang
    HOST_AR=/usr/bin/ar
else
    APT_HINT="Debian/Ubuntu: sudo apt install build-essential clang lld llvm curl unzip zip python3-venv"
    LLVM_CFG="$(linux_llvm_config)"
    [[ -n "${LLVM_CFG}" ]] || { echo "missing: llvm-config (${APT_HINT})" >&2; exit 2; }
    LLVM_BIN="$("${LLVM_CFG}" --bindir)"
    LLVM_VER="$("${LLVM_CFG}" --version)"
    for t in clang clang++ ld.lld llvm-nm llvm-ar; do
        [[ -x "${LLVM_BIN}/${t}" ]] || { echo "missing: ${LLVM_BIN}/${t} for LLVM ${LLVM_VER} (${APT_HINT})" >&2; exit 2; }
    done
    for t in make curl unzip zip tar python3; do
        command -v "$t" >/dev/null 2>&1 || { echo "missing: $t (${APT_HINT})" >&2; exit 2; }
    done
    echo "==> LLVM ${LLVM_VER} at ${LLVM_BIN}"
    HOST_CC="${LLVM_BIN}/clang"
    HOST_AR="${LLVM_BIN}/llvm-ar"
fi

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
        # Z_HAVE_UNISTD_H: what configure would find (lseek & co. on Linux)
        "${HOST_CC}" -O2 -fPIC -DZ_HAVE_UNISTD_H -c "${tmp}/zlib-${ZV}/${f}.c" -o "${tmp}/${f}.o"
    done
    "${HOST_AR}" rcs "${HZ}/lib/libz.a" "${tmp}"/*.o
    cp "${tmp}/zlib-${ZV}/zlib.h" "${tmp}/zlib-${ZV}/zconf.h" "${HZ}/include/"
    rm -rf "${tmp}"
fi

echo "==> toolchain ready at ${SDK}"
echo "    eval \"\$(scripts/setup-toolchain.sh --env)\""
