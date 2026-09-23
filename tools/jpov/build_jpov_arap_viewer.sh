#!/usr/bin/env bash
# =============================================================================
# build_jpov_arap_viewer.sh — 编译 jpov_arap_viewer（JPOV ARAP 软体查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_arap_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_arap_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_arap_viewer/ 下（含 exe 旁 fonts/）
#   3. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_arap_viewer/jpov_arap_viewer <glb 路径>
#
# headless 摔落序列出图（不弹窗）：
#   ... --headless_drop [--frames N] [--every K] [--output_dir DIR]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_arap_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_arap_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_arap_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_arap_viewer/"
cp -v "$BAZEL_BIN/jpov_arap_viewer" "$OUTPUT_DIR/jpov_arap_viewer"

echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_arap_viewer/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf" "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"
ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/fonts/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_arap_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_arap_viewer /absolute/path/to/model.glb"
echo "       （未指定时用项目内 catapult.glb 演示）"
echo ""
echo "   2. 内置方块（不传模型路径时的默认；也可显式 --box）："
echo "       $OUTPUT_DIR/jpov_arap_viewer              # 交互：内置方块"
echo "       $OUTPUT_DIR/jpov_arap_viewer --box        # 同上（显式）"
echo ""
echo "   3. headless 摔落序列（不弹窗，逐帧推进动力学）："
echo "       $OUTPUT_DIR/jpov_arap_viewer --headless_drop --frames 90 --every 10 \\"
echo "           /absolute/path/to/model.glb"
echo "       → <模型名>_drop_000/010/.../089.png"
echo "       （物理固定步长 0.05 s、渲染帧 1/60 s ⇒ 每 3 帧推进一步）"
echo ""
echo "   4. 四参数单变量对照（headless 批量跑用，缺省不改）："
echo "       --hooke H   --gravity G   --arap B   --damping U   --substeps N"
echo "       例：--gravity 0（关重力，验证形变是否只由重力驱动）"
echo ""
echo "   5. UI 自检（headless 单帧 + 面板，不开窗口即可检查布局/字体）："
echo "       $OUTPUT_DIR/jpov_arap_viewer --box --ui_shot --output_dir /tmp/ui"
echo "       → /tmp/ui/arap_viewer_ui.png"
echo ""
echo "   验收：1280x720 不可 resize 窗口，加载的模型悬在 300×300 灰色地面之上。"
echo "   右键 drag 转视角、滚轮 zoom。面板："
echo "     顶部按钮「动力学：已暂停 ▶」→ 点击开始自由落体（四参数受力 + 地面投影）"
echo "     顶部按钮「重置 mesh」     → 网格恢复 bind pose 并暂停"
echo "     底部左列：太阳仰角 / 浊度 / 季节 R / 地面高度 y"
echo "     底部右列：胡克 h / 重力 g / ARAP b / 阻尼 u（可实时调，做单变量对照）"
echo "   物理模型与实测结论见 tools/jpov/docs/jpov_arap_viewer_design.md"
