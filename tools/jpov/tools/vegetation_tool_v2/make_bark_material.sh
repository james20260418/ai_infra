#!/usr/bin/env bash
# =============================================================================
# make_bark_material.sh — 把 ambientCG 下载的 JPG 转成 viewer 认的树皮材质目录
#
# 用法：
#   ./make_bark_material.sh <src_dir> <out_dir>
#
#   <src_dir>  含 Bark001_1K-JPG_{Color,NormalGL,Roughness}.jpg 的目录
#              （tools/vegetation/fetch_textures.sh 下下来的就是）
#   <out_dir>  产物目录，viewer 用 --bark <out_dir> 直接吃
#
# 产物（文件名固定，viewer 硬编码）：
#   albedo.png     ← *_Color.jpg
#   normal.png     ← *_NormalGL.jpg   （OpenGL 约定：绿 = +v）
#   roughness.png  ← *_Roughness.jpg  （灰度，采 .r）
#
# 为什么不做成"运行时读 JPG"：材质目录是**资产**，要能入库、能被别的工具复用；
# 转换只做一次。另外 PNG 无损、没有 JPEG 色度抽样在法线贴图上造成的块状噪声。
# =============================================================================

set -euo pipefail

SRC="${1:-}"
DST="${2:-}"

if [ -z "$SRC" ] || [ -z "$DST" ]; then
    echo "用法: $0 <src_dir> <out_dir>" >&2
    exit 2
fi
if [ ! -d "$SRC" ]; then
    echo "ERROR: 源目录不存在: $SRC" >&2
    exit 2
fi
if ! command -v convert >/dev/null 2>&1; then
    echo "ERROR: 需要 ImageMagick（convert）。Debian/Ubuntu: apt-get install -y imagemagick" >&2
    exit 2
fi

# 只认 *_Color / *_NormalGL / *_Roughness 一个文件；多余的（Displacement / .blend 等）忽略。
pick() {
    local pattern="$1"
    local f
    f="$(find "$SRC" -maxdepth 1 -type f -name "$pattern" | sort | head -n 1)"
    if [ -z "$f" ]; then
        echo "ERROR: 在 $SRC 找不到 $pattern" >&2
        exit 2
    fi
    printf '%s' "$f"
}

COLOR="$(pick '*_Color.jpg')"
NORMAL="$(pick '*_NormalGL.jpg')"
ROUGH="$(pick '*_Roughness.jpg')"

mkdir -p "$DST"
echo "==> albedo.png    <- $(basename "$COLOR")"
convert "$COLOR"  "$DST/albedo.png"
echo "==> normal.png    <- $(basename "$NORMAL")"
convert "$NORMAL" "$DST/normal.png"
echo "==> roughness.png <- $(basename "$ROUGH")"
# 灰度化：roughness 只采 .r，留着彩色通道只会让文件大三倍。
convert "$ROUGH" -colorspace Gray "$DST/roughness.png"

# 长宽比提示：UV 的 v 必须按 宽/高 缩放（否则花纹被压扁、法线细节变糊）。
read -r W H < <(identify -format '%w %h' "$DST/albedo.png")
echo
echo "完成: $DST"
echo "贴图尺寸: ${W}x${H}"
if [ "$W" -eq "$H" ]; then
    echo "长宽比 1:1 —— uv_v_scale = 1.0（无需补偿）"
else
    echo "长宽比非 1:1 —— viewer 会自动按 宽/高 补偿（uv_v_scale = $(awk "BEGIN{printf \"%.4f\", $W/$H}")）"
fi
echo
echo "运行："
echo "  ./jpov_vegetation_v2_viewer --shape shapes/willow.vegshape --bark $DST"
