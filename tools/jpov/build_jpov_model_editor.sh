#!/usr/bin/env bash
# =============================================================================
# build_jpov_model_editor.sh — 编译 jpov_model_editor（JPOV 模型编辑器）
#
# 用法：
#   ./tools/jpov/build_jpov_model_editor.sh
#
# 效果：
#   1. bazel build //tools/jpov/demo/editor:jpov_model_editor（Linux ELF）
#   2. 产物 + 字体拷贝到工程 output/jpov_model_editor/ 下
#   3. 打印交互操作说明
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_model_editor/jpov_model_editor <gltf/glb 路径>
#   未指定路径时 fallback 到项目内 pliers.gltf 演示。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_model_editor"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_model_editor"
cd "$PROJECT_DIR"
bazel build //tools/jpov/demo/editor:jpov_model_editor 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_model_editor/"
cp -v "$BAZEL_BIN/jpov_model_editor" "$OUTPUT_DIR/jpov_model_editor"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（cfg.fonts 声明的是相对 exe 路径，
# ResolveFontPath 按 exe 相对路径命中；CJK 显中文，DejaVu 做拉丁回退）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_model_editor/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"
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
echo "   $OUTPUT_DIR/jpov_model_editor /absolute/path/to/model.glb"
echo "   （也可传相对路径；未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "🖐  交互："
echo "   左键横向拖动          = 模型绕世界 X 轴旋转（右滑逆时针，纵向分量忽略）"
echo "   Ctrl + 左键横向拖动   = 模型绕世界 Y 轴旋转（右滑逆时针）"
echo "   右键拖动 / 滚轮       = 相机环绕 / 缩放"
echo ""
echo "🎚  底部滑条（缩放 0.1~10 / 平移 XYZ ±3 / 旋转 RX·RY ±180 / 地面高度 -3~0）："
echo "   变换作用顺序 = 先缩放 → 再旋转 → 最后平移（对齐 DrawObject3D 语义）。"
echo "   本 PR 不做资产保存：摆放数学产出的 (center,up,front,scale) 即后续 PR"
echo "   反推顶点调整量的输入。"
