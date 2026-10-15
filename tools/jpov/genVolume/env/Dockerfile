# genVolume 外部工具环境 —— Docker 镜像定义
#
# 环境用**容器镜像**管理：本目录只放镜像定义 + 依赖清单（dependency.txt），
# **不放任何"在宿主机装环境"的脚本**。
#
# 构建：docker build -t jpov-genvolume tools/jpov/genVolume/env
# 运行：docker run --rm -v "$PWD":/work -w /work/tools/jpov/genVolume jpov-genvolume <cmd>
#
# 依赖清单见同目录 dependency.txt；用法见 README.md。
# 首次构建会导致 Mantaflow 从源码编译（约 20~40 min）。
# syntax=docker/dockerfile:1
FROM ubuntu:24.04

ARG MANTAFLOW_TAG=0.13
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
        python3 python3-dev python3-numpy \
        libopenvdb-dev libtbb-dev zlib1g-dev \
        python3-openvdb \
        xvfb curl ca-certificates \
        blender \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04：-lHalf 已并入 Imath，而 Mantaflow 0.13 仍链 libHalf → 兼容软链。
RUN ln -sf /usr/lib/x86_64-linux-gnu/libImath-3_1.so.29 /usr/lib/x86_64-linux-gnu/libHalf.so || true

# Mantaflow standalone 0.13（装成 /usr/local/bin/manta）
RUN git clone --depth 1 --branch "${MANTAFLOW_TAG}" https://github.com/tum-pbs/mantaflow /opt/mantaflow \
    && mkdir -p /opt/mantaflow/build \
    && cd /opt/mantaflow/build \
    && cmake .. -DGUI=OFF -DOPENVDB=ON -DTBB=ON -DNUMPY=ON -DPYTHON_VERSION=3 -DCMAKE_BUILD_TYPE=Release \
    && make -j"$(nproc)" \
    && ln -sf /opt/mantaflow/build/manta /usr/local/bin/manta \
    && cp "$(find /opt/mantaflow -name waveletNoiseTile.bin | head -1)" /opt/mantaflow/waveletNoiseTile.bin

# manta 要求 cwd 含 waveletNoiseTile.bin → 包装器（镜像内内联生成）。
RUN printf '%s\n' \
        '#!/usr/bin/env bash' \
        'set -euo pipefail' \
        'MANTA_ROOT="${MANTA_ROOT:-/opt/mantaflow}"' \
        'cd "${MANTA_ROOT}"' \
        'exec manta "$@"' \
        > /usr/local/bin/manta-run \
    && chmod +x /usr/local/bin/manta-run

WORKDIR /work
CMD ["/bin/bash"]
