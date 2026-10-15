#!/usr/bin/env bash
# setup_dev_env.sh — 在裸机 / 容器内手装 JPOV genVolume 的外部工具环境。
# 与 env/Dockerfile 等价（容器里也可直接跑本脚本）。需要 sudo/root。
#
# 装了：Mantaflow 0.13 + pyopenvdb + numpy + Blender + xvfb。
# 详细说明见同目录 README.md。
set -euo pipefail

MANTA_ROOT="${MANTA_ROOT:-/opt/mantaflow}"
MANTAFLOW_TAG="${MANTAFLOW_TAG:-0.13}"
SUDO="${SUDO:-}"
if [[ "$(id -u)" -ne 0 ]]; then
    SUDO="sudo"
fi

echo "==> apt 依赖"
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
    build-essential cmake git \
    python3 python3-dev python3-numpy \
    libopenvdb-dev libtbb-dev zlib1g-dev \
    python3-openvdb \
    xvfb curl ca-certificates \
    blender

# Ubuntu 24.04：-lHalf 已并入 Imath → 兼容软链（Mantaflow 0.13 仍链 libHalf）
if [[ ! -e /usr/lib/x86_64-linux-gnu/libHalf.so ]]; then
    $SUDO ln -sf /usr/lib/x86_64-linux-gnu/libImath-3_1.so.29 /usr/lib/x86_64-linux-gnu/libHalf.so || true
fi

echo "==> 编译 Mantaflow ${MANTAFLOW_TAG} → ${MANTA_ROOT}"
if [[ ! -d "${MANTA_ROOT}" ]]; then
    $SUDO git clone --depth 1 --branch "${MANTAFLOW_TAG}" https://github.com/tum-pbs/mantaflow "${MANTA_ROOT}"
fi
$SUDO mkdir -p "${MANTA_ROOT}/build"
$SUDO bash -c "cd '${MANTA_ROOT}/build' && cmake .. -DGUI=OFF -DOPENVDB=ON -DTBB=ON -DNUMPY=ON -DPYTHON_VERSION=3 -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)"
$SUDO ln -sf "${MANTA_ROOT}/build/manta" /usr/local/bin/manta
$SUDO bash -c "cp \"\$(find '${MANTA_ROOT}' -name waveletNoiseTile.bin | head -1)\" '${MANTA_ROOT}/waveletNoiseTile.bin'"

echo "==> 安装 manta-run 包装器"
$SUDO cp "$(dirname "$0")/manta-run" /usr/local/bin/manta-run
$SUDO chmod +x /usr/local/bin/manta-run

echo "==> 自检"
python3 -c "import numpy; print('numpy', numpy.__version__)"
python3 -c "import pyopenvdb; print('pyopenvdb ok')" || echo "  ! pyopenvdb 未就绪"
manta --help >/dev/null 2>&1 && echo "manta ok" || echo "  ! manta 未就绪"
blender --version 2>/dev/null | head -1 || echo "  ! blender 未就绪"
echo "完成。下一步：python3 -m unittest discover -s tools/jpov/genVolume -p 'test_*.py' -v"
