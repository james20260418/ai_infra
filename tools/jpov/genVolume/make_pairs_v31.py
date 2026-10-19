#!/usr/bin/env python3
"""make_pairs_v31.py — V3.1：用 Danis 手绘简单版（danis_F5_v2.png）替代 V3 的原始 F1~F5。

源图是 1773×2364 的纵向 5 连图（阈值 0.3 时正好分成 5 个连通域，低阈值下被辉光连在一起）。
按 y 分成 5 条带，每带只保留自己的像素，正方形裁切 → 256² → F1..F5；
之后与 V3 完全相同：S1..S5 = 相邻中点，6 条两段链 × 12 帧。

用法：
  python3 make_pairs_v31.py
"""

import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morph_util as M  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets", "fire", "mp")
SRC = "/james_pm/danis_F5_v2.png"
CELL = 256
SEG_N = 6
BLOCK_N = SEG_N * 2
CHAINS = [(1, 2, 5), (2, 3, 1), (3, 4, 2), (4, 5, 3), (5, 1, 4), (1, 3, 5)]


def load_five(path):
    im = np.array(Image.open(path).convert("RGB"), dtype=np.float32)
    h, w, _ = im.shape
    L = im.max(axis=2) / 255.0
    L[2200:, 1400:] = 0                       # 右下角水印
    m = (L > 0.30).astype(np.uint8)           # 高阈值分开 5 个形状
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    idx = [i for i in range(1, n) if stats[i, 4] > 20000]
    idx.sort(key=lambda i: stats[i, 1])
    assert len(idx) >= 5, f"只找到 {len(idx)} 个形状"

    out = []
    for i in idx[:5]:
        # 每个形状的「自己的辉光」：把高阈值 mask 膨胀 28px 当窗口，
        # 不要用水平带硬切（硬切会在上下边缘留下直边）。
        mask = (lab == i).astype(np.uint8)
        mask = cv2.dilate(mask, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (57, 57)))
        own = L * mask
        own = np.clip((own - 0.25) / 0.47, 0.0, 1.0)   # 压辉光，只留核心
        ys, xs = np.nonzero(mask)
        x0, x1 = xs.min(), xs.max()
        y0, y1 = ys.min(), ys.max()
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        side = int(max(bw, bh) + 32)
        x1c = int(x0 - (side - bw) / 2)
        y1c = int(y0 - (side - bh) / 2)
        crop = np.zeros((side, side), np.float32)
        sx0, sy0 = max(0, x1c), max(0, y1c)
        sx1, sy1 = min(w, x1c + side), min(h, y1c + side)
        crop[sy0 - y1c:sy0 - y1c + (sy1 - sy0), sx0 - x1c:sx0 - x1c + (sx1 - sx0)] = \
            own[sy0:sy1, sx0:sx1]
        out.append(cv2.resize(crop, (CELL, CELL), interpolation=cv2.INTER_AREA))
    return out

def save_mask(a, path):
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(cell).save(path)


def main():
    F = {i: c for i, c in enumerate(load_five(SRC), start=1)}
    for i in F:
        save_mask(F[i], os.path.join(ASSETS, f"v31_f{i}.png"))
    print("v31 f1..f5 saved")

    order = [1, 2, 3, 4, 5, 1]
    S = {k + 1: M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
         for k in range(5)}
    for k in S:
        save_mask(S[k], os.path.join(ASSETS, f"v31_s{k}_mid.png"))
    print("v31 S1..S5 saved")

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

    out = os.path.join(ASSETS, "flames_pairs_v31_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


if __name__ == "__main__":
    main()
