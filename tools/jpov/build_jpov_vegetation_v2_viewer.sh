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
#   4b. 拷贝样例材质资产到 exe 旁 leaf/ 与 bark/（叶卡图 + 树皮 albedo/normal/roughness）
#   5. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_vegetation_v2_viewer/jpov_vegetation_v2_viewer                 # 只渲染 shape
#   output/jpov_vegetation_v2_viewer/jpov_vegetation_v2_viewer \
#       --leaf leaf/willow_leaf.png --bark bark/bark001                        # 完整组合
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
for f in willow.pbtxt willow_two_types.pbtxt; do
    if [ -f "$TOOL_DIR/samples/$f" ]; then
        cp -v "$TOOL_DIR/samples/$f" "$OUTPUT_DIR/shapes/$f"
    else
        echo "WARNING: 缺 $TOOL_DIR/samples/$f（先跑 export_vegetation_shape.py）"
    fi
done

# 入库的样例烘焙产物（samples/*.glb）：用来试「纯 glb 查看模式」，不用先有 shape/素材。
# 12 MB 左右，是 willow.pbtxt + bark001 + willow_leaf 的一份真实烘焙结果（带风链数据）。
echo ""
echo "==> 4c. 拷贝样例 glb 到 samples/"
mkdir -p "$OUTPUT_DIR/samples"
if ls "$TOOL_DIR"/samples/*.glb >/dev/null 2>&1; then
    cp -v "$TOOL_DIR"/samples/*.glb "$OUTPUT_DIR/samples/"
else
    echo "（没有 samples/*.glb，跳过：纯 glb 查看模式需要自己先 --bake 一份）"
fi

# 样例材质资产：入库的现成资产（叶卡图 + 转换好的树皮），clone 下来即可用。
# 树皮 png 入库的理由：转换要 ImageMagick，不该让使用者为「跑起来看看」装依赖。
echo ""
echo "==> 4b. 拷贝样例资产到 leaf/ 与 bark/"
if [ -d "$TOOL_DIR/assets" ]; then
    cp -rv "$TOOL_DIR/assets/." "$OUTPUT_DIR/"
else
    echo "WARNING: 缺 $TOOL_DIR/assets（样例叶卡/树皮没入库）"
fi

# 树皮材质：把 fetch_textures.sh 下下来的 JPG 转成 viewer 直接能吃的目录。
# 转换失败不阻断构建（没有 ImageMagick / 没下过贴图都能只跑形状）。
echo ""
echo "==> 5. 生成树皮材质 bark/bark001/"
BARK_SRC="$PROJECT_DIR/tools/jpov/tools/vegetation/textures/bark001"
if [ -d "$OUTPUT_DIR/bark/bark001" ]; then
    echo "SKIP: bark/bark001 来自入库资产"
elif [ -d "$BARK_SRC" ] && command -v convert >/dev/null 2>&1; then
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
echo "    ./jpov_vegetation_v2_viewer                          # 只渲染 shape（不贴树皮/叶图）"
echo "    ./jpov_vegetation_v2_viewer --leaf leaf/willow_leaf.png --leaf-black 30 \\"
echo "        --bark bark/bark001                              # 完整组合：shape + 树皮 + 叶卡"
echo "    （shape 省略时默认 shapes/willow.pbtxt；双叶型范本 --shape shapes/willow_two_types.pbtxt --leaf2 leaf/willow_leaf_flip.png）"
echo ""
echo "  烘焙一份 glb，再（纯查看模式）打开它："
echo "    ./jpov_vegetation_v2_viewer --bake output/veg.glb \\"
echo "        --shape shapes/willow.pbtxt --leaf leaf/willow_leaf.png --leaf-black 30 \\"
echo "        --bark bark/bark001"
echo "    ./jpov_vegetation_v2_viewer --glb output/veg.glb --glb-only   # 左下角是风调试面板"
echo "    （懒得到处找素材：入库的样例产物 ./jpov_vegetation_v2_viewer --glb samples/willow_baked.glb --glb-only）"
echo ""
echo "  出图（无窗口）："
echo "    xvfb-run -a ./jpov_vegetation_v2_viewer --capture shots/full \\"
echo "        --leaf leaf/willow_leaf.png --leaf-black 30 --bark bark/bark001"
echo "    （纯 glb 模式也能出图：--glb samples/willow_baked.glb --glb-only --capture shots/glb）"
echo ""
echo "  交互说明："
echo "    左键拖 = 转视角 / 滚轮 = 推拉 / 中键上下拖 = 平移注视点（半屏 = 1m）"
echo "    [ ] = 调最大显示层级    P = 打印相机    H = 打印叶卡帮助（对齐方式/推荐长宽比）"
echo "    面板：原材料态 = 场景(地面 y/最大层级/树皮/法线) + 叶卡 + 风调试 + 烘焙 glb；glb 态 = 风调试"
echo ""
echo "  样例资产（已入库，见 assets/README.md）："
echo "    leaf/willow_leaf.png       根部在左下角（anchor_mode bottom_left）"
echo "    leaf/willow_leaf_flip.png  左右翻转版，给镜像手性的 leaf type（bottom_right）"
echo "    bark/bark001/              albedo + normal(GL) + roughness"
echo "    法线强度默认 0.75，面板可调 / --normal-scale <0~3>"
