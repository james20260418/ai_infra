#!/usr/bin/env bash
# =============================================================================
# build_jpov_strip_viewer.sh — 编译 jpov_strip_viewer（JPOV 半透明条带查看器）
#
# 用法： ./tools/jpov/build_jpov_strip_viewer.sh
# 产物： output/jpov_strip_viewer/jpov_strip_viewer
#
# 交互（需 DISPLAY/WSLg）： output/jpov_strip_viewer/jpov_strip_viewer
# headless 拍摄：           .../jpov_strip_viewer --capture <out_dir>
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_strip_viewer"

echo "==> 0. 输出目录"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "==> 1. 编译 jpov_strip_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_strip_viewer
cp -v "$BAZEL_BIN/jpov_strip_viewer" "$OUTPUT_DIR/jpov_strip_viewer"

echo "==> 2. 字体"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf" "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo "==> 3. 条带纹理"
mkdir -p "$OUTPUT_DIR/textures"
cp -v "$PROJECT_DIR/tools/jpov/assets/strip/ribbon_test.png" "$OUTPUT_DIR/textures/"

echo "==> 4. 橡树模型（玻璃缸天光场景用；与 fire-fog viewer 同款）"
mkdir -p "$OUTPUT_DIR/models"
cp -v "$PROJECT_DIR/tools/jpov/assets/models/samples/oak_negative/tripo_oak_4k.glb" \
      "$OUTPUT_DIR/models/"

echo ""
echo "============================================"
echo "  完成！"
echo "  交互： $OUTPUT_DIR/jpov_strip_viewer"
echo "  拍摄： $OUTPUT_DIR/jpov_strip_viewer --capture <out_dir>"
echo "============================================"
