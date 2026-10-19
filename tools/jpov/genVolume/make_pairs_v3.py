#!/usr/bin/env python3
"""make_pairs_v3.py — 成对渐变 V3：以「相邻原图的 PROFILE 中间帧」为基准形状。

步骤（Danis 的设计）：
  1) 5 张原始图 F1..F5 → 亮度转 alpha（掩膜）；
  2) 用 PROFILE 取相邻两张的中点：
         I12=profile(F1,F2,.5) → S1
         I23=profile(F2,F3,.5) → S2
         I34=profile(F3,F4,.5) → S3
         I45=profile(F4,F5,.5) → S4
         I51=profile(F5,F1,.5) → S5
  3) 在 S1..S5 之间用 PROFILE 预烘「两段链」：每块 12 帧（S_a→S_b, S_b→S_c），
     每颗粒子随机选一条链（端点随机）。
  4) 输出 12×6 atlas（6 条链），JPOV CellUV 行序。

输出：tools/jpov/assets/fire/mp/flames_pairs_v3_12x6.png

用法：
  python3 make_pairs_v3.py
"""

import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morph_util  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets", "fire", "mp")
SRC = "/james_pm/flipbooks_mp"
CELL = 256
SEG_N = 6                    # 每段 6 帧
BLOCK_N = SEG_N * 2          # 每块 12 帧（两段）

# 6 条链（S 下标 1..5）；其中第一条就是 Danis 举例的 (S1,S2,S5)
CHAINS = [
    (1, 2, 5),
    (2, 3, 1),
    (3, 4, 2),
    (4, 5, 3),
    (5, 1, 4),
    (1, 3, 5),
]


def load_src(n):
    p = os.path.join(SRC, f"Flame_{n}.jpg")
    a = np.array(Image.open(p).convert("RGB"), dtype=np.float32).max(axis=2) / 255.0
    a = np.array(Image.fromarray((a * 255).astype(np.uint8))
                 .resize((CELL, CELL), Image.BILINEAR), dtype=np.float32) / 255.0
    return a


def save_mask(a, path):
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(cell).save(path)


def main():
    F = {i: load_src(i) for i in range(1, 6)}

    # 1) 相邻中点 S1..S5
    order = [1, 2, 3, 4, 5, 1]
    S = {}
    for k in range(5):
        a, b = order[k], order[k + 1]
        S[k + 1] = morph_util.morph_profile(F[a], F[b], 0.5)
        save_mask(S[k + 1], os.path.join(ASSETS, f"s{k+1}_mid.png"))
    print("S1..S5 done")

    # 2) 两段链（PROFILE 预烘）
    rows, cols = len(CHAINS), BLOCK_N
    sheet = np.zeros((rows * CELL, cols * CELL, 4), np.uint8)
    for r, (a, b, c) in enumerate(CHAINS):
        seg1 = morph_util.morph_frames(S[a], S[b], SEG_N, "profile")
        seg2 = morph_util.morph_frames(S[b], S[c], SEG_N, "profile")
        frames = seg1 + seg2
        rr = rows - 1 - r          # JPOV CellUV 行序
        for col, f in enumerate(frames):
            cell = np.zeros((CELL, CELL, 4), np.uint8)
            cell[..., 0:3] = 255
            cell[..., 3] = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
            sheet[rr * CELL:(rr + 1) * CELL, col * CELL:(col + 1) * CELL] = cell
        print(f"chain {r}: S{a}->S{b}->S{c}")

    out = os.path.join(ASSETS, "flames_pairs_v3_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


if __name__ == "__main__":
    main()
