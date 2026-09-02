#!/usr/bin/env bash
# =============================================================================
# gen3d.sh — JPOV 自动生成静态 3D 模型全链路（编译 + tripo3d 生成 + JPOV 渲染验证）
#
# 编排职责（本脚本不发 HTTP，只调各 elf）：
#   步骤0: bazel 编译 gen3d_cmd（调 tripo3d API 生成 GLB）+ jpov_model_viewer
#   步骤1: gen3d_cmd   → tripo3d 生成 .glb → output_dir/<name>.glb
#   步骤2: model_viewer --four_views --output_dir → <name>_{front,up,left,perspective}.png
#   收尾:  打印产物清单（glb + 4 张 png 的绝对路径），防用户对落盘位置 confuse
#
# 用法（output_dir 是第一直觉入口）：
#   ./tools/jpov/gen3d.sh <output_dir> <name> --prompt "..." [更多 gen3d_option]
#   例:
#     ./tools/jpov/gen3d.sh output/gen3d chair --prompt "一把中世纪木椅"
#   - <output_dir> 不存在会自动创建
#   - 除 output_dir / name 外，其余参数原样透传给 gen3d_cmd（--prompt/--triangles
#     [--high_poly] 等）。缺省走 tripo P1 低模 4000 面。
#   - 步骤1用 gen3d_cmd 从其 stdout 末行取 .glb 绝对路径；失败即中止。
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_ROOT="$PROJECT_DIR/output"

# ---- 必填: output_dir + name（本脚本的核心，最终产物统一落这里）----
if [ $# -lt 2 ]; then
    echo "用法: $0 <output_dir> <name> --prompt \"...\" [gen3d 参数...]" >&2
    echo "例:   $0 output/gen3d chair --prompt \"一把中世纪木椅\"" >&2
    exit 1
fi
OUTPUT_DIR="$1"; shift
NAME="$1"; shift

# 允许相对路径的 output_dir——以工程根为基准落到绝对路径，产物打印更友好。
case "$OUTPUT_DIR" in
    /*) : ;;                    # 已是绝对路径
    *)  OUTPUT_DIR="$PROJECT_DIR/$OUTPUT_DIR" ;;
esac
mkdir -p "$OUTPUT_DIR"
echo "==> 产物目录: $OUTPUT_DIR"

if [ -z "${TRIPO_API_KEY:-}" ]; then
    echo "错误: 环境变量 TRIPO_API_KEY 未设置（调 tripo3d 需 API key）" >&2
    exit 1
fi

# ---- 步骤0: 编译两个 bin（首次较慢，产物缓存于 bazel-bin/-Bazel 增量）----
echo ""
echo "==> 0. 编译 gen3d_cmd + jpov_model_viewer"
cd "$PROJECT_DIR"
# 静默成功，出错时输出日志尾部
if ! bazel build //tools/jpov/gen3d:gen3d_cmd \
    //tools/jpov:jpov_model_viewer 2>/tmp/gen3d_bazel_err.log; then
    echo "bazel 编译失败，日志尾部:" >&2
    tail -30 /tmp/gen3d_bazel_err.log >&2
    exit 1
fi
GEN3D_CMD="$PROJECT_DIR/bazel-bin/tools/jpov/gen3d/gen3d_cmd"
VIEWER="$PROJECT_DIR/bazel-bin/tools/jpov/jpov_model_viewer"

# ---- 步骤1: gen3d_cmd 生成 GLB（额外传 --output_dir/--name）+ 捕获产物路径 ----
echo ""
echo "==> 1. tripo3d 生成模型 (name=$NAME)..."
# stdout 末行是 glb 绝对路径（gen3d_cmd 明确打印），缓存它。
GLB_PATH="$("$GEN3D_CMD" generate \
    --name "$NAME" --output_dir "$OUTPUT_DIR" "$@" 2>/dev/null | tail -1)"
if [ -z "$GLB_PATH" ] || [ ! -f "$GLB_PATH" ]; then
    echo "错误: gen3d_cmd 未产出 GLB（见上面日志）" >&2
    exit 1
fi
echo "GLB 产物: $GLB_PATH"

# ---- 步骤2: model viewer four_views 出图到同一个 output_dir（C 项新参数）----
echo ""
echo "==> 2. JPOV 渲染 4 视图 (front/up/left/perspective)"
# model viewer 需 GL 上下文。headless 需 DISPLAY（Xvfb）；无人值守环境若
# 无 DISPLAY 但装 Xvfb 则自动拉起（避免直接 abort，JPOV headless RunOnce 走 llvmpipe）。
if [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
    Xvfb :99 -screen 0 1280x720x24 >/tmp/gen3d_xvfb.log 2>&1 &
    # 等 X server 就绪（最多 ~3s）。
    for _ in $(seq 1 10); do
        DISPLAY=:99 xdpyinfo >/dev/null 2>&1 && break
        sleep 0.3
    done
    export DISPLAY=:99
    echo "     (无 DISPLAY，已自动拉起 Xvfb :99)"
fi
# model viewer 需要 exe 同目录的 fonts/（CJK）。bazel-bin 产物不带 fonts/，
# 因此临时把字体拷到 viewer 同目录。
FONT_DIR="$PROJECT_DIR/tools/jpov/fonts"
VIEWER_FONTS="$(dirname "$VIEWER")/fonts"
if [ ! -d "$VIEWER_FONTS" ]; then
    mkdir -p "$VIEWER_FONTS"
    cp "$FONT_DIR/DejaVuSans.ttf" "$VIEWER_FONTS/"
    cp "$FONT_DIR/NotoSansCJK-Regular.ttc" "$VIEWER_FONTS/" 2>/dev/null || true
fi
"$VIEWER" --four_views --output_dir "$OUTPUT_DIR" "$GLB_PATH"
echo ""
echo "==> JPOV 已渲染完成"

# ---- 收尾: 产物清单（绝对路径，防 confuse）----
echo ""
echo "============================================"
echo "  gen3d 产物清单 (output_dir = $OUTPUT_DIR)"
echo "============================================"
echo "  GLB: $GLB_PATH"
for v in front up left perspective; do
    f="$OUTPUT_DIR/${NAME}_${v}.png"
    echo "  PNG: $f"
done
