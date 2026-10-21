#!/usr/bin/env bash
# =============================================================================
# build_jpov_vegetation_v2_viewer.sh — 编译 vegetation v2 形状查看器
#
# 用法：
#   ./tools/jpov/build_jpov_vegetation_v2_viewer.sh
#
# 效果：
#   1. bazel build //tools/jpov/tools/vegetation_tool_v2:jpov_vegetation_v2_viewer
#   2. 产物拷贝到 output/jpov_vegetation_v2_viewer/
#   3. 拷贝分发态字体到 exe 旁 fonts/（UI 面板要 CJK）
#   4. 拷贝范本拓扑到 exe 旁 shapes/（默认装载的树）
#   5. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_vegetation_v2_viewer/jpov_vegetation_v2_viewer \
#       --shape shapes/willow.pbtxt
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL_DIR="$PROJECT_DIR/tools/jpov/tools/vegetation_tool_v2"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_vegetation_v2_viewer"

echo "==> 0. 清空输出目录"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 jpov_vegetation_v2_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov/tools/vegetation_tool_v2:jpov_vegetation_v2_viewer 2>&1
echo "OK: 编译完成"

echo ""
echo "==> 2. 拷贝产物"
cp -v "$PROJECT_DIR/bazel-bin/tools/jpov/tools/vegetation_tool_v2/jpov_vegetation_v2_viewer" \
      "$OUTPUT_DIR/jpov_vegetation_v2_viewer"

# 字体：cfg.fonts 声明的是相对路径 "fonts/xxx"，ResolveFontPath 按 exe 相对路径命中。
# UI 面板要显中文，所以 CJK 必拷；DejaVu 做拉丁兜底。
echo ""
echo "==> 3. 拷贝字体"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

# 范本拓扑：由 export_vegetation_shape.py 从 Blender/Sapling 导出。
# 入库是为了让人 clone 下来就能跑（同时充当扫管的回归输入）。
echo ""
echo "==> 4. 拷贝范本拓扑到 shapes/"
mkdir -p "$OUTPUT_DIR/shapes"
if [ -f "$TOOL_DIR/samples/willow.pbtxt" ]; then
    cp -v "$TOOL_DIR/samples/willow.pbtxt" "$OUTPUT_DIR/shapes/willow.pbtxt"
else
    echo "WARNING: 缺 $TOOL_DIR/samples/willow.pbtxt（先跑 export_vegetation_shape.py）"
fi

echo ""
ls -lh "$OUTPUT_DIR/"
echo ""
echo "==> 用法"
echo "  交互（需 DISPLAY/WSLg）："
echo "    cd $OUTPUT_DIR"
echo "    ./jpov_vegetation_v2_viewer --shape shapes/willow.pbtxt"
echo ""
echo "  出图（无窗口）："
echo "    xvfb-run -a ./jpov_vegetation_v2_viewer --shape shapes/willow.pbtxt --capture /tmp/veg"
echo ""
echo "  交互说明："
echo "    左键拖 = 转视角 / 滚轮 = 推拉 / 中键上下拖 = 平移注视点（半屏 = 1m）"
echo "    [ ] = 调最大显示层级    P = 打印相机"
echo "    面板：地面 y / 最大层级 / 树皮纹理 / 显示全部叶子 / 叶 type 下标"
echo ""
echo "  贴树皮（材质目录由 bark_gen.py 产出：albedo/normal/roughness.png）："
echo "    ./jpov_vegetation_v2_viewer --shape shapes/willow.pbtxt --bark <材质目录>"
