#!/usr/bin/env bash
# Jelly5 (based on Nuvio PS5, Copyright (C) 2026 Husam Osman)
# SPDX-License-Identifier: GPL-3.0-or-later
# =============================================================================
# app/scripts/build.sh - build the Nuvio app (PPSA99176): the web UI
# host plus the Nuvio Player (EVO Player's playback engine, vendored in
# engine/).
#
# Runs with the PS5 payload SDK and the pacbrew sysroot (FFmpeg, dav1d,
# OpenSSL) set up by scripts/setup-toolchain.sh (macOS or Linux), or inside a
# container image that has them. From the repository's app/ folder:
#
#   ./scripts/build.sh            # development build (in the container when
#                                 # NUVIO_BUILD_IMAGE is set)
#   ./scripts/build.sh --ffpfsc   # also pack PPSA99505.ffpfsc
#   ./scripts/build.sh --release  # for sharing: no .env.local server, no log
#                                 # target, packed as .ffpfsc and .zip
#
# Output: build/app/PPSA99505/{eboot.bin, sce_sys/, sce_module/libc.prx}
# The packaging steps follow the toolkit's app packaging (player mode);
# the loader constants and PRX stub rules there are hardware-validated.
# =============================================================================
set -euo pipefail
# Byte-order sorting for globs: under a Norwegian (or any non-C) locale
# libkernel_web.so sorts before libkernel.so and the link binds the kernel
# imports to the web process's kernel library, which an app cannot load.
export LC_ALL=C

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NUVIO_ROOT="$(cd "${APP_ROOT}/.." && pwd)"
EVO_ROOT="${EVO_ROOT:-${NUVIO_ROOT}/toolkit}"   # native-app toolkit (vendored)
IMAGE="${NUVIO_BUILD_IMAGE:-}"

FFPFSC=0; RELEASE=0
for arg in "$@"; do
    case "${arg}" in
        --ffpfsc) FFPFSC=1 ;;
        --release) RELEASE=1; FFPFSC=1 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option: ${arg}" >&2; exit 2 ;;
    esac
done

# --- Outside the container: re-exec inside it --------------------------------
# Builds with the PS5 Payload SDK on this machine (PS5_PAYLOAD_SDK). Set
# NUVIO_BUILD_IMAGE to run the same build inside a container image that ships it.
if [[ -n "${NUVIO_BUILD_IMAGE:-}" && ! -f /etc/profile.d/ps5-sdk.sh ]]; then
    exec docker run --rm --platform linux/amd64 \
        -v "${NUVIO_ROOT}:/nuvio" \
        -v evoplayer_ps5_sdk:/opt/ps5-payload-sdk \
        -v evoplayer_ccache:/ccache -e CCACHE_DIR=/ccache \
        -w /nuvio/app "${IMAGE}" \
        bash -lc "./scripts/build.sh $*"
fi

log()  { echo "==> $*"; }
ok()   { echo "  ok $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
need() { [[ -e "$1" ]] || die "$2"; }

: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
PS5_SYSROOT="${PS5_PAYLOAD_SDK}/target"
HB="${PS5_SYSROOT}/user/homebrew"
NATIVE="${EVO_ROOT}/tools/native-app"
LLD="$(command -v prospero-lld || echo "${PS5_PAYLOAD_SDK}/bin/prospero-lld")"
AR="$(command -v prospero-ar || echo "${PS5_PAYLOAD_SDK}/bin/prospero-ar")"
need "${NATIVE}/ps5-pie.ld" "native-app toolkit not found at ${NATIVE}"

# Hardware-validated loader profile.
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

TCC="prospero-clang"
TCXX="prospero-clang++"
TFLAGS=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)

BUILD="${APP_ROOT}/build"
PARAM="${APP_ROOT}/sce_sys/param.json"
# JELLY5_PROBE=1: build tests/probe_main.c alone as "Jelly5 Probe" (PPSA99506),
# to tell toolchain/loader problems from the app's own start-up.
if [[ -n "${JELLY5_PROBE:-}" ]]; then
    mkdir -p "${BUILD}"
    python3 - "${PARAM}" "${BUILD}/probe-param.json" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
