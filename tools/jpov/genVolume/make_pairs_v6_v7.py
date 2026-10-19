#!/usr/bin/env python3
"""make_pairs_v6_v7.py — 用 danis_S5.png（纵向 5 个候选）替换 S5 / F5。

- V6：F1..F5 = danis5.png 原始手绘；S1..S4 = 相邻中点；**S5 = 指定候选**。
- V7：F1..F4 = 原始；**F5 = 指定候选**；S1..S5 = 相邻中点（含 S5=mid(F5,F1)）。

danis_S5.png 是 1024×2910 的**纵向**排列（5 个独立连通域），按 y 排序拆格，
每格只保留自己的连通域；右下角两个小水印碎片按面积过滤掉。

用法：
  python3 make_pairs_v6_v7.py --cand 4
  # → mp/flames_pairs_v6_12x6.png / mp/flames_pairs_v7_12x6.png
"""

import argparse
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import danis_soften as D  # noqa: E402
import morph_util as M    # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets", "fire", "mp")
SRC5 = "/james_pm/danis5.png"
SRCS = "/james_pm/danis_S5.png"
CELL = 256
SEG_N = 6
BLOCK_N = SEG_N * 2
CHAINS = [(1, 2, 5), (2, 3, 1), (3, 4, 2), (4, 5, 3), (5, 1, 4), (1, 3, 5)]


def load_vertical_candidates(path):
    """纵向 sheet → 5 个 256² 候选（按 y 排序）。"""
    im = np.array(Image.open(path).convert("RGB"), dtype=np.float32)
    h, w, _ = im.shape
    L = im.max(axis=2) / 255.0
    m = (L > 0.04).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    idx = [i for i in range(1, n) if stats[i, 4] > 50000]
    idx.sort(key=lambda i: stats[i, 1])          # 按 y（纵向）
    out = []
    for i in idx:
        x0, y0, bw, bh = stats[i, 0], stats[i, 1], stats[i, 2], stats[i, 3]
        own = np.where(lab == i, L, 0.0).astype(np.float32)
        side = int(max(bw, bh) + 20)
        x1 = int(x0 - (side - bw) / 2)
        y1 = int(y0 - (side - bh) / 2)
        crop = np.zeros((side, side), np.float32)
        sx0, sy0 = max(0, x1), max(0, y1)
        sx1, sy1 = min(w, x1 + side), min(h, y1 + side)
        crop[sy0 - y1:sy0 - y1 + (sy1 - sy0), sx0 - x1:sx0 - x1 + (sx1 - sx0)] = \
            own[sy0:sy1, sx0:sx1]
        out.append(cv2.resize(crop, (CELL, CELL), interpolation=cv2.INTER_AREA))
    return out


def save_mask(a, path):
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(cell).save(path)


def build(F, tag):
    order = [1, 2, 3, 4, 5, 1]
    S = {k + 1: M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
         for k in range(5)}
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
    out = os.path.join(ASSETS, f"flames_pairs_{tag}_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cand", type=int, default=4,
                    help="用 danis_S5.png 的第几个候选（1..5，从上到下）")
    args = ap.parse_args()

    base = D.load_cells(SRC5)                 # 原始 F1..F5
    cands = load_vertical_candidates(SRCS)
    print(f"danis_S5: {len(cands)} candidates; use #{args.cand}")
    for k, c in enumerate(cands, 1):
        save_mask(c, os.path.join(ASSETS, f"danis_s5cand{k}.png"))
    repl = cands[args.cand - 1]

    # V6：替换 S5
    F6 = {i: base[i - 1] for i in range(1, 6)}
    S5_v6 = repl
    order = [1, 2, 3, 4, 5, 1]
    S6 = {k + 1: M.morph_profile(F6[order[k]], F6[order[k + 1]], 0.5)
          for k in range(4)}                   # S1..S4
    S6[5] = S5_v6
    for k in S6:
        save_mask(S6[k], os.path.join(ASSETS, f"v6_s{k}_mid.png"))
    # 复用 build 逻辑：直接给 S
    rows, cols = len(CHAINS), BLOCK_N
    sheet = np.zeros((rows * CELL, cols * CELL, 4), np.uint8)
    for r, (a, b, c) in enumerate(CHAINS):
        frames = (M.morph_frames(S6[a], S6[b], SEG_N, "profile") +
                  M.morph_frames(S6[b], S6[c], SEG_N, "profile"))
        rr = rows - 1 - r
        for col, f in enumerate(frames):
            cell = np.zeros((CELL, CELL, 4), np.uint8)
            cell[..., 0:3] = 255
            cell[..., 3] = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
            sheet[rr * CELL:(rr + 1) * CELL, col * CELL:(col + 1) * CELL] = cell
    out6 = os.path.join(ASSETS, "flames_pairs_v6_12x6.png")
    Image.fromarray(sheet).save(out6)
    print("wrote", out6, sheet.shape)

    # V7：替换 F5
    F7 = {i: base[i - 1] for i in range(1, 5)}
    F7[5] = repl
    build(F7, "v7")


if __name__ == "__main__":
    main()
