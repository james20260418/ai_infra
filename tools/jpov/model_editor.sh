#!/usr/bin/env bash
# =============================================================================
# model_editor.sh — 编译 jpov_model_editor（JPOV 模型编辑器 · 裁剪）
#
# 用法（在仓库根或本目录下均可）：
#   ./tools/jpov/model_editor.sh
#
# 效果：
#   1. bazel build //tools/jpov/model_editor:jpov_model_editor（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_model_editor/ 下（含 exe 旁 fonts/）
#   3. 打印用法提示
#
# 本工具：加载 reference（可空）+ target；target 平移/旋转/缩放（左上角面板）+
#   沿水平面 y=y0 裁剪（右侧面板，删除 y<y0 或 y>y0）+ 保存 glb。
#   裁剪面用 primitives3d 半透明画出。裁剪会截断跨面三角形并按插值补边界顶点。
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_model_editor/jpov_model_editor \
#       --target_path /path/to/target.glb [--reference_path /path/to/reference.glb]
#
#   --reference_path  可选（缺省无 reference；只显示 target）
#   --target_path     必填（被编辑对象）
#   --phi_deg         初始俯视角（度，默认 20）
#
# headless UI 自检（不弹窗，出单张带面板的图）：
#   ... --ui_shot --output_dir /tmp/ui [--clip_y0 Y --clip_side above|below]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov/model_editor"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_model_editor"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_model_editor"
cd "$PROJECT_DIR"
bazel build //tools/jpov/model_editor:jpov_model_editor 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_model_editor/"
cp -v "$BAZEL_BIN/jpov_model_editor" "$OUTPUT_DIR/jpov_model_editor"

echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_model_editor/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$SCRIPT_DIR/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$SCRIPT_DIR/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"
ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/fonts/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_model_editor       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   $OUTPUT_DIR/jpov_model_editor \\"
echo "       --target_path /absolute/path/to/target.glb \\"
echo "       [--reference_path /absolute/path/to/reference.glb]"
echo ""
echo "   headless 自检："
echo "   $OUTPUT_DIR/jpov_model_editor --ui_shot --output_dir /tmp/ui \\"
echo "       --target_path /path/to/target.glb [--clip_y0 0.5 --clip_side above]"
echo "       → /tmp/ui/model_editor_ui.png"
echo ""
echo "🖐  交互：右键 drag = 相机环绕；滚轮 = 缩放。"
echo "    左上角面板：target 平移/旋转/缩放（步进式）+ 保存 + 地面 + 显示开关 + 重置。"
echo "    右侧面板：裁剪面 y0 滑条 + [删除 y<y0] + [删除 y>y0] + 显示裁剪面开关。"
echo ""