import os
if os.environ.get("JELLY5_PROBE") == "media":   # Media category, no extra attribute
    j["titleId"] = "PPSA99507"; j["conceptId"] = "99507"
    j["contentId"] = "UP9000-PPSA99507_00-JELLY5MPROBE0000"
    j["applicationCategoryType"] = 65536; j["attribute"] = 0
    j["localizedParameters"] = {"defaultLanguage": "en-US", "en-US": {"titleName": "Jelly5 Media Probe"}}
else:
    j["titleId"] = "PPSA99506"; j["conceptId"] = "99506"
    j["contentId"] = "UP9000-PPSA99506_00-JELLY5PROBE00000"
    j["localizedParameters"] = {"defaultLanguage": "en-US", "en-US": {"titleName": "Jelly5 Probe"}}
json.dump(j, open(sys.argv[2], "w"), indent=2)
PY
    PARAM="${BUILD}/probe-param.json"
fi
TITLE_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "${PARAM}")"
APPDIR="${BUILD}/app/${TITLE_ID}"
mkdir -p "${BUILD}/obj" "${BUILD}/host" "${BUILD}/stubs"

# --- Host converter + runtime shim ---------------------------------------------
log "native-app tools"
eval "$("${EVO_ROOT}/scripts/setup-native-app-deps.sh")"
TOOL="${BUILD}/host/ps5-native-tool"
if [[ ! -x "${TOOL}" ]]; then
    clang++ -std=c++20 -O2 -I "${ZLIB_INCLUDE}" \
        "${NATIVE}/native_app_builder.cpp" "${NATIVE}/self_container.cpp" \
        "${NATIVE}/elf_object.cpp" "${NATIVE}/sce_module_writer.cpp" \
        "${ZLIB_ARCHIVE}" -o "${TOOL}"
fi
LIBC_PRX="${NATIVE}/runtime/libc.prx"
need "${LIBC_PRX}" "libc.prx missing from the toolkit"
ok "converter + libc.prx"

# --- Compile ---------------------------------------------------------------------
log "compiling (${TCC})"
# Jelly5 dev settings from ../.env.local: the Jellyfin server, Seerr's address
# (what its setting suggests) and where the console sends its log (this
# machine, as the PS5 sees it).
ENV_LOCAL="${NUVIO_ROOT}/.env.local"
JF_URL=""; PS5_HOST=""; SEERR_URL=""
(( RELEASE )) && ENV_LOCAL=/dev/null   # nothing personal in a shared build
[[ -f "${ENV_LOCAL}" ]] && eval "$(grep -E '^(JF_URL|PS5_HOST|SEERR_URL)=' "${ENV_LOCAL}")"
LOG_HOST="${JELLY5_LOG_HOST:-}"
(( RELEASE )) && LOG_HOST=""
if [[ -z "${LOG_HOST}" && -n "${PS5_HOST}" ]]; then
    if [[ "$(uname -s)" == "Darwin" ]]; then
        IFACE="$(route -n get "${PS5_HOST}" 2>/dev/null | awk '/interface:/{print $2}')"
        [[ -n "${IFACE}" ]] && LOG_HOST="$(ipconfig getifaddr "${IFACE}" 2>/dev/null || true)"
    else   # Linux: the source address of the route to the console
        LOG_HOST="$(ip route get "${PS5_HOST}" 2>/dev/null |
            awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}' || true)"
    fi
fi
JELLY5_DEFS="-DJELLY5_VERSION=\\\"$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["contentVersion"])' "${PARAM}")\\\""
[[ -n "${JF_URL}" ]] && JELLY5_DEFS+=" -DJELLY5_SERVER=\\\"${JF_URL}\\\""
[[ -n "${SEERR_URL}" ]] && JELLY5_DEFS+=" -DJELLY5_SEERR_URL=\\\"${SEERR_URL}\\\""
# JELLY5_REMOTE_LOG=1: a remote test build (issue #12), raw remote samples in
# /download0/jelly5/remote-log.txt.
[[ -n "${JELLY5_REMOTE_LOG:-}" ]] && JELLY5_DEFS+=" -DJELLY5_REMOTE_LOG=1"
[[ -n "${LOG_HOST}" ]] && JELLY5_DEFS+=" -DJELLY5_LOG_HOST=\\\"${LOG_HOST}\\\" -DJELLY5_LOG_PORT=${JELLY5_LOG_PORT:-5555}"
ok "log -> ${LOG_HOST:-none}:${JELLY5_LOG_PORT:-5555}, server ${JF_URL:-default}, Seerr ${SEERR_URL:-not set}"
make -C "${APP_ROOT}" -j"$(nproc)" objects \
    CC="${TCC}" CXX="${TCXX}" TFLAGS="${TFLAGS[*]}" HB="${HB}" JELLY5_DEFS="${JELLY5_DEFS}" \
    > "${BUILD}/compile.log" 2>&1 || { tail -40 "${BUILD}/compile.log"; die "compile failed"; }
