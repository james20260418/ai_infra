#!/usr/bin/env python3
"""逐帧 A/B（diff 曲线）：对整个序列**每一帧**抓 JPOV 孤立火 → 与官方对应帧同口径比对。

资产帧 `k` ↔ 官方帧 `k + OFFSET`（默认 60，见 docs/jpov_campfire_repro_plan.md §1.1）。

产出：
  * 每帧一行指标（h/w、面积、均值 RGB）→ 打印 + CSV；
  * 拼图 `strip.png`：逐帧 [官方 | 我们] 竖排，一眼看形态随帧的差距。

为什么它是「范式」而不只是个脚本：
  * 纯数值、无 vision 模型 ⇒ 确定性、可无人值守，直接当机制补齐（P1..P6）的**自动验收门禁**；
  * **只有逐帧才看得见**的问题：闪烁、循环接缝、烟随时间的演化；
  * 顺带**验证帧对应**（映射错则曲线跳/乱）。

用法::

    python3 ab_seq.py --volume <campfire.jpovvol> --viewer <path/to/viewer> \\
        --refdir <官方参考帧目录> --frames 0:80:10 --outdir /tmp/ab_seq [--box 1.22]

参考帧须已在 `--refdir`（用 fetch_reference.py 抽）。
"""

import argparse
import json
import os
import subprocess
import sys
from typing import List, Tuple

import numpy as np
from PIL import Image, ImageDraw

import ab_metrics

# 资产帧 k ↔ 官方帧 k + OFFSET（与 docs/jpov_campfire_repro_plan.md §1.1 一致）。
OFFICIAL_OFFSET = 60
# 拼图每行（[官方 | 我们]）统一缩放后的高度（像素）。
STRIP_CELL_HEIGHT = 200


def derive_box_xyz(volume: str) -> str:
    """从 sidecar `<volume>.json` 的 dims×voxel_m 推**各轴**尺寸（米）→ "x,y,z"；缺 sidecar 返回 ""。"""
    try:
        with open(volume + ".json", encoding="utf-8") as f:
            side = json.load(f)
        vm = float(side["voxel_m"])
        return ",".join(f"{v * vm:.3f}" for v in side["dims"])
    except Exception:
        return ""


def parse_frames(spec: str) -> List[int]:
    """把 '0:80:10'（start:stop:step）或 '0,10,20' 解析成帧号列表。"""
    if ":" in spec:
        parts = [int(p) for p in spec.split(":")]
        if len(parts) == 3:
            return list(range(parts[0], parts[1], parts[2]))
        if len(parts) == 2:
            return list(range(parts[0], parts[1]))
        raise ValueError(f"无法解析 --frames: {spec}")
    return [int(p) for p in spec.split(",") if p != ""]


