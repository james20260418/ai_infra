#!/usr/bin/env bash
# =============================================================================
# build_jpov_wind_viewer.sh — 编译 jpov_wind_viewer（JPOV 植被风查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_wind_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_wind_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_wind_viewer/
#   3. 拷贝分发态字体到 exe 旁 fonts/
#   4. 拷贝小柳树 GLB（模特）到 exe 旁
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_wind_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_wind_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_wind_viewer 2>&1

echo ""
echo "==> 2. 拷贝产物"
cp -v "$BAZEL_BIN/jpov_wind_viewer" "$OUTPUT_DIR/jpov_wind_viewer"

echo ""
echo "==> 3. 拷贝字体到 exe 旁 fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo ""
echo "==> 4. 拷贝小柳树 GLB（模特）到 exe 旁"
cp -v "$PROJECT_DIR/tools/jpov/tools/vegetation/samples/willow_wind_tex.glb" \
      "$OUTPUT_DIR/willow_wind_tex.glb"

ls -lh "$OUTPUT_DIR/"
echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_wind_viewer"
echo "     面板：风向方位角 / 风速 / 振幅 / 频率 / 湍流 / 时间缩放 / 株距 / 每株扰动 / 株数 / 模型缩放。"
echo "     右键 drag 转视角、滚轮 zoom。"
echo ""
echo "   headless 拍摄（关风参照 + 逐秒开风帧）："
echo "       $OUTPUT_DIR/jpov_wind_viewer --capture-wind <out_dir> [--distance <r>] [--frames <n>]"