mapfile -t OBJS < <(make -C "${APP_ROOT}" -s print-objects | tr ' ' '\n' | grep -E '\.o$')
if [[ -n "${JELLY5_PROBE:-}" ]]; then
    # shellcheck disable=SC2086
    eval "${TCC} -std=gnu11 -O2 ${TFLAGS[*]} ${JELLY5_DEFS} -c '${APP_ROOT}/tests/probe_main.c' -o '${BUILD}/obj/probe_main.o'"
    OBJS=("${BUILD}/obj/probe_main.o")
fi
ok "compiled ${#OBJS[@]} objects"

# Runtime pieces from EVO's native-app kit: CRT, C++ allocation runtime, the
# libc gap fillers, getaddrinfo on the console resolver, the mmap malloc.
"${TCXX}" -std=c++20 -O2 -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_crt.cpp" -o "${BUILD}/obj/app_crt.o"
"${TCXX}" -std=c++20 -O2 -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_cpp_runtime.cpp" -o "${BUILD}/obj/app_cpp_runtime.o"
"${AR}" rcs "${BUILD}/obj/libpthread.a" "${BUILD}/obj/app_cpp_runtime.o"
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${NATIVE}/stubs/libc_ext.c" -o "${BUILD}/obj/libc_ext.o"
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${NATIVE}/stubs/evo_dns.c" -o "${BUILD}/obj/evo_dns.o"
# Nuvio's copy: 64-byte-aligned small blocks come from slabs (see its header).
"${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" -c "${APP_ROOT}/runtime/malloc_shim.c" -o "${BUILD}/obj/malloc_shim.o"
RUNTIME_OBJS=("${BUILD}/obj/malloc_shim.o" "${BUILD}/obj/app_crt.o" "${BUILD}/obj/app_cpp_runtime.o"
              "${BUILD}/obj/libc_ext.o" "${BUILD}/obj/evo_dns.o")

# --- PRX import stubs -------------------------------------------------------------
# System modules the SDK has no link stub for. Each becomes a positional
# DT_NEEDED, so a stub that nothing imports would make the loader refuse the
# app: only the symbols our objects really import go in.
log "PRX import stubs"
UNDEF="${BUILD}/obj-undef.txt"
for o in "${OBJS[@]}" "${RUNTIME_OBJS[@]}"; do
    llvm-nm -u "${o}" 2>/dev/null | grep -oE '\bsce[A-Za-z0-9_]+' || true
done | sort -u > "${UNDEF}"
PRX_STUB_SOS=()
for base in libSceVideodec2 libSceAudiodec libSceAgc libSceAgcDriver libSceCommonDialog \
            libSceWebBrowserDialog; do
    csrc="${BUILD}/stubs/${base}.c"
    : > "${csrc}"
    while read -r s; do
        case "${base}" in
            libSceAgcDriver) [[ "${s}" == sceAgcDriver* ]] || continue ;;
            libSceAgc)       [[ "${s}" == sceAgc* && "${s}" != sceAgcDriver* ]] || continue ;;
            libSceVideodec2) [[ "${s}" == sceVideodec2* ]] || continue ;;
            libSceAudiodec)  [[ "${s}" == sceAudiodec* ]] || continue ;;
            libSceCommonDialog) [[ "${s}" == sceCommonDialog* ]] || continue ;;
            libSceWebBrowserDialog) [[ "${s}" == sceWebBrowserDialog* ]] || continue ;;
        esac
        echo "void ${s}(void){}" >> "${csrc}"
    done < "${UNDEF}"
    [[ -s "${csrc}" ]] || continue
    so="${BUILD}/stubs/${base}.so"
    "${TCC}" -shared -nostdlib -nodefaultlibs -fPIC -Wl,-soname,"${base}.sprx" -o "${so}" "${csrc}"
    PRX_STUB_SOS+=("${so}")
    printf '     %-28s %s syms\n' "${base}.sprx" "$(wc -l < "${csrc}")"
