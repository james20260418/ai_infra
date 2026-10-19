#!/usr/bin/env python3
"""make_pairs_v33_v34.py — V3.3 / V3.4：F5 换成 danis_F5_v3.png 的候选 #2。

- V3.3：不加旋转平均；
- V3.4：对**中间结果 S1..S5** 做旋转平均（limited ±15°）。

danis_F5_v3.png 背景是深灰径向渐变（不是纯黑）→ 先按边框拟合二次曲面减背景，
再把辉光压成 `clip((res-0.4)/0.5)`，边缘羽化到 0。

用法：
  python3 make_pairs_v33_v34.py
"""

import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morph_util as M  # noqa: E402
import rot_avg as R     # noqa: E402
from make_pairs_v31 import load_five, save_mask, CHAINS, CELL, SEG_N, ASSETS  # noqa: E402

SRC = "/james_pm/danis_F5_v3.png"
BLOCK_N = SEG_N * 2
CAND = 2          # 1..5（上→下）；#2 最像向上火舌


def key_background(path, cand=CAND, lo=0.40, hi=0.90, feather=30):
    a = np.array(Image.open(path).convert("RGB"), dtype=np.float32)
    h, w, _ = a.shape
    L = a.max(axis=2) / 255.0
    ys, xs = np.mgrid[0:h, 0:w]
    bw = 60
    border = np.zeros((h, w), bool)
    border[:bw, :] = border[-bw:, :] = True
    border[:, :bw] = border[:, -bw:] = True
    X, Y = xs[border] / w, ys[border] / h
    A = np.stack([np.ones_like(X), X, Y, X * X, Y * Y, X * Y], axis=1)
    coef, *_ = np.linalg.lstsq(A, L[border], rcond=None)
    xn, yn = xs / w, ys / h
    bg = (coef[0] + coef[1] * xn + coef[2] * yn + coef[3] * xn ** 2 +
          coef[4] * yn ** 2 + coef[5] * xn * yn)
    res = np.clip((L - bg) / (1.0 - bg + 1e-6), 0, 1)
    res[2200:, 1400:] = 0                       # 右下角水印

    # 边缘羽化到 0（保证纹理四周是全黑/全透明）
    ramp = np.linspace(0, 1, feather)
    feather_map = np.ones((h, w), np.float32)
    feather_map[:feather, :] *= ramp[:, None]
    feather_map[-feather:, :] *= ramp[::-1, None]
    feather_map[:, :feather] *= ramp[None, :]
    feather_map[:, -feather:] *= ramp[::-1][None, :]
    res *= feather_map

    m = (res > 0.30).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    idx = [i for i in range(1, n) if stats[i, 4] > 5000]
    idx.sort(key=lambda i: stats[i, 1])
    assert len(idx) >= cand, f"候选不足：{len(idx)}"
    i = idx[cand - 1]
    x0, y0, bw_, bh_ = stats[i, 0], stats[i, 1], stats[i, 2], stats[i, 3]
    mask = cv2.dilate((lab == i).astype(np.uint8),
                      cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (81, 81)))
    own = res * mask
    own = np.clip((own - lo) / (hi - lo), 0, 1)
    side = int(max(bw_, bh_) + 40)
    x1 = int(x0 - (side - bw_) / 2)
    y1 = int(y0 - (side - bh_) / 2)
    crop = np.zeros((side, side), np.float32)
    sx0, sy0 = max(0, x1), max(0, y1)
    sx1, sy1 = min(w, x1 + side), min(h, y1 + side)
    crop[sy0 - y1:sy0 - y1 + (sy1 - sy0), sx0 - x1:sx0 - x1 + (sx1 - sx0)] = \
        own[sy0:sy1, sx0:sx1]
    return cv2.resize(crop, (CELL, CELL), interpolation=cv2.INTER_AREA)


def build(S, tag, F=None):
    if F:
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
    out = os.path.join(ASSETS, f"flames_pairs_{tag}_12x6.png")
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


def main():
    base = load_five("/james_pm/danis_F5_v2.png")   # F1..F4 用之前的手绘简单版
    F = {i: base[i - 1] for i in range(1, 5)}
    F[5] = key_background(SRC)
    save_mask(F[5], os.path.join(ASSETS, "v33_f5.png"))

    order = [1, 2, 3, 4, 5, 1]
    S = {k + 1: M.morph_profile(F[order[k]], F[order[k + 1]], 0.5)
         for k in range(5)}

    # V3.3：不旋转平均
    build(dict(S), "v33", F)

    # V3.4：对 S 做旋转平均
    S4 = {k: R.avg_limited(S[k], 15.0, 8.0) for k in S}
    build(S4, "v34")


if __name__ == "__main__":
    main()