def capture(viewer: str, volume: str, frame: int, box: str, y_shift: float, out_png: str) -> None:
    """headless 抓一帧孤立火（xvfb 包一层，glfwInit 需要 DISPLAY）。

    box 形如 "x,y,z"（各轴独立，不拉伸资产）；y_shift 为取景下移（米）。
    """
    cmd = [
        "xvfb-run", "-a", viewer,
        "--ab-capture", out_png,
        "--volume", volume,
        "--ab-frame", str(frame),
        "--ab-box", box,
        "--ab-y-shift", f"{y_shift}",
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def strip_row(ref_rgb: np.ndarray, ref_mask: np.ndarray,
              ours_rgb: np.ndarray, ours_mask: np.ndarray) -> Image.Image:
    """一行 = [官方 | 我们]，两侧等比缩到同高。"""
    ref_c, _ = ab_metrics.crop_to_mask(ref_rgb, ref_mask, ab_metrics.CROP_MARGIN)
    ours_c, _ = ab_metrics.crop_to_mask(ours_rgb, ours_mask, ab_metrics.CROP_MARGIN)

    def to_image(arr: np.ndarray) -> Image.Image:
        im = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
        scale = STRIP_CELL_HEIGHT / im.height
        return im.resize((max(1, int(im.width * scale)), STRIP_CELL_HEIGHT))

    a, b = to_image(ref_c), to_image(ours_c)
    row = Image.new("RGB", (a.width + b.width + 12, STRIP_CELL_HEIGHT), (0, 0, 0))
    row.paste(a, (0, 0))
    row.paste(b, (a.width + 12, 0))
    return row


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Per-frame campfire A/B diff")
    ap.add_argument("--volume", required=True)
    ap.add_argument("--viewer", required=True)
    ap.add_argument("--refdir", required=True)
    ap.add_argument("--frames", default="0:80:10", help="'0:80:10' 或 '0,10,20'")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--box", default="",
                    help="A/B 火盒 'X,Y,Z'（米）；缺省时从 <volume>.json 的 dims×voxel_m 推")
    ap.add_argument("--y-shift", type=float, default=-0.10,
                    help="A/B 取景下移（米），用于与参考画面基线对齐")
    ap.add_argument("--offset", type=int, default=OFFICIAL_OFFSET, help="官方帧偏移")
    args = ap.parse_args(argv)

    box = args.box if args.box else derive_box_xyz(args.volume)
    if not box:
        print(f"错误：无法从 {args.volume}.json 推火盒尺寸，请显式 --box X,Y,Z", file=sys.stderr)
        return 2

    os.makedirs(args.outdir, exist_ok=True)
    frames = parse_frames(args.frames)

    rows: List[Tuple[int, ab_metrics.Metrics, ab_metrics.Metrics]] = []
    strips: List[Image.Image] = []
    print(f"{'asset_f':>7} {'off_f':>6} | {'GT h/w':>7} {'our h/w':>7} | "
          f"{'GT area%':>8} {'our area%':>9} | {'GT rgb':>14} {'our rgb':>14}")
    print(f"(box = {box} m, y_shift = {args.y_shift} m)")
    for k in frames:
        ref_path = os.path.join(
            args.refdir, f"embergen_smallCampfire_f{k + args.offset:04d}.png")
        if not os.path.isfile(ref_path):
            print(f"{k:>7} {k + args.offset:>6} | 跳过：缺参考帧 {ref_path}")
            continue
        ours_path = os.path.join(args.outdir, f"ours_f{k:04d}.png")
        capture(args.viewer, args.volume, k, box, args.y_shift, ours_path)

        ref_rgb = ab_metrics.load_rgb(ref_path)
        ours_rgb = ab_metrics.load_rgb(ours_path)
        m_gt = ab_metrics.measure(ref_rgb, ab_metrics.DEFAULT_THRESH)
        m_our = ab_metrics.measure(ours_rgb, ab_metrics.DEFAULT_THRESH)
        rows.append((k, m_gt, m_our))

        gt_rgb_s = "(" + ",".join(f"{v:.0f}" for v in m_gt.mean_rgb) + ")"
        our_rgb_s = "(" + ",".join(f"{v:.0f}" for v in m_our.mean_rgb) + ")"
        print(f"{k:>7} {k + args.offset:>6} | {m_gt.aspect:>7.2f} {m_our.aspect:>7.2f} | "
              f"{m_gt.area_pct:>8.2f} {m_our.area_pct:>9.2f} | {gt_rgb_s:>14} {our_rgb_s:>14}")

        strips.append(strip_row(
            ref_rgb, ab_metrics.foreground_mask(ref_rgb, ab_metrics.DEFAULT_THRESH),
            ours_rgb, ab_metrics.foreground_mask(ours_rgb, ab_metrics.DEFAULT_THRESH)))

    csv_path = os.path.join(args.outdir, "ab_seq.csv")
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write("asset_frame,official_frame,gt_aspect,our_aspect,gt_area_pct,our_area_pct,"
                "gt_r,gt_g,gt_b,our_r,our_g,our_b\n")
        for k, m_gt, m_our in rows:
            gr, gg, gb = m_gt.mean_rgb
            orr, og, ob = m_our.mean_rgb
            f.write(f"{k},{k + args.offset},{m_gt.aspect:.4f},{m_our.aspect:.4f},"
                    f"{m_gt.area_pct:.4f},{m_our.area_pct:.4f},"
                    f"{gr:.2f},{gg:.2f},{gb:.2f},{orr:.2f},{og:.2f},{ob:.2f}\n")

    if strips:
        width = max(r.width for r in strips)
        height = sum(r.height + 4 for r in strips) + 24
        canvas = Image.new("RGB", (width, height), (14, 14, 18))
        draw = ImageDraw.Draw(canvas)
        y = 24
        for (k, _, _), r in zip(rows, strips):
            draw.text((4, y - 14), f"asset f{k}  /  official f{k + args.offset}",
                      fill=(230, 230, 230))
            canvas.paste(r, (0, y))
            y += r.height + 4
        strip_path = os.path.join(args.outdir, "strip.png")
        canvas.save(strip_path)
        print(f"\nstrip: {strip_path}")

    print(f"csv:   {csv_path}")
    return 0 if rows else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