done

# --- Link ---------------------------------------------------------------------------
log "linking"
ARCHIVES=()
# The overlay: libass (subtitles), FreeType/HarfBuzz/fribidi (text), and the
# image decoders for artwork. libass links fontconfig, which is never used.
for a in libavformat libavcodec libswresample libavutil libswscale \
         libass libharfbuzz libfribidi libfontconfig libexpat libfreetype \
         libpng16 libjpeg libwebp libsharpyuv \
         libssl libcrypto libiconv libxml2 libz libbz2 liblzma libzstd libm; do
    need "${HB}/lib/${a}.a" "sysroot archive missing: ${a}.a"
    ARCHIVES+=("${HB}/lib/${a}.a")
done
CXX_RUNTIME=("${PS5_SYSROOT}/lib/libc++.a" "${PS5_SYSROOT}/lib/libc++abi.a" "${PS5_SYSROOT}/lib/libunwind.a")
STUBDIR="${PS5_SYSROOT}/lib"
# SDK link stubs, minus the WebKit-process and system-process kernel variants
# (apps on 11.60 import libkernel only; see src/jelly5_libc_stubs.c).
SDK_STUBS=()
for so in "${STUBDIR}"/*.so; do
    case "$(basename "${so}")" in
        libkernel_web.so|libScePosixForWebKit.so|libkernel_sys.so) continue ;;
    esac
    SDK_STUBS+=("${so}")
done
LINK_LOG="${BUILD}/link.log"
link_app() {
    # --allow-multiple-definition: evo_dns.o (first on the line) supplies
    # getaddrinfo & co. on the console resolver; libc.a's netdb.o still comes
    # in for gethostbyname/getservbyname, which the WebKit stubs used to cover.
    "${LLD}" -T "${NATIVE}/ps5-pie.ld" --eh-frame-hdr --allow-multiple-definition \
        --version-script "${NATIVE}/app-symbols.map" --exclude-libs=ALL --error-limit=0 \
        -L "${BUILD}/obj" -e _start -o "${BUILD}/llvm-pie.elf" \
        "${RUNTIME_OBJS[@]}" "${OBJS[@]}" "$@" "${PRX_STUB_SOS[@]}" \
        --start-group "${ARCHIVES[@]}" --end-group "${CXX_RUNTIME[@]}" \
        --as-needed "${SDK_STUBS[@]}" "${PRX_STUB_SOS[@]}" \
        --start-group "${PS5_SYSROOT}/lib/libc.a" --end-group \
        2> "${LINK_LOG}"
}
if ! link_app; then
    grep -oE "undefined symbol: .*" "${LINK_LOG}" | sed 's/^undefined symbol: //' \
        | sort -u > "${BUILD}/undefined-symbols.txt" || true
    if [[ -s "${BUILD}/undefined-symbols.txt" ]]; then
        echo "   $(wc -l < "${BUILD}/undefined-symbols.txt") undefined symbols (build/undefined-symbols.txt):"
        head -60 "${BUILD}/undefined-symbols.txt" | sed 's/^/     /'
    else
        tail -30 "${LINK_LOG}"
    fi
    die "link failed"
fi

# Runtime import check: the loader binds an import the console does not
# export to NULL, silently (strcasestr did that). Relink with a table of every
# import so the app can log the NULL ones at start (nuvio_import_check).
IMPORTS="${BUILD}/imports.txt"
llvm-nm "${BUILD}/llvm-pie.elf" | awk '$1=="U"{print $2}' | sort -u > "${IMPORTS}"
python3 - "${IMPORTS}" "${BUILD}/obj/import_check.c" <<'PY'
import sys
names = [n for n in open(sys.argv[1]).read().split() if n and not n.startswith("nuvio_import")]
c = ["/* Generated by scripts/build.sh: every import of the eboot. */"]
c += ["extern void %s(void);" % n for n in names]
c += ["static const void *const s_addr[] = {"] + ["    (const void *)&%s," % n for n in names] + ["};"]
c += ["static const char *const s_name[] = {"] + ['    "%s",' % n for n in names] + ["};"]
c += ["int nuvio_import_count(void) { return %d; }" % len(names),
      "const char *nuvio_import_null(int i) { return s_addr[i] ? 0 : s_name[i]; }"]
