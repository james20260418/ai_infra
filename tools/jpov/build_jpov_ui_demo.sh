#!/usr/bin/env bash
# =============================================================================
# build_jpov_ui_demo.sh — 编译 jpov_ui_demo（S7 集成 UI 调试台）的 Linux 和 Windows 产物
#
# 用法：
#   ./tools/jpov/build_jpov_ui_demo.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_ui_demo（Linux ELF）
#   2. bazel build //tools/jpov:jpov_ui_demo.exe --config=windows（Windows PE）
#   3. 两个产物拷贝到工程 output/jpov_ui_demo/ 下
#   4. 打印用法提示
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_ui_demo"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_ui_demo"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_ui_demo 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 编译 Windows 版 jpov_ui_demo.exe（MinGW 交叉编译）"
bazel build //tools/jpov:jpov_ui_demo.exe --config=windows 2>&1
echo ""
echo "OK: Windows 编译完成"

echo ""
echo "==> 3. 拷贝产物到 output/jpov_ui_demo/"
cp -v "$BAZEL_BIN/jpov_ui_demo"     "$OUTPUT_DIR/jpov_ui_demo"
cp -v "$BAZEL_BIN/jpov_ui_demo.exe" "$OUTPUT_DIR/jpov_ui_demo.exe"

# 拷贝分发态字体到 exe 旁边 fonts/（跨平台资源定位：ResolveFontPath 优先找 exe 旁 fonts/）
# UI demo 用 CJK 显中文，DejaVu 做拉丁，两者都拷。
echo "==> 4. 拷贝字体资源到 output/jpov_ui_demo/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"       "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"
ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/fonts/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_ui_demo       (Linux ELF)"
echo "   $OUTPUT_DIR/jpov_ui_demo.exe   (Windows PE)"
echo "   $OUTPUT_DIR/fonts/            (字体资源，需与 exe 同目录放置)"
echo ""
echo "🧪 用法："
echo "   1. Linux 版 → 在 WSL2 中运行（需要 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_ui_demo"
echo "   2. Windows 版 → 拷贝整个目录（exe + fonts/ 必须在一起）到 Windows 本地："
echo "       cp -r $OUTPUT_DIR /mnt/c/Users/<你的用户名>/Desktop/jpov_ui_demo_gold"
echo "       然后在 Windows 资源管理器中进入该目录，双击 jpov_ui_demo.exe 执行"
echo "       （exe 通过 GetModuleFileName 定位自身旁 fonts/ 目录，字体不能落单）"
echo ""
echo "   验收：1280x720 窗口内操作全部控件（滑条/勾选/输入/下拉/按钮），"
echo "   右侧实时日志框应逐条反映每次操作（带时间戳）。"
