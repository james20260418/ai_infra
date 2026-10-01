#!/usr/bin/env bash
# =============================================================================
# clothing_tool.sh — 编译 jpov_clothing_tool（JPOV 穿衣工具）
#
# 用法（在仓库根或本目录下均可）：
#   ./tools/jpov/clothing_tool.sh
#
# 效果：
#   1. bazel build //tools/jpov/clothing:jpov_clothing_tool（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_clothing_tool/ 下（含 exe 旁 fonts/）
#   3. 打印用法提示
#
# 本工具当前阶段：**加载 + 显示 + 变换 + 保存 + 软体仿真**。
#   ① 进入界面时后台线程为两份 glb 建「最近邻三角形」匹配器（期间主界面黑底白字显进度）；
#   ② 左上角半透明黑底面板（变换 / 保存 / 场景）：
#        - 平移 x/y/z：步长输入框（默认 0.1，|值|≤5.0）+ 步进按钮 "<" ">"；
#        - 旋转 RX/RY/RZ：步长输入框（默认 45°，|值|≤90°）+ 步进按钮 "<" ">"（绕该轴逆时针）；
#        - 整体缩放：系数输入框（默认 1.1，夹在 [1.0,2.0]）+ "-"（除系数）/ "+"（乘系数）
#          + 当前缩放只读；
#        - "保存衣服 glb" 按钮：把当前几何写到源 glb 同目录的 *_cloth_edit<时间戳>.glb。
#        ⭐ 所有调节都**就地直接改衣服 mesh 顶点**（不靠绘制参数，也不再有"绝对位置"输入框），
#          见 clothing_transform.h——状态即顶点坐标。
#   ③ 右上角面板（贴右边缘，随窗口 resize 自适应）：
#        - 动力学滑条：重力 g / 总质量 M / 力系数 F（指数坐标）/ 衰减 k；
#        - "继续/暂停仿真" 与 "重置衣服"（重置回**启动时**的原始几何）；
#        - 只读：实际 F(N) / 仿真 t·步 / 仿真点计数。
#   仍不做：人体排斥（Step 2）/ 对齐。
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_clothing_tool/jpov_clothing_tool \
#       --body_reference_path /path/to/body.glb \
#       --cloth_path /path/to/cloth.glb
#
#   --body_reference_path  人体 reference（.glb/.gltf）；缺省用项目自带 Mixamo 男性人体
#   --cloth_path           衣服模型（.glb/.gltf）；**必填**（没有通用默认值）
#   --phi_deg              初始俯视角（度，默认 20；>0 = 相机在上方俯视）
#
# headless UI 自检（不弹窗，出单张带面板的图）：
#   ... --ui_shot --output_dir /tmp/ui [--sim_steps N] [--window_width W --window_height H]
#   → /tmp/ui/clothing_tool_ui.png（--sim_steps 出图前先推进 N 步仿真验证下坠/落地）
# =============================================================================

set -euo pipefail

# 本脚本位于 <repo>/tools/jpov/，故工程根 = 上溯两级（与兄弟脚本
# build_soft_mesh_simulator.sh 等同构）。
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov/clothing"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_clothing_tool"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_clothing_tool"
cd "$PROJECT_DIR"
bazel build //tools/jpov/clothing:jpov_clothing_tool 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_clothing_tool/"
cp -v "$BAZEL_BIN/jpov_clothing_tool" "$OUTPUT_DIR/jpov_clothing_tool"

# 拷贝分发态字体到 exe 旁 fonts/（与 cfg.fonts 声明的相对路径一致，
# ResolveFontPath 优先命中 exe 旁 fonts/；PR #68）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_clothing_tool/fonts/"
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
echo "   $OUTPUT_DIR/jpov_clothing_tool       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_clothing_tool \\"
echo "         --body_reference_path /absolute/path/to/body.glb \\"
echo "         --cloth_path /absolute/path/to/cloth.glb"
echo "       （--body_reference_path 缺省 = 项目自带 Mixamo 男性人体；"
echo "        --cloth_path 必填）"
echo ""
echo "   2. UI 自检（headless 单帧 + 面板，不开窗口即可检查布局/字体）："
echo "       $OUTPUT_DIR/jpov_clothing_tool --ui_shot --output_dir /tmp/ui \\"
echo "         --cloth_path /path/to/cloth.glb"
echo "       → /tmp/ui/clothing_tool_ui.png"
echo ""
echo "   验收：初始 1280x720 窗口（**可 resize**；面板按每帧窗口尺寸自适应，右上角贴右边缘）。"
echo "   启动先黑底白字显示后台进度（建最近邻三角形），完成后显示场景：300×300 灰色地面 +"
echo "   人体 reference + 衣服，固定 45° 天光 + 三色环境光。右键 drag 转视角、滚轮 zoom。"
echo "   左上角半透明黑底面板（自上而下）："
echo "     · 平移表头（轴/步长(米)/步进） + X/Y/Z 行（步长框 + \"<\" \">\"）"
echo "     · 旋转表头（轴/步长(°)/步进） + RX/RY/RZ 行（步长框 + \"<\" \">\"）"
echo "     · 缩放系数框 + \"-\" \"+\" + 当前缩放只读"
echo "     · \"保存衣服 glb\" 按钮 + 保存状态"
echo "     · 地面高度 y [-3,+3]（默认 -3）"
echo "     · 勾选：显示人体 reference / 显示衣服"
echo "     · 只读行：人体 reference 与衣服的来源文件名 + primitive 数"
echo "   右上角半透明黑底面板（贴右边缘）："
echo "     · 仿真动力学滑条：重力 g / 总质量 M / 力系数 F（指数坐标）/ 衰减 k"
echo "     · 按钮：继续/暂停仿真、重置衣服（回到启动几何）"
echo "     · 只读：实际 F(N) / 仿真 t·步 / 仿真点计数"
echo ""
echo "   架构："
echo "     · 本工具：tools/jpov/clothing/（独立包，namespace jpov::clothing）"
echo "         - clothing_tool_app.h    渲染核心 App（场景 + 面板 + 仿真接线 + OneIteration）"
echo "         - clothing_init.{h,cc}    后台初始化（加载 + 建图 + 保留衣服 CPU 几何）"
echo "         - clothing_axis_input.h   数值填值解析（纯函数）"
echo "         - clothing_transform.h    衣物 mesh 就地几何操作（平移/旋转/缩放 + 法线重算）"
echo "         - clothing_save.{h,cc}    保存衣服 mesh 为 glb（后台线程）"
echo "         - jpov_clothing_tool.cc  主程序（CLI 装配 + 交互/headless 分发）"
echo "     · 软体仿真：复用 //tools/jpov/soft_mesh_simulator（独立包，纯 CPU）"
echo "     · 视角/光照/地面：复用 //tools/jpov:view_config / skylight_scene（zero 分叉）"
