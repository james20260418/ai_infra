#!/usr/bin/env bash
# =============================================================================
# build_jpov_demo.sh — 编译 jpov_demo 的 Linux 和 Windows 产物
#
# 用法：
#   ./tools/jpov/build_jpov_demo.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_demo（Linux ELF）
#   2. bazel build //tools/jpov:jpov_demo.exe --config=windows（Windows PE）
#   3. 两个产物拷贝到工程 output/jpov_demo/ 下
#   4. 打印用法提示
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_demo"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_demo"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_demo 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 编译 Windows 版 jpov_demo.exe（MinGW 交叉编译）"
bazel build //tools/jpov:jpov_demo.exe --config=windows 2>&1
echo ""
echo "OK: Windows 编译完成"

echo ""
echo "==> 3. 拷贝产物到 output/jpov_demo/"
cp -v "$BAZEL_BIN/jpov_demo"     "$OUTPUT_DIR/jpov_demo"
cp -v "$BAZEL_BIN/jpov_demo.exe" "$OUTPUT_DIR/jpov_demo.exe"
ls -lh "$OUTPUT_DIR/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_demo       (Linux ELF,  $(ls -lh "$OUTPUT_DIR/jpov_demo" | awk '{print $5}'))"
echo "   $OUTPUT_DIR/jpov_demo.exe   (Windows PE, $(ls -lh "$OUTPUT_DIR/jpov_demo.exe" | awk '{print $5}'))"
echo ""
echo "🧪 用法："
echo "   1. Linux 版 → 在 WSL2 中运行（需要 DISPLAY 环境变量指向 X server）："
echo "       $OUTPUT_DIR/jpov_demo"
echo "       # 或者从 WSL2 内直接运行，WSLg 会自动处理显示"
echo ""
echo "   2. Windows 版 → 把 jpov_demo.exe 拷贝到 Windows 本地后直接双击运行："
echo "       cp $OUTPUT_DIR/jpov_demo.exe /mnt/c/Users/<你的用户名>/Desktop/"
echo "       然后在 Windows 资源管理器中双击执行"
echo ""
echo "   提示：Windows 版 exe 为静态链接（无 .dll 依赖），可直接拷贝到任意 Windows 机器运行"
