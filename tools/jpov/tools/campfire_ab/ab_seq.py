#!/usr/bin/env python3
"""逐帧 A/B（diff 曲线）：对整个序列**每一帧**抓 JPOV 孤立火 → 与官方对应帧同口径比对。

资产帧 `k` ↔ 官方帧 `k + OFFSET`（默认 60，见 docs/jpov_campfire_repro_plan.md §1.1）。

产出：
  * 每帧一行指标（h/w、面积、均值 RGB、近白占比）→ 打印 + CSV；
  * 拼图 `strip.png`：逐帧 [官方 | 我们] 竖排，一眼看形态随帧的差距；
  * 可选 `--profile-plot`（无 matplotlib 时跳过）：略。

用法::

    python3 ab_seq.py --volume <campfire.jpovvol> --viewer <path/to/viewer> \\
        --refdir <官方参考帧目录> --frames 0:80:10 --outdir /tmp/ab_seq [--box 1.22]

参考帧须已在 `--refdir`（用 fetch_reference.py 抽）。
"""

import argparse
import os
import subprocess
import sys
from typing import Dict, List, Tuple

import numpy as np
from PIL import Image, ImageDraw

import ab_metrics

OFFICIAL_OFFSET = 60
STRIP_CELL_HEIGHT = 200


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


def capture(viewer: str, volume: str, frame: int, box: float, out_png: str) -> None:
    """headless 抓一帧孤立火（xvfb 包一层，glfwInit 需要 DISPLAY）。"""
    cmd = [
        "xvfb-run", "-a", viewer,
        "--ab-capture", out_png,
        "--volume", volume,
        "--ab-frame", str(frame),
        "--ab-box", f"{box}",
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
    ap.add_argument("--box", type=float, default=1.22)
    ap.add_argument("--offset", type=int, default=OFFICIAL_OFFSET)
    args = ap.parse_args(argv)

    os.makedirs(args.outdir, exist_ok=True)
    frames = parse_frames(args.frames)

    rows: List[Tuple[int, Dict[str, object], Dict[str, object]]] = []
    strip_rows: List[Image.Image] = []
    print(f"{'asset_f':>7} {'off_f':>6} | {'GT h/w':>7} {'our h/w':>7} | "
          f"{'GT area%':>8} {'our area%':>9} | {'GT rgb':>14} {'our rgb':>14}")
    for k in frames:
        ref_path = os.path.join(args.refdir, f"embergen_smallCampfire_f{k + args.offset:04d}.png")
        if not os.path.isfile(ref_path):
            print(f"{k:>7} {k + args.offset:>6} | 跳过：缺参考帧 {ref_path}")
            continue
        ours_path = os.path.join(args.outdir, f"ours_f{k:04d}.png")
        capture(args.viewer, args.volume, k, args.box, ours_path)

        ref_rgb = ab_metrics.load_rgb(ref_path)
        ours_rgb = ab_metrics.load_rgb(ours_path)
        m_gt = ab_metrics.measure(ref_rgb, ab_metrics.DEFAULT_THRESH)
        m_our = ab_metrics.measure(ours_rgb, ab_metrics.DEFAULT_THRESH)
        rows.append((k, m_gt, m_our))

        gt_rgb_s = "(" + ",".join(f"{v:.0f}" for v in m_gt["mean_rgb"]) + ")"  # type: ignore[union-attr]
        our_rgb_s = "(" + ",".join(f"{v:.0f}" for v in m_our["mean_rgb"]) + ")"  # type: ignore[union-attr]
        print(f"{k:>7} {k + args.offset:>6} | {m_gt['aspect']:>7.2f} {m_our['aspect']:>7.2f} | "
              f"{m_gt['area_pct']:>8.2f} {m_our['area_pct']:>9.2f} | {gt_rgb_s:>14} {our_rgb_s:>14}")

        strip_rows.append(strip_row(ref_rgb, ab_metrics.foreground_mask(ref_rgb, ab_metrics.DEFAULT_THRESH),
                                    ours_rgb, ab_metrics.foreground_mask(ours_rgb, ab_metrics.DEFAULT_THRESH)))

    csv_path = os.path.join(args.outdir, "ab_seq.csv")
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write("asset_frame,official_frame,gt_aspect,our_aspect,gt_area_pct,our_area_pct,"
                "gt_r,gt_g,gt_b,our_r,our_g,our_b\n")
        for k, m_gt, m_our in rows:
            gr, gg, gb = m_gt["mean_rgb"]  # type: ignore[misc]
            orr, og, ob = m_our["mean_rgb"]  # type: ignore[misc]
            f.write(f"{k},{k + args.offset},{m_gt['aspect']:.4f},{m_our['aspect']:.4f},"
                    f"{m_gt['area_pct']:.4f},{m_our['area_pct']:.4f},"
                    f"{gr:.2f},{gg:.2f},{gb:.2f},{orr:.2f},{og:.2f},{ob:.2f}\n")

    if strip_rows:
        width = max(r.width for r in strip_rows)
        height = sum(r.height + 4 for r in strip_rows) + 24
        canvas = Image.new("RGB", (width, height), (14, 14, 18))
        y = 24
        for k, r in zip([k for k, _, _ in rows], strip_rows):
            canvas.paste(r, (0, y))
            ImageDraw.Draw(canvas).text((4, y - 14), f"asset f{k}  /  official f{k + args.offset}",
                                        fill=(230, 230, 230))
            y += r.height + 4
        strip_path = os.path.join(args.outdir, "strip.png")
        canvas.save(strip_path)
        print(f"\nstrip: {strip_path}")

    print(f"csv:   {csv_path}")
    return 0 if rows else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
