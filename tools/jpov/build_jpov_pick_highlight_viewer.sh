#!/usr/bin/env bash
# =============================================================================
# build_jpov_pick_highlight_viewer.sh — 编译 jpov_pick_highlight_viewer
#                                        （JPOV 拾取/高亮查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_pick_highlight_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_pick_highlight_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_pick_highlight_viewer/
#   3. 拷贝所需模型资产到 exe 旁 models/（保持仓库相对子路径，供 exe-旁路径解析）
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_pick_highlight_viewer/jpov_pick_highlight_viewer
#   —— 左键点选切换高亮；右键拖拽轨道旋转；滚轮缩放。
#   headless 出图：加 `--capture <out_dir>`。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_pick_highlight_viewer"
MODELS_SRC="$PROJECT_DIR/tools/jpov/assets/models"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_pick_highlight_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_pick_highlight_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_pick_highlight_viewer/"
cp -v "$BAZEL_BIN/jpov_pick_highlight_viewer" "$OUTPUT_DIR/jpov_pick_highlight_viewer"

# 模型资产：保持仓库相对子路径（characters/ samples/lace_skirt/ scene/scene_assets/），
# 以匹配 demo 里的 exe-旁 "models/<rel>" 查找（见 AssetPath）。
echo ""
echo "==> 3. 拷贝模型资源到 output/jpov_pick_highlight_viewer/models/"
mkdir -p "$OUTPUT_DIR/models/characters"
mkdir -p "$OUTPUT_DIR/models/samples/lace_skirt"
mkdir -p "$OUTPUT_DIR/models/scene/scene_assets"
cp -v "$MODELS_SRC/characters/mixamo_male.glb"                    "$OUTPUT_DIR/models/characters/"
cp -v "$MODELS_SRC/samples/lace_skirt/lace_skirt_cutout.glb"      "$OUTPUT_DIR/models/samples/lace_skirt/"
cp -v "$MODELS_SRC/scene/scene_assets/stool.glb"                  "$OUTPUT_DIR/models/scene/scene_assets/"

ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/models/"*/* 2>/dev/null || true

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_pick_highlight_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_pick_highlight_viewer"
echo ""
echo "   验收：1280×720 不可 resize 窗口。场景含三大 renderer："
echo "     · 蒙皮蓝人 ×5（一次 instanced draw）"
echo "     · 连衣裙实例 ×3（lace_skirt_cutout，MASK cutout）"
echo "     · 凳子 object3d ×2"
echo "   交互：左键点选 → 高亮该物体（金黄描边）；再次点击同一物体 → 取消高亮；"
echo "         右键拖拽 → 轨道旋转；滚轮 → 缩放。固定光照，30 Hz。"
echo ""
echo "   headless 出图（确定性、自动化核对）："
echo "       $OUTPUT_DIR/jpov_pick_highlight_viewer --capture <out_dir>"
echo "   —— 输出 scene.png（无高亮）/ highlighted.png（三 renderer 各高亮一个）/"
echo "      probe.png（屏幕中心拾取自检）。"
