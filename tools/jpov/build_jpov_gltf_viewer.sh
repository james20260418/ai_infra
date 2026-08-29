#!/usr/bin/env bash
# =============================================================================
# build_jpov_gltf_viewer.sh — 编译 jpov_gltf_viewer（JPOV glTF 交互查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_gltf_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_gltf_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_gltf_viewer/ 下
#   3. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_gltf_viewer/jpov_gltf_viewer <gltf 路径>
#   gltf 路径为相对/绝对路径，未指定时 fallback 到项目内 pliers.gltf 演示。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_gltf_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_gltf_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_gltf_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_gltf_viewer/"
cp -v "$BAZEL_BIN/jpov_gltf_viewer" "$OUTPUT_DIR/jpov_gltf_viewer"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（与 demo cfg.fonts 声明的相对路径一致，
# ResolveFontPath 按 exe 相对路径命中；PR #68 强调 exe 旁 fonts/ 优先）。
# UI 滑条用 CJK 显中文，DejaVu 做拉丁，两者都拷。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_gltf_viewer/fonts/"
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
echo "   $OUTPUT_DIR/jpov_gltf_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   1. 交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_gltf_viewer /absolute/path/to/model.gltf"
echo "       （也可传相对路径；未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "   2. AI 自查拍照（headless，不弹窗，输出 4 张图到 glTF 同级目录）："
echo "       $OUTPUT_DIR/jpov_gltf_viewer --four_views /absolute/path/to/model.gltf"
echo "       → <模型名>_front/up/left/perspective.png"
echo ""
echo "   验收：1280x720 不可 resize 窗口，显示 300×300 灰色地平面 + 被加载的"
echo "   glTF 模型，正午日照（太阳阴影 + 环境光）。右键 drag 转视角、滚轮 zoom。"
echo "   窗口底部居中 3 个半屏宽滑条（实时调节光照）："
echo "     ① 太阳角度 θ [0,π]（太阳方向 (−sinθ,−cosθ,0)，θ=0 天顶直射）"
echo "     ② 平行光正午强度 [1,10]（默认 3.0）"
echo "     ③ 环境光正午强度 [0.1,1.0]（默认 0.3）"
echo "   光照颜色（ambient/directional）仍由天空自动推导，sky 其余参数不变。"
