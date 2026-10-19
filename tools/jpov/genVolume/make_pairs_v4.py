#!/usr/bin/env python3
"""make_pairs_v4.py — V4：Danis 手绘 5 图 + M5 柔化，再接 V3 管线。

流程：
  1) `/james_pm/danis5.png` → 按连通域拆 5 格（每格只保留自己的像素；
     修正了旧版裁剪偏移，第二格不再带第一张的尾巴）；
  2) 每格做 M5（DoG 亮核 + 向上摆动拖尾 + 宽软底）；
  3) 保存 `mp/danis_f1..f5.png`（RGB=白，A=处理后形状）；
  4) 与 V3 相同：S1..S5 = 相邻两张的 PROFILE 中点；6 条链 × 两段 × 6 帧
     → `mp/flames_pairs_v4_12x6.png`（3072×1536）。

用法：
  python3 make_pairs_v4.py
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
    cells = D.load_cells(SRC)
    F = {}
    for i, c in enumerate(cells, start=1):
        F[i] = D.m5_wisp(c)
        save_mask(F[i], os.path.join(ASSETS, f"danis_f{i}.png"))
    print("danis f1..f5 (M5) saved")

    order = [1, 2, 3, 4, 5, 1]
    S = {}
    for k in range(5):
        S[k + 1] = M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
        save_mask(S[k + 1], os.path.join(ASSETS, f"danis_s{k+1}_mid.png"))
    print("S1..S5 saved")

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

    out = os.path.join(ASSETS, "flames_pairs_v4_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


if __name__ == "__main__":
    main()
