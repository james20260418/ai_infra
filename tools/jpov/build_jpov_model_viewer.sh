#!/usr/bin/env bash
# =============================================================================
# build_jpov_model_viewer.sh — 编译 jpov_model_viewer（JPOV JPOV 模型查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_model_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_model_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_model_viewer/ 下
#   3. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_model_viewer/jpov_model_viewer <gltf 路径>
#   gltf 路径为相对/绝对路径，未指定时 fallback 到项目内 pliers.gltf 演示。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_model_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_model_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_model_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_model_viewer/"
cp -v "$BAZEL_BIN/jpov_model_viewer" "$OUTPUT_DIR/jpov_model_viewer"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（与 demo cfg.fonts 声明的相对路径一致，
# ResolveFontPath 按 exe 相对路径命中；PR #68 强调 exe 旁 fonts/ 优先）。
# UI 滑条用 CJK 显中文，DejaVu 做拉丁，两者都拷。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_model_viewer/fonts/"
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
echo "   $OUTPUT_DIR/jpov_model_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_model_viewer /absolute/path/to/model.gltf"
echo "       （也可传相对路径；未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "   2. AI 自查拍照（headless，不弹窗，输出 4 张图到 glTF 同级目录）："
echo "       $OUTPUT_DIR/jpov_model_viewer --four_views /absolute/path/to/model.gltf"
echo "       → <模型名>_front/up/left/perspective.png"
echo ""
echo "   验收：1280x720 不可 resize 窗口，显示 300×300 灰色地平面 + 被加载的"
echo "   glTF 模型，正午日照（太阳阴影 + 环境光）。右键 drag 转视角、滚轮 zoom。"
echo "   窗口底部居中 5 个半屏宽滑条（实时调节光照/场景）："
echo "     ① 太阳仰角 ° [0,90]（0=贴地日出日落 → 90=天顶正午）"
echo "     ② 浊度 turb [2,8]（默认 2=大晴 → 8=阴/重霾；衰减 sun/ambient 强度 + 天空霾化）"
echo "     ③ 季节 R 色温乘子 [0.5,2.0]（默认 1.0 中性；只偏色不改亮度，联动天空+sun/ambient）"
echo "     ④ 地面高度 y [-3,+3]（默认 -3）"
echo "     ⑤ 模型缩放 [0.1,20]（默认 1.0）"
echo "   光照 color+intensity 全部由 sky 自动推导：color 随仰角/季节变，intensity 随仰角"
echo "   + Turb*Loss(turb) 衰减（turb=2 时 Loss=1.0 不改变晴空基准），所见即所得地"
echo "   标定高浊度下 sun/ambient 该衰减成多少（锚点见 LIGHT_INTENSITY.md 第九节）。"
