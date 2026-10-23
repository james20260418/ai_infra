#!/usr/bin/env bash
# =============================================================================
# make_generated_bark.sh — 生图模型出的「一张树皮图」→ viewer 认的树皮材质目录
#
# 用法：
#   ./make_generated_bark.sh <in.png> <out_dir> [normal_scale] [blend_ratio]
#
#   <in.png>        生图模型出的树皮（不需要提前做无缝、不需要法线/粗糙度）
#   <out_dir>       产物目录，viewer 用 --bark <out_dir> 直接吃
#   normal_scale    法线强度，默认 1.5（和 viewer 的 --normal-scale 默认一致）
#   blend_percent   无缝化时的边缘混合带宽（占短边的百分比，0~50），默认 15
#
# 产物（文件名固定，viewer 硬编码）：
#   albedo.png     ← 输入图（无变化：顺序是 seamless → normal → roughness）
#   normal.png     ← 由 albedo 的灰度当高度场算（Sobel）
#   roughness.png  ← 由 albedo 的亮度推：暗缝更粗糙（1 - luma，再拉对比）
#
# 为什么要无缝：树皮的 UV v 沿枝累加、u 每圈归零，两向都靠 GL_REPEAT 平铺。
# 图不无缝，每一圈都会出现一条硬缝（比"花纹不够好"难看得多）。
#
# 为什么 roughness 从 albedo 推：生图只给一张彩色图；树皮的暗部就是凹缝，
# 凹缝更粗糙 —— 这是一个够用的近似，总比全 0.5 的塑料感强。
#
# 依赖：bazel 构建的 texture_tool（先 bazel build，见下），以及 imagemagick。
# =============================================================================

set -euo pipefail

IN="${1:-}"
DST="${2:-}"
NORMAL_SCALE="${3:-1.5}"
BLEND_PERCENT="${4:-15}"

if [ -z "$IN" ] || [ -z "$DST" ]; then
    echo "用法: $0 <in.png> <out_dir> [normal_scale] [blend_percent]" >&2
    exit 2
fi
if [ ! -f "$IN" ]; then
    echo "ERROR: 输入图不存在: $IN" >&2
    exit 2
fi
if ! command -v convert >/dev/null 2>&1; then
    echo "ERROR: 需要 ImageMagick（convert）。Debian/Ubuntu: apt-get install -y imagemagick" >&2
    exit 2
fi

# 定位 texture_tool 的产物：优先环境变量，其次仓库根的 bazel-bin。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
TOOL_DIR="$REPO_ROOT/bazel-bin/tools/jpov/tools/texture_tool"
SEAMLESS="${SEAMLESS:-$TOOL_DIR/jpov_texture_seamless}"
NORMAL="${NORMAL:-$TOOL_DIR/jpov_texture_normal}"

if [ ! -x "$SEAMLESS" ] || [ ! -x "$NORMAL" ]; then
    echo "ERROR: 找不到 texture_tool 的可执行文件。" >&2
    echo "  先构建：cd $REPO_ROOT && bazel build //tools/jpov/tools/texture_tool:all" >&2
    echo "  或用 SEAMLESS=/path/to/... NORMAL=/path/to/... 覆盖" >&2
    exit 2
fi

mkdir -p "$DST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> albedo.png    ← $(basename "$IN")"
convert "$IN" "$TMP/raw.png"

# 无缝化：带宽按短边百分比给（太小仍有缝、太大把花纹糊成一团）。
read -r W H < <(identify -format '%w %h\n' "$TMP/raw.png")
SHORT=$(( W < H ? W : H ))
if ! [[ "$BLEND_PERCENT" =~ ^[0-9]+$ ]] || [ "$BLEND_PERCENT" -gt 50 ]; then
    echo "ERROR: blend_percent 必须是 0~50 的整数（百分比），实际: $BLEND_PERCENT" >&2
    exit 2
fi
BLEND=$(( SHORT * BLEND_PERCENT / 100 ))
[ "$BLEND" -lt 1 ] && BLEND=1
echo "==> seamless      （${W}x${H}，带宽 ${BLEND}px）"
"$SEAMLESS" "$TMP/raw.png" "$DST/albedo.png" "$BLEND" blend

echo "==> normal.png    （Sobel，强度 ${NORMAL_SCALE}）"
"$NORMAL" "$DST/albedo.png" "$DST/normal.png" "$NORMAL_SCALE"

echo "==> roughness.png （由亮度推：暗 = 更粗糙）"
convert "$DST/albedo.png" -colorspace Gray -negate -level 20%,95% "$DST/roughness.png"

read -r W H < <(identify -format '%w %h\n' "$DST/albedo.png")
echo
echo "完成: $DST  （${W}x${H}）"
if [ "$W" -ne "$H" ]; then
    echo "长宽比非 1:1 —— viewer 会按 宽/高 自动补偿 uv_v_scale（$(awk "BEGIN{printf \"%.4f\", $W/$H}")）"
fi
echo
echo "运行："
echo "  cd output/jpov_vegetation_v2_viewer"
echo "  ./jpov_vegetation_v2_viewer --shape ../veg_shapes/pine.pbtxt --bark $DST \\"
echo "      --leaf leaf/willow_leaf.png --leaf2 leaf/willow_leaf_flip.png --leaf-black 32"
