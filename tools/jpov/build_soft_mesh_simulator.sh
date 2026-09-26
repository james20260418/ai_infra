#!/usr/bin/env bash
# =============================================================================
# build_soft_mesh_simulator.sh — 编译 jpov_soft_mesh_viewer（JPOV 软体仿真查看器）
#
# 用法（在仓库根或本目录下均可）：
#   ./tools/jpov/build_soft_mesh_simulator.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_soft_mesh_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_soft_mesh_viewer/ 下（含 exe 旁 fonts/）
#   3. 打印用法提示
#
# ⚠️ 当前阶段（M0）：仿真器的动力学尚未实现 —— Simulator::Step 是显式的
#    恒等桩（mesh 进、mesh 出，只推进时钟）。本脚本现在验证的是**静态展示模型**
#    的能力（加载 → 摆放 → 光照 → 相机 → UI 全链路）。
#    后续每加一条物理，都改 tools/jpov/soft_mesh_simulator/soft_mesh_simulator.cc，
#    本查看器一行不用改。
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_soft_mesh_viewer/jpov_soft_mesh_viewer <glb 路径>
#
# 仿真点可视化（M1）：
#   --bind_distance <m>  关联距离 d（默认 0.1）；决定长边加密密度，即仿真点数量。
#                        原始顶点画**红**点，加密插入的虚拟顶点画**蓝**点（2px 圆，
#                        不看遮挡）。地面叠 1m 栅格（±5m，XZ 平面）作尺度参照。
#   仿真点计数会打印在日志与面板上。
#
# headless UI 自检（不弹窗，出单张带面板的图）：
#   ... --ui_shot --output_dir /tmp/ui
# =============================================================================

set -euo pipefail

# 本脚本位于 <repo>/tools/jpov/，故工程根 = 上溯两级（与兄弟脚本
# build_jpov_model_viewer.sh 等同构）。
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
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
cp -v "$SCRIPT_DIR/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$SCRIPT_DIR/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"
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
echo "   3. 仿真点可视化（M1）：--bind_distance <m> 调关联距离（默认 0.1）"
echo "       红点 = 原始顶点，蓝点 = 长边加密插入的虚拟顶点（2px 圆，不看遮挡）"
echo "       地面叠 1m 栅格（±5m，XZ 平面）作尺度参照；仿真点计数打印在日志+面板"
echo ""
echo "   验收：1280x720 不可 resize 窗口，300×300 灰色地面 + 被加载的模型，"
echo "   固定正午晴天光照（太阳阴影 + 环境光）。右键 drag 转视角、滚轮 zoom。"
echo "   窗口底部居中一个面板（自上而下）："
echo "     · 地面高度 y [-3,+3]（默认 -3）"
echo "     · 关联距离 d (m) [0.005,1.0]（默认 0.1；拖它重建仿真点，红/蓝点实时变）"
echo "     · 勾选：推进仿真 / 仿真点 / 地面栅格"
echo "     · 只读行：仿真点计数（原始+虚拟=合计）、仿真 t/步数/顶点/三角形、操作提示"
echo "   （太阳仰角 / 浊度 / 季节 R / 模型缩放 已按需求删除 —— 光照固定正午）"
echo ""
echo "   架构："
echo "     · 物理：tools/jpov/soft_mesh_simulator/（独立包，纯 CPU、GL-free、可单测）"
echo "     · 显示：tools/jpov/demo/soft_mesh_viewer_app.h + jpov_soft_mesh_viewer.cc"
echo "     · 单测：bazel test //tools/jpov/soft_mesh_simulator:soft_mesh_simulator_test"
