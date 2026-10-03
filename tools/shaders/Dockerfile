# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# AMD's LLPC pipeline compiler (amdllpc) for the PS5 GPU (gfx1013), to build
# app/shaders/*.pipe. Adapted from EVO Player's Dockerfile.amdllpc (GPL-3.0),
# on a plain Ubuntu base instead of EVO's private image.
#
# x86-64 only: AMDVLK's build wants g++-multilib and DirectX's dxc, both x86.
# On Apple silicon it runs under Rosetta (docker --platform linux/amd64).
# The LLVM-scale build takes a long while once; the image keeps only amdllpc.
#
#   tools/shaders/build.sh            (builds the image if needed, then the pipes)

FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake ninja-build build-essential python3 python3-yaml \
      pkg-config curl ca-certificates libssl-dev zlib1g-dev llvm-18 \
    && rm -rf /var/lib/apt/lists/*

ARG AMDVLK_TAG=refs/tags/v-2025.Q2.1
ENV AMDLLPC_SRC=/opt/amdllpc
RUN curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo -o /usr/local/bin/repo \
    && chmod +x /usr/local/bin/repo
WORKDIR ${AMDLLPC_SRC}
RUN git config --global user.email "build@jelly5.local" \
    && git config --global user.name "Jelly5 build" \
    && git config --global color.ui false \
    && repo init -u https://github.com/GPUOpen-Drivers/AMDVLK.git \
         -b ${AMDVLK_TAG} -m build_with_tools.xml --depth=1 \
    && repo sync -j4 -c --no-tags --no-clone-bundle

# AMDVLK's documented driver-build dependencies (xgl's configure needs them even
# when only the compiler is built).
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-multilib gcc-multilib python3-jinja2 python3-ruamel.yaml \
      libwayland-dev libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev \
      libxcb-dri3-dev libxcb-dri2-0-dev libxcb-present-dev \
      libxcb-randr0-dev libxcb-sync-dev libxcb-shm0-dev libxcb-glx0-dev \
      libxcb-xfixes0-dev libxshmfence-dev libxrandr-dev libdrm-dev \
      x11proto-dri2-dev \
    && rm -rf /var/lib/apt/lists/*

# Public LLPC does not list gfx1013 (the PS5's GPU); LLVM's AMDGPU backend knows it.
COPY patches/llpc-add-gfx1013.py /tmp/llpc-add-gfx1013.py
RUN python3 /tmp/llpc-add-gfx1013.py ${AMDLLPC_SRC}/drivers/llpc/lgc/state/TargetInfo.cpp \
    && grep -q 'gfx1013' ${AMDLLPC_SRC}/drivers/llpc/lgc/state/TargetInfo.cpp

# gfxruntime's configure hard-requires DirectX's dxc (nothing here goes through it).
ARG DXC_RELEASE=v1.9.2607
ARG DXC_TARBALL=linux_dxc_2026_07_29.x86_x64.tar.gz
RUN curl -fsSL -o /tmp/dxc.tar.gz \
      "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_RELEASE}/${DXC_TARBALL}" \
    && mkdir -p /opt/dxc && tar -xzf /tmp/dxc.tar.gz -C /opt/dxc && rm /tmp/dxc.tar.gz \
    && find /opt/dxc -name dxc -type f -exec chmod +x {} \; \
    && find /opt/dxc -name "libdxcompiler.so*" -exec install -m 0755 {} /usr/local/lib/ \; \
    && ldconfig \
    && ln -sf "$(find /opt/dxc -name dxc -type f -perm -u+x | head -1)" /usr/local/bin/dxc \
    && test -x /usr/local/bin/dxc

# Four compile jobs and one link: the AMDGPU backend's largest units need 2-4 GB each.
RUN cmake -G Ninja -S ${AMDLLPC_SRC}/drivers/xgl -B ${AMDLLPC_SRC}/build \
      -DCMAKE_BUILD_TYPE=Release -DVKI_BUILD_TOOLS=ON \
      -DVKI_RAY_TRACING=OFF -DLLPC_RAY_TRACING=OFF \
      -DLLVM_PARALLEL_COMPILE_JOBS=4 -DLLVM_PARALLEL_LINK_JOBS=1 \
    && cmake --build ${AMDLLPC_SRC}/build --target amdllpc -j4 \
    && find ${AMDLLPC_SRC}/build -name amdllpc -type f -perm -u+x -exec install -m 0755 {} /usr/local/bin/amdllpc \; \
    && find ${AMDLLPC_SRC} -name "libspvgen*.so" -exec install -m 0755 {} /usr/local/lib/ \; \
    && ldconfig && test -x /usr/local/bin/amdllpc && amdllpc --version \
    && rm -rf ${AMDLLPC_SRC}

WORKDIR /workspace
