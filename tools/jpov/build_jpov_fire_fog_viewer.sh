#!/usr/bin/env bash
# =============================================================================
# build_jpov_fire_fog_viewer.sh — 编译 jpov_fire_fog_viewer（JPOV 雾火查看器）
#
# 用法：
#   ./tools/jpov/build_jpov_fire_fog_viewer.sh [--windows]
#
# 效果：
#   1. bazel build //tools/jpov:jpov_fire_fog_viewer（Linux ELF）
#   1b. 加 --windows 时额外：`bazel build //tools/jpov:jpov_fire_fog_viewer.exe --config=windows`（Windows PE，同目录）
#   2. 产物拷贝到工程 output/jpov_fire_fog_viewer/ 下
#   3. 拷贝分发态字体到 exe 旁 fonts/
#   4. 拷贝橡树模型到 exe 旁 models/
#   5. 打印用法提示
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_fire_fog_viewer/jpov_fire_fog_viewer
#
# headless 拍摄（出图验收/自动化，需 Xvfb 或 WSLg）：
#   output/jpov_fire_fog_viewer/jpov_fire_fog_viewer --capture <out_dir>
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
OUTPUT_DIR="$PROJECT_DIR/output/jpov_fire_fog_viewer"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_fire_fog_viewer"
cd "$PROJECT_DIR"
bazel build //tools/jpov:jpov_fire_fog_viewer 2>&1
echo ""
echo "OK: Linux 编译完成"

if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "==> 1b. 编译 Windows 版 jpov_fire_fog_viewer.exe（MinGW 交叉编译）"
    bazel build //tools/jpov:jpov_fire_fog_viewer.exe --config=windows 2>&1
    cp -v "$BAZEL_BIN/jpov_fire_fog_viewer.exe" "$OUTPUT_DIR/jpov_fire_fog_viewer.exe"
    # 本地代码签名（可选）：消除 SmartScreen「未查到此应用的发布信息」。
    # 没有 osslsigncode 时脚本自己静默跳过，不阻塞构建。
    "$SCRIPT_DIR/sign_windows_exe.sh" "$OUTPUT_DIR/jpov_fire_fog_viewer.exe" || true
    echo "OK: Windows 编译完成"
fi

echo ""
echo "==> 2. 拷贝产物到 output/jpov_fire_fog_viewer/"
cp -v "$BAZEL_BIN/jpov_fire_fog_viewer" "$OUTPUT_DIR/jpov_fire_fog_viewer"

echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_fire_fog_viewer/fonts/"
mkdir -p "$OUTPUT_DIR/fonts"
cp -v "$PROJECT_DIR/tools/jpov/fonts/DejaVuSans.ttf"          "$OUTPUT_DIR/fonts/"
cp -v "$PROJECT_DIR/tools/jpov/fonts/NotoSansCJK-Regular.ttc" "$OUTPUT_DIR/fonts/"

echo ""
echo "==> 4. 拷贝模型资源到 output/jpov_fire_fog_viewer/models/"
mkdir -p "$OUTPUT_DIR/models"
cp -v "$PROJECT_DIR/tools/jpov/assets/models/samples/oak_negative/tripo_oak_4k.glb" "$OUTPUT_DIR/models/"
cp -v "$PROJECT_DIR/tools/jpov/assets/models/characters/mixamo_male.glb" "$OUTPUT_DIR/models/"
ls -lh "$OUTPUT_DIR/" "$OUTPUT_DIR/fonts/" "$OUTPUT_DIR/models/"

echo ""
echo "============================================"
echo "  编译完成！"
echo "============================================"
echo ""
echo "📂 产物位置："
echo "   $OUTPUT_DIR/jpov_fire_fog_viewer       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   交互窗口（需 DISPLAY/WSLg）："
echo "       $OUTPUT_DIR/jpov_fire_fog_viewer"
echo ""
echo "   验收：1280×720 不可 resize 窗口，场景为灰色地面 + 一棵高模橡树；"
echo "   点状雾默认摆在橡树中段。交互面板：雾中心 x/y/z、半径、强度 σ、衰减剖面、"
echo "   雾色 RGB、太阳仰角/方位角/浊度，以及**光照开关**（step1 base 发射 / step2 CSM）。"
echo "   视角变换与 model viewer 相同（右键 drag 转视角、滚轮 zoom）。"
echo ""
echo "   headless 拍摄："
echo "       $OUTPUT_DIR/jpov_fire_fog_viewer --capture <out_dir>"

if [ "$BUILD_WINDOWS" -eq 1 ]; then
    echo ""
    echo "📂 Windows 产物："
    echo "   $OUTPUT_DIR/jpov_fire_fog_viewer.exe   (Windows PE，静态链接，无 .dll 依赖)"
    echo "🪟 跑法：把整个 $OUTPUT_DIR 目录拷到 Windows，双击 jpov_fire_fog_viewer.exe"
    echo "   （字体/贴图/模型都是相对 exe 的路径，必须连目录一起拷）"
else
    echo ""
    echo "🪟 需要 Windows 版 exe：$0 --windows（额外交叉编译，产物落在同一目录）"
fi
