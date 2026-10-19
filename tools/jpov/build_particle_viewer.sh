#!/usr/bin/env bash
# =============================================================================
# build_particle_viewer.sh — 编译 jpov_particle_viewer（JPOV 粒子查看器）
#
# 用法： ./tools/jpov/build_particle_viewer.sh
# 产物： output/jpov_particle_viewer/jpov_particle_viewer
#
# 交互（需 DISPLAY/WSLg）： output/jpov_particle_viewer/jpov_particle_viewer
# headless 拍摄：           ...jpov_particle_viewer --capture <out_dir>
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_particle_viewer"

echo "==> 0. 输出目录"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "==> 1. 编译 jpov_particle_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_particle_viewer
cp -v "$BAZEL_BIN/jpov_particle_viewer" "$OUTPUT_DIR/jpov_particle_viewer"

echo "==> 2. 字体"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf" "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo "==> 3. 火/烟 flipbook 纹理"
mkdir -p "$OUTPUT_DIR/textures/candidates" "$OUTPUT_DIR/textures/mp" "$OUTPUT_DIR/textures/puffs" "$OUTPUT_DIR/textures/smoke"
cp -v "$PROJECT_DIR/tools/jpov/assets/fire/"*.png "$OUTPUT_DIR/textures/" 2>/dev/null || true
cp -v "$PROJECT_DIR/tools/jpov/assets/fire/candidates/"*.png "$OUTPUT_DIR/textures/candidates/" 2>/dev/null || true
cp -v "$PROJECT_DIR/tools/jpov/assets/fire/mp/"*.png "$OUTPUT_DIR/textures/mp/" 2>/dev/null || true
cp -v "$PROJECT_DIR/tools/jpov/assets/fire/puffs/"*.png "$OUTPUT_DIR/textures/puffs/" 2>/dev/null || true
cp -v "$PROJECT_DIR/tools/jpov/assets/smoke/"*.png "$OUTPUT_DIR/textures/smoke/" 2>/dev/null || true
ls -lh "$OUTPUT_DIR/textures"

echo ""
echo "============================================"
echo "  完成！"
echo "  交互： $OUTPUT_DIR/jpov_particle_viewer"
echo "  拍摄： $OUTPUT_DIR/jpov_particle_viewer --capture <out_dir>"
echo "============================================"
