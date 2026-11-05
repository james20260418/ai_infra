#!/usr/bin/env bash
# =============================================================================
# build_jpov_strip_viewer.sh — 编译 jpov_strip_viewer（JPOV 半透明条带查看器）
#
# 用法： ./tools/jpov/build_jpov_strip_viewer.sh [--windows]
# 产物： output/jpov_strip_viewer/jpov_strip_viewer
#      加 --windows 时额外产出同名 .exe（MinGW 交叉编译，落同一目录）
#
# 交互（需 DISPLAY/WSLg）： output/jpov_strip_viewer/jpov_strip_viewer
# headless 拍摄：           .../jpov_strip_viewer --capture <out_dir>
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
OUTPUT_DIR="$PROJECT_DIR/output/jpov_strip_viewer"

echo "==> 0. 输出目录"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "==> 1. 编译 jpov_strip_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_strip_viewer
if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "==> 1b. 编译 Windows 版 jpov_strip_viewer.exe（MinGW 交叉编译）"
    bazel build //tools/jpov:jpov_strip_viewer.exe --config=windows 2>&1
    cp -v "$BAZEL_BIN/jpov_strip_viewer.exe" "$OUTPUT_DIR/jpov_strip_viewer.exe"
    # 本地代码签名（可选）：消除 SmartScreen「未查到此应用的发布信息」。
    # 没有 osslsigncode 时脚本自己静默跳过，不阻塞构建。
    "$SCRIPT_DIR/sign_windows_exe.sh" "$OUTPUT_DIR/jpov_strip_viewer.exe" || true
    echo "OK: Windows 编译完成"
fi

cp -v "$BAZEL_BIN/jpov_strip_viewer" "$OUTPUT_DIR/jpov_strip_viewer"

echo "==> 2. 字体"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf" "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo "==> 3. 条带纹理"
mkdir -p "$OUTPUT_DIR/textures"
cp -v "$PROJECT_DIR/tools/jpov/assets/strip/ribbon_test.png" "$OUTPUT_DIR/textures/"

echo "==> 4. 橡树模型（玻璃缸天光场景用；与 fog viewer 同款）"
mkdir -p "$OUTPUT_DIR/models"
cp -v "$PROJECT_DIR/tools/jpov/assets/models/samples/oak_negative/tripo_oak_4k.glb" \
      "$OUTPUT_DIR/models/"

echo ""
echo "============================================"
echo "  完成！"
echo "  交互： $OUTPUT_DIR/jpov_strip_viewer"
echo "  拍摄： $OUTPUT_DIR/jpov_strip_viewer --capture <out_dir>"
echo "============================================"

if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "📂 Windows 产物："
    echo "   $OUTPUT_DIR/jpov_strip_viewer.exe   (Windows PE，静态链接，无 .dll 依赖)"
    echo "🪟 跑法：把整个 $OUTPUT_DIR 目录拷到 Windows，双击 jpov_strip_viewer.exe"
    echo "   （字体/贴图/模型都是相对 exe 的路径，必须连目录一起拷）"
else
    echo ""
    echo "🪟 需要 Windows 版 exe：$0 --windows（额外交叉编译，产物落在同一目录）"
fi
