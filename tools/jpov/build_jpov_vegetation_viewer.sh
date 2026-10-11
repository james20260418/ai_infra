#!/usr/bin/env bash
# =============================================================================
# build_jpov_vegetation_viewer.sh — 编译 jpov_vegetation_viewer（JPOV 植被查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_vegetation_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_vegetation_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_vegetation_viewer/ 下
#   3. 拷贝分发态字体到 exe 旁 fonts/（UI 面板显中文）
#   4. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_vegetation_viewer/jpov_vegetation_viewer
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_vegetation_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_vegetation_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_vegetation_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_vegetation_viewer/"
cp -v "$BAZEL_BIN/jpov_vegetation_viewer" "$OUTPUT_DIR/jpov_vegetation_viewer"

# 拷贝分发态字体到 exe 旁 fonts/（与 demo cfg.fonts 声明的相对路径一致）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_vegetation_viewer/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

# 拷贝植被纹理（树皮 + 叶）到 exe 旁 textures/（与 demo 里相对路径一致）。
echo ""
echo "==> 4. 拷贝植被纹理到 output/jpov_vegetation_viewer/textures/"
mkdir -p "$OUTPUT_DIR/textures"
cp -v "$PROJECT_DIR/tools/jpov/assets/vegetation/bark_dragonclaw.png"     "$OUTPUT_DIR/textures/"
cp -v "$PROJECT_DIR/tools/jpov/assets/vegetation/leaf_dragonclaw_1.png"   "$OUTPUT_DIR/textures/"

ls -lh "$OUTPUT_DIR/"
echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_vegetation_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_vegetation_viewer"
echo "     面板 knob：展开代数（生长）/ 长度缩放 / 基本粗细 / 随机种子 / 枝干：管状 / 管截面边数 /"
echo "              树皮瓦长 / 显示叶片 / 叶长 / 叶片生长 / 每叶丛叶片数。"
echo "     右键 drag 转视角、滚轮 zoom。"
echo ""
echo "   headless 拍摄（生长扫谱 + 成品多视角 + 叶片生长扫谱）："
echo "       $OUTPUT_DIR/jpov_vegetation_viewer --capture <out_dir>"
