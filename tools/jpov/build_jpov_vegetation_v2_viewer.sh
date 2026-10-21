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

# 树皮材质：把 fetch_textures.sh 下下来的 JPG 转成 viewer 直接能吃的目录。
# 转换失败不阻断构建（没有 ImageMagick / 没下过贴图都能只跑形状）。
echo ""
echo "==> 5. 生成树皮材质 bark/bark001/"
BARK_SRC="$PROJECT_DIR/tools/jpov/tools/vegetation/textures/bark001"
if [ -d "$BARK_SRC" ] && command -v convert >/dev/null 2>&1; then
    if "$TOOL_DIR/make_bark_material.sh" "$BARK_SRC" "$OUTPUT_DIR/bark/bark001" >/dev/null; then
        echo "OK: $OUTPUT_DIR/bark/bark001"
    else
        echo "WARNING: 树皮材质生成失败（见上）"
    fi
else
    echo "SKIP: 缺 $BARK_SRC 或 ImageMagick（fetch_textures.sh 后重跑即可）"
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
echo "    [ ] = 调最大显示层级    P = 打印相机    H = 打印叶卡帮助（对齐方式/推荐长宽比）"
echo "    面板：地面 y / 最大层级 / 树皮纹理 / 显示全部叶子 / 叶 type 下标"
echo ""
echo "  贴树皮（bark/bark001 已自动生成；也可自己造 albedo/normal/roughness.png）："
echo "    ./jpov_vegetation_v2_viewer --shape shapes/willow.pbtxt --bark bark/bark001"
echo "    法线强度默认 1.5，面板可调 / --normal-scale <0~3>"