open(sys.argv[2], "w").write("\n".join(c) + "\n")
PY
"${TCC}" -std=gnu11 -O1 -fPIC -w "${TFLAGS[@]}" -c "${BUILD}/obj/import_check.c" -o "${BUILD}/obj/import_check.o"
link_app "${BUILD}/obj/import_check.o" || { tail -30 "${LINK_LOG}"; die "relink with the import check failed"; }
ok "linked $(stat -c %s "${BUILD}/llvm-pie.elf") bytes"

# --- Convert, sign, assemble ------------------------------------------------------------
log "converting + signing"
CONV_STUB_ARGS=()
for so in "${PRX_STUB_SOS[@]}"; do CONV_STUB_ARGS+=(--stub "${so}"); done
"${TOOL}" link --in "${BUILD}/llvm-pie.elf" --out "${BUILD}/eboot.elf" \
    --stub-dir "${STUBDIR}" "${CONV_STUB_ARGS[@]}" \
    --module-sdk "${MODULE_SDK}" --companion-sdk "${COMPANION_SDK}" --file-name eboot.elf
rm -rf -- "${APPDIR}"
mkdir -p "${APPDIR}/sce_sys" "${APPDIR}/sce_module"
"${TOOL}" self --sign --in "${BUILD}/eboot.elf" --out "${APPDIR}/eboot.bin" --magic "${FSELF_MAGIC}"
cp "${PARAM}" "${APPDIR}/sce_sys/param.json"
cp "${LIBC_PRX}" "${APPDIR}/sce_module/libc.prx"
# pic0/pic1.dds: the home screen's backgrounds (scripts/make_dds.py); the PNGs are PS4-style leftovers
for asset in icon0.png pic0.png pic1.png pic0.dds pic1.dds; do
    [[ -f "${APP_ROOT}/sce_sys/${asset}" ]] && cp "${APP_ROOT}/sce_sys/${asset}" "${APPDIR}/sce_sys/"
done
"${TOOL}" self --inspect --file "${APPDIR}/eboot.bin" | grep -E "integrity|digest" || true

if (( FFPFSC )); then
    log "PFS-packing ${TITLE_ID}.ffpfsc"
    MKPFS="$("${EVO_ROOT}/scripts/setup-pfs-tool.sh")"
    rm -f -- "${BUILD}/app/${TITLE_ID}.ffpfsc"
    "${MKPFS}" pack folder --no-adjust-output-file-extension --version PS5 --verify \
        "${APPDIR}" "${BUILD}/app/${TITLE_ID}.ffpfsc"
fi
if (( RELEASE )); then
    VER="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["contentVersion"])' "${PARAM}")"
    rm -f -- "${BUILD}/app/Jelly5-${VER}.zip"
    # The licences travel with the binaries (GPL, and the fonts' OFL).
    LIC="${BUILD}/app/licenses"
    rm -rf -- "${LIC}" && mkdir -p "${LIC}"
    cp "${NUVIO_ROOT}/LICENSE" "${NUVIO_ROOT}/THIRD_PARTY_NOTICES.md" "${LIC}/"
    cp "${APP_ROOT}"/assets/fonts/*.txt "${LIC}/"
    (cd "${BUILD}/app" && zip -qr "Jelly5-${VER}.zip" "${TITLE_ID}" "${TITLE_ID}.ffpfsc" licenses)
    ok "release: ${BUILD#"${NUVIO_ROOT}/"}/app/Jelly5-${VER}.zip"
fi
ok "app: ${APPDIR#"${NUVIO_ROOT}/"}/"
