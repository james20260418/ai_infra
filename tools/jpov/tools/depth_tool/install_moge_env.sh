#!/usr/bin/env bash
# ============================================================================
# JPOV depth tool — MoGe-3 运行环境安装
#
# 只做三件事：建 venv → 装 torch(CUDA) → 装 MoGe（钉在指定 commit）。
# 所有缓存（pip wheel / HF 权重 / 临时文件）都落在 env 目录里，
# 所以把 --env-dir 指到挂载盘就不会撑大镜像层。
#
# 用法:
#   ./install_moge_env.sh --env-dir /path/on/mounted/disk/moge
#   JPOV_MOGE_ENV_DIR=/data/envs/moge ./install_moge_env.sh
#
# 可用环境变量覆盖:
#   JPOV_PYTHON_BIN         python 解释器（默认 python3，需 >= 3.10）
#   JPOV_TORCH_VERSION      torch 版本（默认 2.14.1）
#   JPOV_TORCHVISION_VERSION torchvision 版本（默认 0.29.1）
#   JPOV_TORCH_INDEX_URL    torch wheel 源（默认 cu130；cu128 卡用 cu128）
#   JPOV_MOGE_COMMIT        MoGe 提交号（默认钉住已验证版本）
#   HF_ENDPOINT             不设则自动探测，huggingface.co 不通时退回 hf-mirror.com
# ============================================================================
set -euo pipefail

ENV_DIR="${JPOV_MOGE_ENV_DIR:-${HOME}/jpov_envs/moge}"
PYTHON_BIN="${JPOV_PYTHON_BIN:-python3}"
TORCH_VERSION="${JPOV_TORCH_VERSION:-2.14.1}"
TORCHVISION_VERSION="${JPOV_TORCHVISION_VERSION:-0.29.1}"
TORCH_INDEX_URL="${JPOV_TORCH_INDEX_URL:-https://download.pytorch.org/whl/cu130}"
MOGE_COMMIT="${JPOV_MOGE_COMMIT:-74fbce054ebed49800de42d0ad0e83495065719a}"

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --env-dir)
            [[ -n "${2:-}" ]] || { echo "error: --env-dir 需要参数" >&2; exit 2; }
            ENV_DIR="$2"
            shift 2
            ;;
        --torch-index-url)
            [[ -n "${2:-}" ]] || { echo "error: --torch-index-url 需要参数" >&2; exit 2; }
            TORCH_INDEX_URL="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: 未知参数 '$1'（-h 看用法）" >&2
            exit 2
            ;;
    esac
done

VENV_DIR="${ENV_DIR}/venv"
PY="${VENV_DIR}/bin/python"

echo "== env dir      : ${ENV_DIR}"
echo "== torch        : ${TORCH_VERSION} / torchvision ${TORCHVISION_VERSION}"
echo "== torch index  : ${TORCH_INDEX_URL}"
echo "== moge commit  : ${MOGE_COMMIT}"

# 所有缓存写进 env 目录，避免落到镜像可写层
export PIP_CACHE_DIR="${ENV_DIR}/.cache/pip"
export HF_HOME="${ENV_DIR}/.cache/huggingface"
export TMPDIR="${ENV_DIR}/tmp"
mkdir -p "${PIP_CACHE_DIR}" "${HF_HOME}" "${TMPDIR}"

# HF 直连探测：很多内网/容器环境访问不到 huggingface.co
if [[ -z "${HF_ENDPOINT:-}" ]]; then
    if curl -sS -o /dev/null --max-time 5 https://huggingface.co 2>/dev/null; then
        echo "== HF           : 直连可用"
    else
        export HF_ENDPOINT="https://hf-mirror.com"
        echo "== HF           : 直连不可用，改用 ${HF_ENDPOINT}"
    fi
else
    echo "== HF           : 使用已有 HF_ENDPOINT=${HF_ENDPOINT}"
fi

if [[ ! -x "${PY}" ]]; then
    echo "== 创建 venv    : ${VENV_DIR}"
    "${PYTHON_BIN}" -m venv --copies "${VENV_DIR}"
fi

"${PY}" -m pip install --quiet --upgrade pip wheel setuptools

echo "== 安装 torch（数百 MB，耐心等）"
"${PY}" -m pip install --index-url "${TORCH_INDEX_URL}" \
    "torch==${TORCH_VERSION}" "torchvision==${TORCHVISION_VERSION}"

echo "== 安装 MoGe @ ${MOGE_COMMIT:0:7}"
"${PY}" -m pip install "git+https://github.com/microsoft/MoGe.git@${MOGE_COMMIT}"

echo "== 自检"
HF_ENDPOINT="${HF_ENDPOINT:-}" "${PY}" - <<'PY'
import sys

import torch

print("python  ", sys.version.split()[0])
print("torch   ", torch.__version__)
print("cuda    ", torch.cuda.is_available())
if not torch.cuda.is_available():
    # 没有 CUDA 不是安装错误：moge_depth.py --device cpu 仍能跑，只是很慢
    print("warning : CUDA 不可用，moge_depth.py 只能用 --device cpu")
    raise SystemExit(0)
print("device  ", torch.cuda.get_device_name(0), "cap", torch.cuda.get_device_capability(0))
mat = torch.randn(512, 512, device="cuda")
print("matmul  ", float((mat @ mat).sum()))
PY

cat > "${ENV_DIR}/env.sh" <<EOF
# 由 install_moge_env.sh 生成：source 它就得到正确的缓存路径与镜像源
export HF_HOME="${HF_HOME}"
EOF

if [[ -n "${HF_ENDPOINT:-}" ]]; then
    echo "export HF_ENDPOINT=\"${HF_ENDPOINT}\"" >> "${ENV_DIR}/env.sh"
fi

cat <<EOF

安装完成。

  env      : ${VENV_DIR}
  python   : ${PY}
  权重缓存 : ${HF_HOME}
  pip 缓存 : ${PIP_CACHE_DIR}
  环境脚本 : ${ENV_DIR}/env.sh

用法（在仓库根目录）：

  cd <repo_root>/tools/jpov/tools/depth_tool
  . ${ENV_DIR}/env.sh            # 关键：把 HF_HOME 指到挂载盘，别下到镜像里
  . ${VENV_DIR}/bin/activate
  python moge_depth.py ../../../../test/xxx.png --out-dir out/depth

注意：venv 里写的是绝对路径，env 目录被移动/重命名后需要重建。
EOF
