#!/usr/bin/env bash
# =============================================================================
# build_jpov_skylight_viewer.sh — 编译 jpov_skylight_viewer（JPOV 天光查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_skylight_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_skylight_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_skylight_viewer/ 下
#   3. 拷贝分发态字体到 exe 旁 fonts/
#   4. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_skylight_viewer/jpov_skylight_viewer
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_skylight_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_skylight_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_skylight_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_skylight_viewer/"
cp -v "$BAZEL_BIN/jpov_skylight_viewer" "$OUTPUT_DIR/jpov_skylight_viewer"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（与 demo cfg.fonts 声明的相对路径一致，
# ResolveFontPath 按 exe 相对路径命中；PR #68 强调 exe 旁 fonts/ 优先）。
# UI 滑条用 CJK 显中文，DejaVu 做拉丁，两者都拷。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_skylight_viewer/fonts/"
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
echo "   $OUTPUT_DIR/jpov_skylight_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_skylight_viewer"
echo ""
echo "   验收：1280×720 不可 resize 窗口，场景为三个不同材质的方块（低反 / 高光 /"
echo "   金属）+ 灰色地面，视角变换与 model viewer 相同（右键 drag 转视角、滚轮 zoom）。"
echo "   窗口底部居中 6 个半屏宽滑条（表达**四个自由度**，其余走 SkyCommand 默认）："
echo "     ① 浊度 turb [0,8]（默认 2）"
echo "     ② 季节色温 [−1,+1]（左=蓝偏 / 右=红偏 / 中=中性）"
echo "     ③ 太阳方向：仰角 [0,90] + 方位角 [0,360)"
echo "     ④ 月亮方向：仰角 [0,90] + 方位角 [0,360)"
echo ""
echo "   天光由 CreateDefaultSkyCommand(四自由度) 构造，**光照（日光 + ambient）由它推导**；"
echo "   月光平行光不做（月盘只作为天体画出）。"
echo "   夜色两色见 demo/skylight_scene.h；锚点推导见 interface/LIGHT_INTENSITY.md 第十节。"
echo ""
echo "   headless 拍摄（出图验收/自动化）："
echo "       $OUTPUT_DIR/jpov_skylight_viewer --capture <out_dir>"
