#!/usr/bin/env bash
# =============================================================================
# run_ab.sh — 篝火 A/B 基准：抓 JPOV 孤立火 → 与 EmberGen 官方参考帧同口径比对
#
# 用法：
#   bash tools/jpov/tools/campfire_ab/run_ab.sh <campfire.jpovvol> [asset_frame]
# 例：
#   bash tools/jpov/tools/campfire_ab/run_ab.sh \
#     /james_pm/fire_hero/assets/prototypes/campfire.jpovvol 20
#
# 帧对应（见 docs/jpov_campfire_repro_plan.md §1.1）：
#   资产帧 k  ↔  官方帧 60 + k
#
# 依赖：已编译 viewer（output/jpov_volume_fire_viewer/jpov_volume_fire_viewer）、
#       xvfb-run（headless GL）、python3 + numpy + Pillow。
# 可用环境变量覆盖：AB_BOX（火盒边长，米）、OUT_DIR（输出目录）。
# =============================================================================
set -euo pipefail

VOLUME="${1:-}"
FRAME="${2:-20}"
OFFICIAL_OFFSET=60            # 资产帧 k ↔ 官方帧 60+k

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
VIEWER="$PROJECT_DIR/output/jpov_volume_fire_viewer/jpov_volume_fire_viewer"
OUT_DIR="${OUT_DIR:-/tmp/campfire_ab}"

if [[ -z "$VOLUME" || ! -f "$VOLUME" ]]; then
  echo "用法：run_ab.sh <campfire.jpovvol> [asset_frame]；资产不存在：$VOLUME" >&2
  exit 1
fi
if [[ ! -x "$VIEWER" ]]; then
  echo "找不到 viewer：$VIEWER（先 bash tools/jpov/build_volume_fire_viewer.sh）" >&2
  exit 1
fi

# 火盒边长：优先 AB_BOX；否则从 sidecar 的 dims×voxel_m 取最大边；再否则 1.3。
if [[ -z "${AB_BOX:-}" ]]; then
  SIDECAR="$VOLUME.json"
  AB_BOX="$(python3 - "$SIDECAR" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    dims, vm = d["dims"], float(d["voxel_m"])
    print(f"{max(dims) * vm:.3f}")
except Exception:
    print("1.3")
PY
)"
fi

REF_FRAME=$((OFFICIAL_OFFSET + FRAME))
REF=$(printf "%s/reference/embergen_smallCampfire_f%04d.png" "$SCRIPT_DIR" "$REF_FRAME")
OURS="$OUT_DIR/ours_f$(printf '%04d' "$FRAME").png"
mkdir -p "$OUT_DIR"

if [[ ! -f "$REF" ]]; then
  echo "缺参考帧：$REF（用 fetch_reference.py 从官方 mp4 抽帧，帧号 $REF_FRAME）" >&2
  exit 1
fi

echo "== 1/2 抓 JPOV 孤立火（asset frame=$FRAME, box=${AB_BOX}m）=="
xvfb-run -a "$VIEWER" --ab-capture "$OURS" --volume "$VOLUME" \
  --ab-frame "$FRAME" --ab-box "$AB_BOX"

echo "== 2/2 比对（参考帧 $REF_FRAME vs 我们）=="
python3 "$SCRIPT_DIR/ab_metrics.py" --gt "$REF" --ours "$OURS" \
  --out "$OUT_DIR/ab_f$(printf '%04d' "$FRAME").png" \
  --report "$OUT_DIR/ab_f$(printf '%04d' "$FRAME").txt"

echo
echo "产物："
echo "  我们：  $OURS"
echo "  并排图：$OUT_DIR/ab_f$(printf '%04d' "$FRAME").png"
echo "  报告：  $OUT_DIR/ab_f$(printf '%04d' "$FRAME").txt"
