#!/usr/bin/env bash
# =============================================================================
# build_jpov_sp_ui_demo.sh — 编译 jpov_sp_ui_demo（sp_ui 板材调试台）的 Linux 和 Windows 产物
#
# 用法：
#   ./tools/jpov/build_jpov_sp_ui_demo.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_sp_ui_demo（Linux ELF）
#   2. bazel build //tools/jpov:jpov_sp_ui_demo.exe --config=windows（Windows PE）
#   3. 产物 + 字体 + 材质纹理拷到 output/jpov_sp_ui_demo/
#   4. 打印用法提示
#
# 为什么要拷纹理：demo 用 exe 相对路径找材质
# （textures/ → 同目录 → ../assets/sp_ui/marble/），产物目录里放一份最省事。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_sp_ui_demo"
MARBLE_DIR="$PROJECT_DIR/tools/jpov/assets/sp_ui/marble"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_sp_ui_demo"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_sp_ui_demo 2>&1
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 编译 Windows 版 jpov_sp_ui_demo.exe（MinGW 交叉编译）"
bazel build //tools/jpov:jpov_sp_ui_demo.exe --config=windows 2>&1
echo "OK: Windows 编译完成"

echo ""
echo "==> 3. 拷贝产物到 output/jpov_sp_ui_demo/"
cp -v "$BAZEL_BIN/jpov_sp_ui_demo"     "$OUTPUT_DIR/jpov_sp_ui_demo"
cp -v "$BAZEL_BIN/jpov_sp_ui_demo.exe" "$OUTPUT_DIR/jpov_sp_ui_demo.exe"

echo "==> 4. 拷贝字体到 output/jpov_sp_ui_demo/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"           "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo "==> 5. 拷贝材质纹理到 output/jpov_sp_ui_demo/textures/"
mkdir -p "$OUTPUT_DIR/textures"
cp -v "$MARBLE_DIR/plate_marble_flipbook_4x4.png" "$OUTPUT_DIR/textures/"

ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/fonts/" "$OUTPUT_DIR/textures/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_sp_ui_demo       (Linux ELF)"
echo "   $OUTPUT_DIR/jpov_sp_ui_demo.exe   (Windows PE)"
echo ""
echo "🖥  交互（需 DISPLAY / WSLg）："
echo "   ./output/jpov_sp_ui_demo/jpov_sp_ui_demo"
echo ""
echo "📸 headless 拍摄（每个时间点一张）："
echo "   ./output/jpov_sp_ui_demo/jpov_sp_ui_demo --capture output/shots/sp_ui --times 0,0.4,0.8,1.2"
echo ""
echo "🎛  板材参数对照："
echo "   ... --bump 0            # 平面边（无剖面明暗）"
echo "   ... --bump 1 --fade 0   # 圆管银边（默认）"
echo "   ... --bump 1 --fade 8   # 圆管 + 渐变消失带"
echo "   ... --no-texture        # 纯色板材（与旧 ui 逐像素对照）"
