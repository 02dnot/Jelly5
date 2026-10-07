#!/usr/bin/env bash
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds AV1 software decoding for the console: dav1d (with its x86-64 asm)
# and FFmpeg 7.1 with --enable-libdav1d, cross-compiled with the PS5 SDK.
# Installs into a prefix of its own, toolchain/av1-prefix/{include,lib}, and
# leaves the SDK sysroot (pacbrew's FFmpeg 7.0) untouched. The app builds with
# it by default (app/scripts/build.sh; JELLY5_AV1=0 builds without).
#
#   scripts/setup-toolchain.sh          # runs this at its end (--no-av1 skips it)
#   scripts/build-av1.sh                # on its own (once; ~5-10 min)
#   scripts/build-av1.sh --force        # rebuild
#
# Host tools: nasm, meson, ninja, pkg-config (brew install nasm meson ninja
# pkgconf; apt: nasm meson ninja-build pkg-config).
#
# FFmpeg gets pacbrew's configure flags (from the 7.0 libavcodec.a in the SDK
# sysroot), plus libdav1d, minus FFmpeg's own "av1" decoder: on the PS5 that
# one is hardware-only and dereferences null without an hwaccel. The av1
# parser stays (Matroska/MP4 framing needs it).
set -euo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="${ROOT}/toolchain"
DL="${TC}/dl"
SDK="${TC}/ps5-payload-sdk"
AV1="${JELLY5_AV1_PREFIX:-${TC}/av1-prefix}"
WORK="${ROOT}/build/av1-src"

DAV1D_V=1.5.1
DAV1D_SHA=401813f1f89fa8fd4295805aa5284d9aed9bc7fc1fdbe554af4292f64cbabe21
FF_V=7.1.1
FF_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -x "${SDK}/bin/prospero-clang" && -f "${SDK}/target/user/homebrew/lib/libssl.a" ]] ||
    die "SDK sysroot missing: run scripts/setup-toolchain.sh first"
if [[ "$(uname -s)" == Darwin ]]; then
    TOOL_HINT="brew install nasm meson ninja pkgconf"
else
    TOOL_HINT="apt install nasm meson ninja-build pkg-config"
fi
for t in nasm meson ninja pkg-config make curl tar; do
    command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t (${TOOL_HINT})"
done
# The prefix is replaced wholesale: never let a stray JELLY5_AV1_PREFIX point that at
# something else (the SDK, a home folder).
case "$(basename "${AV1}")" in *av1*) ;; *) die "JELLY5_AV1_PREFIX must name an av1 folder: ${AV1}" ;; esac
[[ "${AV1}" != "${SDK}"* ]] || die "JELLY5_AV1_PREFIX must not be inside the SDK: ${AV1}"

# The toolchain environment (LLVM, lld, GNU coreutils, the SDK's bin).
eval "$("${ROOT}/scripts/setup-toolchain.sh" --env)"
# shellcheck disable=SC1091
source "${SDK}/toolchain/prospero.sh"
# prospero.sh points DESTDIR at the SDK sysroot: installs here go to our prefix.
unset DESTDIR

# (no grep -q on a pipe: under pipefail its early exit fails llvm-nm with SIGPIPE)
has_sym() { [[ "$(llvm-nm "$1" 2>/dev/null | grep -cE "$2")" -gt 0 ]]; }
built() {
    [[ -f "${AV1}/lib/libdav1d.a" && -f "${AV1}/lib/libavcodec.a" ]] &&
        has_sym "${AV1}/lib/libavcodec.a" ' D ff_libdav1d_decoder$'
}
if (( ! FORCE )) && built; then
    echo "==> AV1 prefix ready at ${AV1} (--force to rebuild)"
    exit 0
fi

sha256_ok() { # sha file
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$1  $2" | sha256sum -c - >/dev/null 2>&1
    else
        echo "$1  $2" | shasum -a 256 -c - >/dev/null 2>&1
    fi
}
fetch() { # url sha file
    local out="${DL}/$3"
    mkdir -p "${DL}"
    if [[ ! -f "${out}" ]] || ! sha256_ok "$2" "${out}"; then
        echo "==> downloading $3"
        curl -fSL -o "${out}" "$1"
    fi
    sha256_ok "$2" "${out}" || die "checksum mismatch: $3"
}
fetch "https://downloads.videolan.org/pub/videolan/dav1d/${DAV1D_V}/dav1d-${DAV1D_V}.tar.xz" \
      "${DAV1D_SHA}" "dav1d-${DAV1D_V}.tar.xz"
fetch "https://ffmpeg.org/releases/ffmpeg-${FF_V}.tar.xz" "${FF_SHA}" "ffmpeg-${FF_V}.tar.xz"

# Built into a fresh prefix beside the old one, which is swapped out only when
# this one is complete: a failed (re)build leaves the working prefix in place.
FINAL="${AV1}"
AV1="${FINAL}.new"
rm -rf -- "${WORK}" "${AV1}"
mkdir -p "${WORK}" "${AV1}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"

