#!/usr/bin/env bash
# =============================================================================
# build_jpov_model_editor.sh — 编译 jpov_model_editor（JPOV 模型编辑器）
#
# 用法：
#   ./tools/jpov/build_jpov_model_editor.sh
#
# 效果：
#   1. bazel build //tools/jpov/demo/editor:jpov_model_editor（Linux ELF）
#   2. 产物 + 字体拷贝到工程 output/jpov_model_editor/ 下
#   3. 打印交互操作说明
#
# 运行（交互窗口，需 DISPLAY/WSLg）：
#   output/jpov_model_editor/jpov_model_editor <gltf/glb 路径>
#   未指定路径时 fallback 到项目内 pliers.gltf 演示。
#
# ⚠️ 权限提示（Danis 2026-09-14 实测）：
#   **运行时若报权限/无法打开输出目录等错误，用 sudo 跑本脚本或产物**：
#       sudo ./tools/jpov/build_jpov_model_editor.sh
#       sudo output/jpov_model_editor/jpov_model_editor model.glb
#   （本项目在部分环境下 bazel 缓存 / output 目录属 root，不加 sudo 会写入失败；
#     "没加 sudo，加上就好了" —— 不要在这上面浪费时间排查。）
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# ⚠️ 二进制路径必须与上面的 bazel target 一致（曾经因为这里写旧路径
#    bazel-bin/tools/jpov/ 而把旧产物拷进 output/，导致"编译了但跑的是老程序"）。
BAZEL_BIN="$PROJECT_DIR/bazel-bin/tools/jpov/demo/editor"
OUTPUT_DIR="$PROJECT_DIR/output/jpov_model_editor"

echo "==> 0. 确保输出目录存在"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo ""
echo "==> 1. 编译 Linux 版 jpov_model_editor"
cd "$PROJECT_DIR"
bazel build //tools/jpov/demo/editor:jpov_model_editor 2>&1
echo ""
echo "OK: Linux 编译完成"

echo ""
echo "==> 2. 拷贝产物到 output/jpov_model_editor/"
cp -v "$BAZEL_BIN/jpov_model_editor" "$OUTPUT_DIR/jpov_model_editor"
ls -lh "$OUTPUT_DIR/"

# 拷贝分发态字体到 exe 旁 fonts/（cfg.fonts 声明的是相对 exe 路径，
# ResolveFontPath 按 exe 相对路径命中；CJK 显中文，DejaVu 做拉丁回退）。
echo ""
echo "==> 3. 拷贝字体资源到 output/jpov_model_editor/fonts/"
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
echo "   $OUTPUT_DIR/jpov_model_editor       (Linux ELF)"
echo ""
echo "🧪 用法："
echo "   $OUTPUT_DIR/jpov_model_editor /absolute/path/to/model.glb"
echo "   （也可传相对路径；未指定时用项目内 pliers.gltf 演示）"
echo ""
echo "⚠️  若报权限错误（无法写入 / 无法打开输出目录），**请加 sudo 重跑**："
echo "      sudo ./tools/jpov/build_jpov_model_editor.sh"
echo "      sudo $OUTPUT_DIR/jpov_model_editor model.glb"
echo "   （部分环境 bazel 缓存 / output 属 root，不加 sudo 会写入失败。）"
echo ""
echo "🖐  交互："
echo "   左键横向拖动          = 模型绕世界 X 轴旋转（右滑逆时针，纵向分量忽略）"
echo "   Ctrl + 左键横向拖动   = 模型绕世界 Y 轴旋转（右滑逆时针）"
echo "   右键拖动 / 滚轮       = 相机环绕 / 缩放"
echo ""
echo "🎚  底部面板（缩放 0.1~10 / 平移 XYZ ±3 / 地面高度 -3~0 / 保存按钮）："
echo "   变换作用顺序 = 先缩放 → 再旋转 → 最后平移（对齐 DrawObject3D 语义）。"
echo ""
echo "📐  坐标架（辅助对位）：模型处有 1m 三色坐标架（随模型平移）："
echo "   红 = X 轴、绿 = Y 轴、蓝 = Z 轴；宽 5cm，alpha 0.2 近乎透明。"
echo "   用来看模型的朝向对不对、尺度量级是否合理。"
echo "   ⚙  保存：点击底部『保存』→ 后台线程把当前放置烘进 CPU 资产写入 glb；"
echo "      文件名 <原stem>_edit<YYYYMMDD-HHMMSS>.glb，落在**原 glb 同目录**；"
echo "      完成后左上角追加『保存至 <路径>』。保存中按钮显『保存中...』且不可重复触发。"
echo "      注：保存按刚体放置（scale 视为 1）写资产（骨架一致性要求，见 mesh_transform.h）。"
