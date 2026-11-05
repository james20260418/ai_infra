#!/usr/bin/env bash
# =============================================================================
# build_jpov_pick_highlight_viewer.sh — 编译 jpov_pick_highlight_viewer
#                                        （JPOV 拾取/高亮查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_pick_highlight_viewer.sh [--windows]
#
# 效果：
#   1. bazel build //tools/jpov:jpov_pick_highlight_viewer（Linux ELF）
#   1b. 加 --windows 时额外：`bazel build //tools/jpov:jpov_pick_highlight_viewer.exe --config=windows`（Windows PE，同目录）
#   2. 产物拷贝到工程 output/jpov_pick_highlight_viewer/
#   3. 拷贝所需模型资产到 exe 旁 models/（保持仓库相对子路径，供 exe-旁路径解析）
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_pick_highlight_viewer/jpov_pick_highlight_viewer
#   —— 左键点选切换高亮；右键拖拽轨道旋转；滚轮缩放。
#   headless 出图：加 `--capture <out_dir>`。
# =============================================================================

set -euo pipefail

# ── 参数 ─────────────────────────────────────────────────────────────────────
# --windows：额外交叉编译 Windows 版 .exe（MinGW，静态链接，可直接拷到真卡上跑）。
# 默认只出 Linux ELF —— Windows 交叉编译要多花几十秒，不需要时不必等。
BUILD_WINDOWS=0
for arg in "$@"; do
    case "$arg" in
        --windows)
            BUILD_WINDOWS=1
            ;;
        -h|--help)
            echo "用法：$0 [--windows]"
            echo "  --windows  额外编译 Windows 版 .exe（MinGW 交叉编译，产物放同一目录）"
            exit 0
            ;;
        *)
            echo "未知参数：$arg（只支持 --windows / --help）" >&2
            exit 2
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_pick_highlight_viewer"
MODELS_SRC="$PROJECT_DIR/tools/jpov/assets/models"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_pick_highlight_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_pick_highlight_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "==> 1b. 编译 Windows 版 jpov_pick_highlight_viewer.exe（MinGW 交叉编译）"
    bazel build //tools/jpov:jpov_pick_highlight_viewer.exe --config=windows 2>&1
    cp -v "$BAZEL_BIN/jpov_pick_highlight_viewer.exe" "$OUTPUT_DIR/jpov_pick_highlight_viewer.exe"
    # 本地代码签名（可选）：消除 SmartScreen「未查到此应用的发布信息」。
    # 没有 osslsigncode 时脚本自己静默跳过，不阻塞构建。
    "$SCRIPT_DIR/sign_windows_exe.sh" "$OUTPUT_DIR/jpov_pick_highlight_viewer.exe" || true
    echo "OK: Windows 编译完成"
fi

echo ""
echo "==> 2. 拷贝产物到 output/jpov_pick_highlight_viewer/"
cp -v "$BAZEL_BIN/jpov_pick_highlight_viewer" "$OUTPUT_DIR/jpov_pick_highlight_viewer"

# 模型资产：保持仓库相对子路径（characters/ samples/lace_skirt/ scene/scene_assets/），
# 以匹配 demo 里的 exe-旁 "models/<rel>" 查找（见 AssetPath）。
echo ""
echo "==> 3. 拷贝模型资源到 output/jpov_pick_highlight_viewer/models/"
mkdir -p "$OUTPUT_DIR/models/characters"
mkdir -p "$OUTPUT_DIR/models/samples/lace_skirt"
mkdir -p "$OUTPUT_DIR/models/scene/scene_assets"
cp -v "$MODELS_SRC/characters/mixamo_male.glb"                    "$OUTPUT_DIR/models/characters/"
cp -v "$MODELS_SRC/samples/lace_skirt/lace_skirt_cutout.glb"      "$OUTPUT_DIR/models/samples/lace_skirt/"
cp -v "$MODELS_SRC/scene/scene_assets/stool.glb"                  "$OUTPUT_DIR/models/scene/scene_assets/"

ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/models/"*/* 2>/dev/null || true

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_pick_highlight_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_pick_highlight_viewer"
echo ""
echo "   验收：1280×720 不可 resize 窗口。场景含三大 renderer："
echo "     · 蒙皮蓝人 ×5（一次 instanced draw）"
echo "     · 连衣裙实例 ×3（lace_skirt_cutout，MASK cutout）"
echo "     · 凳子 object3d ×2"
echo "   交互：左键点选 → 高亮该物体（金黄描边）；再次点击同一物体 → 取消高亮；"
echo "         右键拖拽 → 轨道旋转；滚轮 → 缩放。固定光照，30 Hz。"
echo ""
echo "   headless 出图（确定性、自动化核对）："
echo "       $OUTPUT_DIR/jpov_pick_highlight_viewer --capture <out_dir>"
echo "   —— 输出 scene.png（无高亮）/ highlighted.png（三 renderer 各高亮一个）/"
echo "      probe.png（屏幕中心拾取自检）。"

if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "📂 Windows 产物："
    echo "   $OUTPUT_DIR/jpov_pick_highlight_viewer.exe   (Windows PE，静态链接，无 .dll 依赖)"
    echo "🪟 跑法：把整个 $OUTPUT_DIR 目录拷到 Windows，双击 jpov_pick_highlight_viewer.exe"
    echo "   （字体/贴图/模型都是相对 exe 的路径，必须连目录一起拷）"
else
    echo ""
    echo "🪟 需要 Windows 版 exe：$0 --windows（额外交叉编译，产物落在同一目录）"
fi