# --- dav1d ----------------------------------------------------------------------
echo "==> dav1d ${DAV1D_V}"
tar -xJf "${DL}/dav1d-${DAV1D_V}.tar.xz" -C "${WORK}"
# The SDK's meson cross file names only a [target_machine]: on an Apple Silicon
# Mac meson would then take the host (aarch64) as the machine the code runs on
# and pick dav1d's ARM asm. Say it: x86-64 FreeBSD (the PS5).
cat > "${WORK}/ps5-host.ini" <<'EOF'
[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
(
    cd "${WORK}/dav1d-${DAV1D_V}"
    "${MESON}" setup build-ps5 --cross-file="${WORK}/ps5-host.ini" --prefix="${AV1}" --libdir=lib \
        --buildtype=release --default-library=static \
        -Denable_asm=true -Denable_tools=false -Denable_tests=false \
        -Denable_examples=false -Dlogging=false > "${WORK}/dav1d-setup.log" 2>&1 ||
        { tail -30 "${WORK}/dav1d-setup.log"; exit 1; }
    ninja -C build-ps5 > "${WORK}/dav1d-build.log" 2>&1 || { tail -30 "${WORK}/dav1d-build.log"; exit 1; }
    ninja -C build-ps5 install > /dev/null
)
[[ -f "${AV1}/lib/libdav1d.a" ]] || die "dav1d did not install"
has_sym "${AV1}/lib/libdav1d.a" '_avx2$' || die "dav1d built without its AVX2 asm"

# --- FFmpeg ---------------------------------------------------------------------
# pkg-config: dav1d from our prefix, everything else (OpenSSL, FreeType,
# fribidi, HarfBuzz, libass, zlib) from the SDK sysroot as pacbrew built it.
PKGC="${WORK}/pkg-config"
cat > "${PKGC}" <<EOF
#!/usr/bin/env bash
for a in "\$@"; do
    case "\$a" in
        dav1d*) PKG_CONFIG_SYSROOT_DIR= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="${AV1}/libdata/pkgconfig:${AV1}/lib/pkgconfig" \\
                    exec pkg-config --static "\$@" ;;
    esac
done
exec "${PKG_CONFIG}" "\$@"
EOF
chmod +x "${PKGC}"

echo "==> FFmpeg ${FF_V} (+libdav1d)"
tar -xJf "${DL}/ffmpeg-${FF_V}.tar.xz" -C "${WORK}"
(
    cd "${WORK}/ffmpeg-${FF_V}"
    ./configure \
        --prefix="${AV1}" --enable-cross-compile \
        --cross-prefix="${SDK}/bin/prospero-" \
        --enable-static --disable-shared \
        --arch=x86_64 --target-os=freebsd \
        --enable-openssl --enable-version3 \
        --enable-libfreetype --enable-libfribidi --enable-libharfbuzz \
        --enable-libass \
        --disable-debug --disable-doc --disable-programs \
        --enable-libdav1d --disable-decoder=av1 \
        --cc="${CC}" --cxx="${CXX}" --nm="${NM}" --strip="${STRIP}" \
        --ar="${AR}" --ranlib="${RANLIB}" --pkg-config="${PKGC}" \
        > "${WORK}/ffmpeg-configure.log" 2>&1 ||
        { tail -30 "${WORK}/ffmpeg-configure.log"; tail -30 ffbuild/config.log; exit 1; }
    for d in 'CONFIG_LIBDAV1D_DECODER 1' 'CONFIG_AV1_DECODER 0' 'HAVE_X86ASM 1' 'HAVE_AVX2_EXTERNAL 1' \
             'CONFIG_OPENSSL 1' 'CONFIG_LIBASS 1'; do
        grep -q "#define ${d}\$" config.h config_components.h 2>/dev/null ||
            { echo "FFmpeg config: expected ${d}" >&2; exit 1; }
    done
    make -j"${JOBS}" > "${WORK}/ffmpeg-build.log" 2>&1 || { tail -40 "${WORK}/ffmpeg-build.log"; exit 1; }
    make install > /dev/null
)
built || die "FFmpeg was built without the libdav1d decoder"
if has_sym "${AV1}/lib/libavcodec.a" ' D ff_av1_decoder$'; then
    die "FFmpeg still has its hardware-only av1 decoder"
fi

# pkg-config files carry the prefix they were built for: point them at the final
# one. Only those text files: the archives hold the path in their debug info, and
# an edit there corrupts them.
find "${AV1}" -name '*.pc' -type f | while IFS= read -r f; do
    sed -i.bak "s#${AV1}#${FINAL}#g" "$f" && rm -f "$f.bak"
done
rm -rf -- "${FINAL}.old"
[[ -d "${FINAL}" ]] && mv -- "${FINAL}" "${FINAL}.old"
mv -- "${AV1}" "${FINAL}"
rm -rf -- "${FINAL}.old"
AV1="${FINAL}"

echo "==> AV1 prefix ready at ${AV1}"
for a in libdav1d libavcodec libavformat libavutil libswresample libswscale; do
    printf '    %-16s %s bytes\n' "${a}.a" "$(wc -c < "${AV1}/lib/${a}.a" | tr -d ' ')"
done
echo "    the app builds with it by default (app/scripts/build.sh)"
