#!/usr/bin/env bash
# =============================================================================
# build_jpov_gltf_viewer.sh — 编译 jpov_gltf_viewer（JPOV glTF 交互查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_gltf_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_gltf_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_gltf_viewer/ 下
#   3. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_gltf_viewer/jpov_gltf_viewer <gltf 路径>
#   gltf 路径为相对/绝对路径，未指定时 fallback 到项目内 pliers.gltf 演示。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_gltf_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_gltf_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_gltf_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_gltf_viewer/"
cp -v "$BAZEL_BIN/jpov_gltf_viewer" "$OUTPUT_DIR/jpov_gltf_viewer"
ls -lh "$OUTPUT_DIR/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_gltf_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_gltf_viewer /absolute/path/to/model.gltf"
echo "       （也可传相对路径；未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "   2. AI 自查拍照（headless，不弹窗，输出 4 张图到 glTF 同级目录）："
echo "       $OUTPUT_DIR/jpov_gltf_viewer --four_views /absolute/path/to/model.gltf"
echo "       → <模型名>_front/up/left/perspective.png"
echo ""
echo "   验收：1280x720 不可 resize 窗口，显示 300×300 灰色地平面 + 被加载的"
echo "   glTF 模型，正午日照（太阳/(0,-1,-1) 阴影 + 环境光）。右键 drag 转视角、"
echo "   滚轮 zoom。"
