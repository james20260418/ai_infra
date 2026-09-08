#!/usr/bin/env bash
# =============================================================================
# gen3d_skeleton.sh — JPOV 带骨骼 3D 生成链路（占位/骨架）
#
# 本脚本对应 gen3d/skeleton/ 目录：让模型带骨骼（mixamorig rig）+ 蒙皮动画。
# 与 gen3d_static.sh（纯静态 PBR）并列的孪生链路。
#
# 背景（2026-09-08 设计收敛，详见 docs/jpov_clothes_rig_design.md）：
#   静态 gen3d 走 Tripo text-to-model（gen3d/static/）；带骨骼要 Tripo rig/mixamo，
#     skeleton 相关代码落在 gen3d/skeleton/。gltf loader 尚未接 skins/animations（本期不动）。
#
# 当前状态：★ 占位骨架 ★ —— 实际生成逻辑尚未实现。
#   本期 PR 只建好 gen3d/skeleton/ 的目录/结构/命名空间占位，真实带骨骼生成与渲染动画面后续。
#   直接调用本脚本会走到"未实现"分支并打印说明，不会误触发任何真实生成。
#
# 预期最终形态（后续实现后替换占位逻辑）：
#   - bazel 编译 gen3d_skeleton_cmd（调 Tripo rig/mixamo）输出带骨骼 GLB + 动画
#   （具体参数/接口待 gen3d/skeleton/ 实际落地时定，此处不臆造）
# =============================================================================
set -euo pipefail

echo "==> gen3d_skeleton.sh: 占位脚本，带骨骼生成链路尚未实现。" >&2
echo "    本期 PR 只建 gen3d/skeleton/ 结构（见 tools/jpov/gen3d/skeleton/），loader 未接骨骼。" >&2
echo "    请等后续实现；如需生成静态模型请用 tools/jpov/gen3d_static.sh。" >&2
exit 1
