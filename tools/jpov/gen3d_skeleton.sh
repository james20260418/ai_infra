#!/usr/bin/env bash
# =============================================================================
# gen3d_skeleton.sh — JPOV 带骨骼 3D 模型生成（编译 + tripo3d Auto Rig）
#
# 对应 gen3d/skeleton/；生成带 mixamo 兼容骨骼（mixamorig）的 .glb。
# 与 gen3d_static.sh（纯静态 PBR）并列的孪生链路。Tripo v3 两步：
#   text-to-model(静态) → Auto Rig(spec=mixamo) → 下载带骨骼 GLB。
#
# 用法（output_dir + name + prompt，跟 gen3d_static.sh 一致的习惯）：
#   ./tools/jpov/gen3d_skeleton.sh <output_dir> <name> --prompt "..." [gen3d_options]
#   例:
#     ./tools/jpov/gen3d_skeleton.sh output/gen3d_skeleton male_figure \
#         --prompt "bare-chested athletic male human figure, wearing plain underwear shorts, \
#         bald head, neutral standing pose, clearly defined torso muscle and limb joint \
#         proportions (a riggable biped humanoid)"
#   其它可选参：
#     --input_task_id <task_xxx>  跳过 text-to-model，直接对已有模型任务 rig
#     --spec tripo|mixamo         默认 mixamo（mixamorig 骨名）
#     --rig_type biped|quadruped|...  默认 biped
#   - 除 output_dir / name / prompt 外，其余参数原样透传给 gen3d_skeleton_cmd
#     （--negative/--input_task_id/--spec/--rig_type 等）。
#   - 产物 = <output_dir>/<name>.glb（带 mixamorig 骨骼），脚本打印其绝对路径。
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [ $# -lt 2 ]; then
    echo "用法: $0 <output_dir> <name> --prompt \"...\" [额外参数...]" >&2
    echo "例:   $0 output/gen3d_skeleton man --prompt \"athletic male, underwear, bald\"" >&2
    exit 1
fi
OUTPUT_DIR="$1"; shift
NAME="$1"; shift

case "$OUTPUT_DIR" in
    /*) : ;;
    *)  OUTPUT_DIR="$PROJECT_DIR/$OUTPUT_DIR" ;;
esac
mkdir -p "$OUTPUT_DIR"
echo "==> 产物目录: $OUTPUT_DIR"

if [ -z "${TRIPO_API_KEY:-}" ]; then
    echo "错误: 环境变量 TRIPO_API_KEY 未设置（调 tripo3d 需 API key）" >&2
    exit 1
fi

echo ""
echo "==> 0. 编译 gen3d_skeleton_cmd"
cd "$PROJECT_DIR"
if ! bazel build //tools/jpov/gen3d/skeleton:gen3d_skeleton_cmd \
    2>/tmp/gen3d_skel_bazel_err.log; then
    echo "bazel 编译失败，日志尾部:" >&2
    tail -30 /tmp/gen3d_skel_bazel_err.log >&2
    exit 1
fi
CMD="$PROJECT_DIR/bazel-bin/tools/jpov/gen3d/skeleton/gen3d_skeleton_cmd"

echo ""
echo "==> 1. tripo3d 生成带骨骼模型 (name=$NAME)..."
# 不重定向 stderr(诊断/glog 打 task_id 与下载 URL，失败时据此手动抢救，如 GET /tasks/{id}
# 或直接 curl 签名 model_url)。stdout 仅一行：产物 .glb 绝对路径（main 的 printf）。
GLB_PATH="$("$CMD" generate \
    --name "$NAME" --output_dir "$OUTPUT_DIR" "$@" || true )"
GLB_PATH="$(printf '%s' "$GLB_PATH" | tail -1)"
if [ -z "$GLB_PATH" ] || [ ! -f "$GLB_PATH" ]; then
    echo "错误: gen3d_skeleton_cmd 未产出带骨骼 GLB" >&2
    echo "      若任务其实成功，可从上方的 task_id/下载 URL 手动抢救。" >&2
    exit 1
fi

echo ""
echo "============================================"
echo "  gen3d_skeleton 产物 (含 mixamorig 骨骼)"
echo "============================================"
echo "  GLB: $GLB_PATH"
echo ""
echo "说明: 渲染侧暂不驱动骨骼(loader 未接 skin)。可先落盘, 供后续 loader 接骨骼后渲染验证。"
