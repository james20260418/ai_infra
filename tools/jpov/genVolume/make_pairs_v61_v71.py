#!/usr/bin/env python3
"""make_pairs_v61_v71.py — V6.1 / V7.1：在 V6/V7 基础上做旋转平均（默认 limited15）。

- 所有源图先做 `rot_avg.avg_limited(L, 15°, σ=8°)`（有限角度加权平均）；
- V6.1：S5 = 旋转平均后的 danis_S5 候选；V7.1：F5 = 同一个候选；
- 其余与 V6/V7 相同（6 条两段链 × 12 帧）。

用法：
  python3 make_pairs_v61_v71.py --cand 4 --half 15
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import danis_soften as D   # noqa: E402
import morph_util as M     # noqa: E402
import rot_avg as R        # noqa: E402
from make_pairs_v6_v7 import load_vertical_candidates, CHAINS, CELL, SEG_N  # noqa: E402

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                      "assets", "fire", "mp")
BLOCK_N = SEG_N * 2


def save_mask(a, path):
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(cell).save(path)


def build(S, tag):
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
    out = os.path.join(ASSETS, f"flames_pairs_{tag}_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cand", type=int, default=4)
    ap.add_argument("--half", type=float, default=15.0)
    ap.add_argument("--sigma", type=float, default=8.0)
    args = ap.parse_args()

    base = D.load_cells("/james_pm/danis5.png")
    cand = load_vertical_candidates("/james_pm/danis_S5.png")[args.cand - 1]

    def ravg(L):
        V = R.avg_limited(L, args.half, args.sigma)
        save_mask(V, os.path.join(ASSETS, "tmp_ravg.png"))
        return V

    F = {i: ravg(base[i - 1]) for i in range(1, 6)}
    C = ravg(cand)
    for i in F:
        save_mask(F[i], os.path.join(ASSETS, f"v61_f{i}.png"))
    save_mask(C, os.path.join(ASSETS, "v61_cand.png"))

    # V6.1：S1..S4 中点，S5=C
    order = [1, 2, 3, 4, 5, 1]
    S6 = {k + 1: M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
          for k in range(4)}
    S6[5] = C
    build(S6, "v61")

    # V7.1：F5=C，S1..S5 全部中点
    F7 = dict(F)
    F7[5] = C
    S7 = {k + 1: M.morph_profile(F7[order[k]], F7[order[k + 1]], 0.5)
          for k in range(5)}
    build(S7, "v71")


if __name__ == "__main__":
    main()
