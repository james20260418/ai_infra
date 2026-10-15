# JPOV genVolume 外部工具环境（dev-time）：Mantaflow + Blender + pyopenvdb。
#
# 用途：给离线资产生成提供**可复现、统一管理**的工具箱（Danis 2026-10-16 令）。
# 详细说明见同目录 README.md。构建：docker build -t jpov-genvolume tools/jpov/genVolume/env
#
# ⚠️ 本文件在编写环境里**未实际构建过**（该容器无 docker）；首次构建若报错请按日志微调。
# 编译期大头是 Mantaflow（约 20~40 min）。
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

# Ubuntu 24.04：-lHalf 已并入 Imath，而 Mantaflow 0.13 仍链 libHalf → 建兼容软链。
RUN ln -sf /usr/lib/x86_64-linux-gnu/libImath-3_1.so.29 /usr/lib/x86_64-linux-gnu/libHalf.so || true

# ---- Mantaflow standalone 0.13（编译产物装到 /usr/local/bin/manta）----
RUN git clone --depth 1 --branch "${MANTAFLOW_TAG}" https://github.com/tum-pbs/mantaflow /opt/mantaflow \
    && mkdir -p /opt/mantaflow/build \
    && cd /opt/mantaflow/build \
    && cmake .. -DGUI=OFF -DOPENVDB=ON -DTBB=ON -DNUMPY=ON -DPYTHON_VERSION=3 -DCMAKE_BUILD_TYPE=Release \
    && make -j"$(nproc)" \
    && ln -sf /opt/mantaflow/build/manta /usr/local/bin/manta \
    && cp "$(find /opt/mantaflow -name waveletNoiseTile.bin | head -1)" /opt/mantaflow/waveletNoiseTile.bin

# manta 要求 cwd 含 waveletNoiseTile.bin → 包装器自动切目录。
# 注意：COPY 的是同级 manta-run（单一来源）。
COPY manta-run /usr/local/bin/manta-run
RUN chmod +x /usr/local/bin/manta-run

WORKDIR /work
CMD ["/bin/bash"]
