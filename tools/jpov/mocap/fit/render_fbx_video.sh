#!/usr/bin/env bash
# FBX → mp4（动捕算法对比验收）。**渲染与编码都在 jpov_fbx_viewer 里完成**，
# 本脚本只负责选参数（2026-11-04 起不再逐帧 spawn 进程）。
#
# 用法：render_fbx_video.sh <fbx> <out.mp4> [fps] [frames]
#   VIEW=9                  -> 只看蓝人模特（默认；不需要 glb）
#   VIEW=8                  -> 红骨人 + 蓝人模特
#   GLB=<glb 路径> VIEW=5   -> 旧的「glb 真皮带皮」对照（资产自带 rest 是蹲姿，慎用）
#   AZ=0 EL=5               -> 第一机位（度）
#   AZ2=90                  -> 给第二机位 => 输出两角度并排（2560×720）
set -euo pipefail

FBX="$1"
OUT="$2"
FPS="${3:-24}"
FRAMES="${4:-121}"

HERE="$(cd "$(dirname "$0")" && pwd)"
VIEWER="${VIEWER:-$HERE/../../../../output/jpov_fbx_viewer/jpov_fbx_viewer}"
VIEW="${VIEW:-9}"
AZ="${AZ:-0}"
EL="${EL:-5}"

ARGS=("$FBX")
if [ -n "${GLB:-}" ]; then
  ARGS+=("$GLB")
fi
ARGS+=(--view "$VIEW" --video "$OUT" --frames "$FRAMES" --fps "$FPS"
       --azimuth-deg "$AZ" --elev-deg "$EL")
if [ -n "${AZ2:-}" ]; then
  ARGS+=(--azimuth2-deg "$AZ2")
fi

"$VIEWER" "${ARGS[@]}"
echo "wrote $OUT"
