#!/usr/bin/env python3
"""make_pairs_v5.py — V5：Danis 手绘**原始**图（不做 M5）→ S1..S5 → 6 条两段链。

与 V4 唯一区别：不做 M5 柔化，直接用原始笔触。
流程：
  danis5.png → 5 格（每格只留自己的连通域，修好裁剪）→ F1..F5；
  S1..S5 = 相邻两张的 PROFILE 中点（F5→F1 回绕）；
  6 条链 × 12 帧（两段）→ mp/flames_pairs_v5_12x6.png。

用法：
  python3 make_pairs_v5.py
"""

import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import danis_soften as D  # noqa: E402
import morph_util as M    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets", "fire", "mp")
SRC = "/james_pm/danis5.png"
CELL = 256
SEG_N = 6
BLOCK_N = SEG_N * 2
CHAINS = [
    (1, 2, 5),
    (2, 3, 1),
    (3, 4, 2),
    (4, 5, 3),
    (5, 1, 4),
    (1, 3, 5),
]


def save_mask(a, path):
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(cell).save(path)


def main():
    cells = D.load_cells(SRC)          # 原始笔触，不做 M5
    F = {i: c for i, c in enumerate(cells, start=1)}
    for i in F:
        save_mask(F[i], os.path.join(ASSETS, f"danis_raw_f{i}.png"))
    print("raw f1..f5 saved")

    order = [1, 2, 3, 4, 5, 1]
    S = {}
    for k in range(5):
        S[k + 1] = M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
        save_mask(S[k + 1], os.path.join(ASSETS, f"danis_raw_s{k+1}_mid.png"))
    print("raw S1..S5 saved")

    rows, cols = len(CHAINS), BLOCK_N
    sheet = np.zeros((rows * CELL, cols * CELL, 4), np.uint8)
    for r, (a, b, c) in enumerate(CHAINS):
        seg1 = M.morph_frames(S[a], S[b], SEG_N, "profile")
        seg2 = M.morph_frames(S[b], S[c], SEG_N, "profile")
        frames = seg1 + seg2
        rr = rows - 1 - r
        for col, f in enumerate(frames):
            cell = np.zeros((CELL, CELL, 4), np.uint8)
            cell[..., 0:3] = 255
            cell[..., 3] = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
            sheet[rr * CELL:(rr + 1) * CELL, col * CELL:(col + 1) * CELL] = cell
        print(f"chain {r}: S{a}->S{b}->S{c}")

    out = os.path.join(ASSETS, "flames_pairs_v5_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


if __name__ == "__main__":
    main()
