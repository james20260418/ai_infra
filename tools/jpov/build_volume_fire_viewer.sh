#!/usr/bin/env bash
# =============================================================================
# build_volume_fire_viewer.sh — 编译 jpov_volume_fire_viewer（JPOV 体积火查看器）
#
# 用法： bash tools/jpov/build_volume_fire_viewer.sh
# 产物： output/jpov_volume_fire_viewer/jpov_volume_fire_viewer
#
# 交互（需 DISPLAY/WSLg）： output/jpov_volume_fire_viewer/jpov_volume_fire_viewer
# headless 拍摄：           ...jpov_volume_fire_viewer --capture <out_dir>
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_volume_fire_viewer"

echo "==> 0. 输出目录"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "==> 1. 编译 jpov_volume_fire_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_volume_fire_viewer
cp -v "$BAZEL_BIN/jpov_volume_fire_viewer" "$OUTPUT_DIR/jpov_volume_fire_viewer"

echo "==> 2. 字体"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf" "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo "==> 3. 体积火资产"
mkdir -p "$OUTPUT_DIR/volumes"
# 唯一的体积资产：篝火（离线烘焙好、已入库；重烘见 docs/jpov_volumetric_fire_design.md）
cp -v "$PROJECT_DIR/data/fire_vdb/campfire.jpovvol" "$OUTPUT_DIR/volumes/"
ls -lh "$OUTPUT_DIR/volumes" || true

echo ""
echo "============================================"
echo "  完成！"
echo "  交互： $OUTPUT_DIR/jpov_volume_fire_viewer"
echo "  拍摄： $OUTPUT_DIR/jpov_volume_fire_viewer --capture <out_dir>"
echo "============================================"
