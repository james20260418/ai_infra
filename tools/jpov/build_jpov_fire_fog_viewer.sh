#!/usr/bin/env bash
# =============================================================================
# build_jpov_fire_fog_viewer.sh — 编译 jpov_fire_fog_viewer（JPOV 雾火查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_fire_fog_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_fire_fog_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_fire_fog_viewer/ 下
#   3. 拷贝分发态字体到 exe 旁 fonts/、模型到 exe 旁 models/
#   4. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_fire_fog_viewer/jpov_fire_fog_viewer
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_fire_fog_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_fire_fog_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_fire_fog_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_fire_fog_viewer/"
cp -v "$BAZEL_BIN/jpov_fire_fog_viewer" "$OUTPUT_DIR/jpov_fire_fog_viewer"
ls -lh "$OUTPUT_DIR/"

echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_fire_fog_viewer/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo ""
echo "==> 4. 拷贝模型资源到 output/jpov_fire_fog_viewer/models/"
mkdir -p "$OUTPUT_DIR/models"
cp -v "$PROJECT_DIR/tools/jpov/test/object3d/scene_assets/table.glb"              "$OUTPUT_DIR/models/"
cp -v "$PROJECT_DIR/tools/jpov/test/object3d/oak_tripo_negative/tripo_oak_4k.glb" "$OUTPUT_DIR/models/"
cp -v "$PROJECT_DIR/tools/jpov/test/object3d/mixamo_male/mixamo_male.glb"         "$OUTPUT_DIR/models/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo "📂 产物位置： $OUTPUT_DIR/jpov_fire_fog_viewer"
echo "🧪 交互：$OUTPUT_DIR/jpov_fire_fog_viewer"
echo "🧪 headless：$OUTPUT_DIR/jpov_fire_fog_viewer --capture <out_dir>"
echo "   场景：中间一个方块 + 中间一个蓝人 + 灰色地面 + 桌子/橡树；"
echo "   屏幕中心一团 2 米点雾；勾「显示有元素的 tile」可看 L1 剪枝覆盖（绿色块）。"
