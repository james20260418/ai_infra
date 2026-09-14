#!/usr/bin/env bash
# =============================================================================
# build_jpov_fbx_viewer.sh — 编译 jpov_fbx_viewer（JPOV FBX 观察器）
#
# 用法：
#   ./tools/jpov/build_jpov_fbx_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov/demo/fbx_viewer:jpov_fbx_viewer（Linux ELF）
#   2. 产物 + 字体拷贝到工程 output/jpov_fbx_viewer/ 下
#   3. 打印交互操作说明
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_fbx_viewer/jpov_fbx_viewer <fbx 路径>
# 该工具需要**带动画**的 FBX（项目里可用的样例：
#   tools/jpov/test/animations/hip_hop_dance.fbx，Mixamo "Hip Hop Dancing"）。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# ⚠️ 二进制路径必须与上面的 bazel target 一致（曾经因为写错路径而把旧产物拷进 output/，
#    导致"编译了但跑的是老程序"）。
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov/demo/fbx_viewer"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_fbx_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_fbx_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov/demo/fbx_viewer:jpov_fbx_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_fbx_viewer/"
cp -v "$BAZEL_BIN/jpov_fbx_viewer" "$OUTPUT_DIR/jpov_fbx_viewer"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（cfg.fonts 声明的是相对 exe 路径，
# ResolveFontPath 按 exe 相对路径命中；CJK 显中文，DejaVu 做拉丁回退）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_fbx_viewer/fonts/"
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
echo "   $OUTPUT_DIR/jpov_fbx_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   $OUTPUT_DIR/jpov_fbx_viewer /absolute/path/to/anim.fbx [glb 路径]"
echo "   （样例：$PROJECT_DIR/tools/jpov/test/animations/hip_hop_dance.fbx"
echo "            可选目标骨架：$PROJECT_DIR/tools/jpov/test/object3d/mixamo_male/mixamo_male.glb）"
echo ""
echo "🖐  交互："
echo "   右键拖动  = 相机环绕（与模型查看器同款）"
echo "   滚轮      = 缩放"
echo ""
echo "🎚  底部面板（两行）："
echo "   ① [mesh 来源] 下拉（仅传了 glb 时出现）—— 看哪个骨架的骨人被这段 pose 驱动："
echo "        · fbx rest（红，源）        = 源骨架 + fbx 自己的动画（正确参照）"
echo "        · glb rest（蓝，无重定向）  = glb rest 骨架 + 同一份 pose **数值直搬**（对照组）"
echo "        · 两者并列（红左/蓝右）      = 同屏对比（传了 glb 的默认项）"
echo "   ② [暂停 / 继续播放] 按钮 —— 按下后**主频的时间停止更新**，画面定格在当前帧。"
echo "   ③ [固定 rest 位姿(identity)] 复选框 —— 勾上切到 rest 模式看骨架 T-pose；"
echo "      时间同样不推进，取消勾选后从停住的时刻继续。"
echo "   ④ 状态行：帧号 / 总帧数 / 源帧频 / 时刻 / 播放态（+ 蓝骨对照信息与骨名命中数）。"
echo ""
echo "   ⚠️ 蓝骨是**对照组（消融实验）**：刻不做重定向，把 fbx 每帧的 lcl rotation 数值原样"
echo "   搬到 glb 骨架上（两骨架局部帧差最大 ~180°）→ 四肢会绕错轴，动作明显不对。"
echo "   正式的 retarget（按世界朝向共轭）是后续 M4，本 viewer 不包含。"
echo ""
echo "   播放速度 = FBX 自身的 fps（60fps 渲染下每帧推进 1/60 秒，帧间逐骨 Slerp 插值），"
echo "   循环播放（末帧回绕到首帧，接缝处同样插值）。"
echo ""
echo "📷  headless 出图（不弹窗，AI 自查/留证用）："
echo "   $OUTPUT_DIR/jpov_fbx_viewer <fbx> [glb] --shot /tmp/a.png"
echo "        [--time 秒 | --frame 帧号] [--mesh 0|1|2] [--rest]"
echo "   → 指定时刻（或帧号）/指定 mesh 来源的纯 3D 截图；--rest 出 identity 位姿图。"
