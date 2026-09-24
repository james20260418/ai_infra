#!/usr/bin/env bash
# =============================================================================
# soft_mesh_simulator.sh — 编译 jpov_soft_mesh_viewer（JPOV 软体仿真查看器）
#
# 用法：
#   ./soft_mesh_simulator.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_soft_mesh_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_soft_mesh_viewer/ 下（含 exe 旁 fonts/）
#   3. 打印用法提示
#
# ⚠️ 当前阶段（M0）：仿真器的动力学尚未实现 —— SoftMeshSimulator::Step 是显式的
#    恒等桩（mesh 进、mesh 出，只推进时钟）。本脚本现在验证的是**静态展示模型**
#    的能力（加载 → 摆放 → 光照 → 相机 → UI 全链路）。
#    后续每加一条物理，都改 tools/jpov/demo/soft_mesh_sim.cc，本查看器一行不用改。
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_soft_mesh_viewer/jpov_soft_mesh_viewer <glb 路径>
#
# headless UI 自检（不弹窗，出单张带面板的图）：
#   ... --ui_shot --output_dir /tmp/ui
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_soft_mesh_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_soft_mesh_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_soft_mesh_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_soft_mesh_viewer/"
cp -v "$BAZEL_BIN/jpov_soft_mesh_viewer" "$OUTPUT_DIR/jpov_soft_mesh_viewer"

# 拷贝分发态字体到 exe 旁 fonts/（与 cfg.fonts 声明的相对路径一致，
# ResolveFontPath 优先命中 exe 旁 fonts/；PR #68）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_soft_mesh_viewer/fonts/"
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
echo "   $OUTPUT_DIR/jpov_soft_mesh_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_soft_mesh_viewer /absolute/path/to/model.glb"
echo "       （未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "   2. UI 自检（headless 单帧 + 面板，不开窗口即可检查布局/字体）："
echo "       $OUTPUT_DIR/jpov_soft_mesh_viewer --ui_shot --output_dir /tmp/ui /path/to.glb"
echo "       → /tmp/ui/soft_mesh_viewer_ui.png"
echo ""
echo "   验收：1280x720 不可 resize 窗口，300×300 灰色地面 + 被加载的模型，"
echo "   固定正午晴天光照（太阳阴影 + 环境光）。右键 drag 转视角、滚轮 zoom。"
echo "   窗口底部居中 1 个半屏宽滑条（需求：只保留地面高度与视角）："
echo "     · 地面高度 y [-3,+3]（默认 -3）"
echo "   （太阳仰角 / 浊度 / 季节 R / 模型缩放 已按需求删除 —— 光照固定正午）"
echo "   面板上方另有只读状态行：仿真 t / 步数 / 顶点数 / 三角形数。"
echo ""
echo "   架构："
echo "     · 物理：tools/jpov/demo/soft_mesh_sim.{h,cc}（纯 CPU、GL-free、可单测）"
echo "     · 显示：tools/jpov/demo/soft_mesh_viewer_app.h + jpov_soft_mesh_viewer.cc"
echo "     · 单测：bazel test //tools/jpov:soft_mesh_sim_test"
