#!/usr/bin/env bash
# =============================================================================
# build_jpov_vegetation_viewer.sh — 编译 jpov_vegetation_viewer（JPOV 植被查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_vegetation_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov:jpov_vegetation_viewer（Linux ELF）
#   2. 产物拷贝到工程 output/jpov_vegetation_viewer/ 下
#   3. 拷贝分发态字体到 exe 旁 fonts/
#   4. 拷贝柳树范本 GLB 到 exe 旁 plants/（默认装载的植物）
#   5. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_vegetation_viewer/jpov_vegetation_viewer [--glb <植物.glb>]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_vegetation_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_vegetation_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_vegetation_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_vegetation_viewer/"
cp -v "$BAZEL_BIN/jpov_vegetation_viewer" "$OUTPUT_DIR/jpov_vegetation_viewer"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（与 demo cfg.fonts 声明的相对路径一致，
# ResolveFontPath 按 exe 相对路径命中；PR #68 强调 exe 旁 fonts/ 优先）。
# UI 滑条用 CJK 显中文，DejaVu 做拉丁，两者都拷。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_vegetation_viewer/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

# 拷贝柳树范本 GLB 到 exe 旁 plants/（demo 默认植物路径 "plants/willow_small_tex.glb"）。
# 范本由植被工具（tools/jpov/tools/vegetation/，PR #175）Blender/Sapling 产出，
# 已入库；这里只是搬到分发态 exe 旁。
echo ""
echo "==> 4. 拷贝柳树范本 GLB 到 output/jpov_vegetation_viewer/plants/"
mkdir -p "$OUTPUT_DIR/plants"
cp -v "$PROJECT_DIR/tools/jpov/tools/vegetation/samples/willow_small_tex.glb" \
      "$OUTPUT_DIR/plants/willow_small_tex.glb"
# 带风数据的柳树范本（关节表 + loc6），供 --capture-wind / 风动观察。
cp -v "$PROJECT_DIR/tools/jpov/tools/vegetation/samples/willow_wind_tex.glb" \
      "$OUTPUT_DIR/plants/willow_wind_tex.glb"
ls -lh "$OUTPUT_DIR/plants/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_vegetation_viewer   (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_vegetation_viewer [--glb <植物.glb>]"
echo "       --glb 缺省时装载 exe 旁 plants/willow_small_tex.glb。"
echo ""
echo "   观察：右键 drag 转视角、滚轮 zoom；面板可调天光自由度（浊度 / 季节色温 /"
echo "   天体仰角+方位 / 月色 / 夜蓝 / 三色 ambient）—— 仰角正=日光、负=月光（同一条"
echo "   轴覆盖昼夜）；SMAA 抗锯齿默认开。"
echo ""
echo "   headless 拍摄："
echo "       $OUTPUT_DIR/jpov_vegetation_viewer --capture <out_dir>          # 昼夜扫谱 + 正午/夜间机位"
echo "       $OUTPUT_DIR/jpov_vegetation_viewer --capture-smaa <out_dir>     # SMAA A/B（off / on）"
