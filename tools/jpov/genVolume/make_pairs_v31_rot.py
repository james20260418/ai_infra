#!/usr/bin/env python3
"""make_pairs_v31_rot.py — V3.1 + 对 F 源图做旋转平均（削掉外缘尖角）。

在 V3.1 的 5 张手绘 F 上做 `rot_avg.avg_limited(L, half, sigma)`，
再走 V3 管线（S1..S5 中点 + 6 条两段链）。

用法：
  python3 make_pairs_v31_rot.py --half 15 --tag v31r15
  python3 make_pairs_v31_rot.py --half 30 --tag v31r30
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morph_util as M  # noqa: E402
import rot_avg as R     # noqa: E402
from make_pairs_v31 import load_five, save_mask, CHAINS, CELL, SEG_N, ASSETS  # noqa: E402

BLOCK_N = SEG_N * 2


def build(F, tag):
    order = [1, 2, 3, 4, 5, 1]
    S = {k + 1: M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
         for k in range(5)}
    for i in F:
        save_mask(F[i], os.path.join(ASSETS, f"{tag}_f{i}.png"))
    for k in S:
        save_mask(S[k], os.path.join(ASSETS, f"{tag}_s{k}_mid.png"))

    rows, cols = len(CHAINS), BLOCK_N
    sheet = np.zeros((rows * CELL, cols * CELL, 4), np.uint8)
    for r, (a, b, c) in enumerate(CHAINS):
        frames = (M.morph_frames(S[a], S[b], SEG_N, "profile") +
                  M.morph_frames(S[b], S[c], SEG_N, "profile"))
        rr = rows - 1 - r
        for col, f in enumerate(frames):
            cell = np.zeros((CELL, CELL, 4), np.uint8)
            cell[..., 0:3] = 255
            cell[..., 3] = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
            sheet[rr * CELL:(rr + 1) * CELL, col * CELL:(col + 1) * CELL] = cell
        print(f"chain {r}: S{a}->S{b}->S{c}")
    out = os.path.join(ASSETS, f"flames_pairs_{tag}_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--half", type=float, default=15.0)
    ap.add_argument("--sigma", type=float, default=8.0)
    ap.add_argument("--tag", default="v31r15")
    args = ap.parse_args()

    base = load_five("/james_pm/danis_F5_v2.png")
    F = {}
    for i, L in enumerate(base, start=1):
        V = R.avg_limited(L, args.half, args.sigma)
        F[i] = V
    print(f"rotation-averaged F1..F5 (±{args.half}°)")
    build(F, args.tag)


if __name__ == "__main__":
    main()
