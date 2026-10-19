#!/usr/bin/env python3
"""flame_morph_gen.py — 控制点插值：F1 → F5 的形状补间（一张 32 帧 sheet）。

做法（Danis）：不用位图交叉淡化，而是把拟合出来的**贝塞尔控制点/宽度参数**
线性插值；每一帧都是重新渲染的几何中间态，所以没有重影。

单团周期的语义：t=0 是 F1，t=1 是 F5，中间是平滑形变。
viewer 里对应 FireTexPlay::kLifetime（一生走完整张 sheet 一次）。

用法：
  python3 flame_morph_gen.py --fit /tmp/curve_fit/fitted.json --a F1 --b F5 \
      --out tools/jpov/assets/fire/mp/curve_morph_f1f5_8x4.png --preview /tmp/morph_preview.png
"""

import argparse
import json
import math
import os

import numpy as np
from PIL import Image

from flame_cluster_gen import (CELL, colorize, render_tongue, smoothstep,
                              _noise_grid, sample_noise)

TILES_X, TILES_Y = 8, 4
FRAMES = TILES_X * TILES_Y

# 被插值的参数；phase 是角度，单独处理
PKEYS = ["h", "x1", "x2", "x3", "w0", "taper", "bulge", "wig",
         "sharp", "bt", "bx", "by"]


def lerp_params(a, b, t):
    p = {k: a[k] + (b[k] - a[k]) * t for k in PKEYS}
    # 相位差取最短弧，避免多转半圈
    d = (b["phase"] - a["phase"] + math.pi) % (2 * math.pi) - math.pi
    p["phase"] = a["phase"] + d * t
    return p


def frame_image(pa, pb, t, S):
    p = lerp_params(pa, pb, t)
    # 2× 超采样再盒式降采样：轮廓更锐，不产生「糊边/重影」观感
    SS = 2
    S2 = S * SS
    field = render_tongue(p, S2, scale=1.0, rot_deg=0.0,
                          base=(p["bx"], p["by"]), width_scale=1.0, n=128)

    yy, xx = np.mgrid[0:S2, 0:S2].astype(np.float32)
    # 白热核：贴在基点上方一点点
    core = np.exp(-(((xx - p["bx"] * S2) ** 2 + (yy - (p["by"] - 0.06) * S2) ** 2)
                    / (2.0 * (0.045 * S2) ** 2)))
    # 内部亮度扰动（只改亮度，不改轮廓 → 形变仍然干净）
    nz = _noise_grid(8, 8, 16, 991)
    fine = sample_noise(nz, xx / S2, yy / S2, t)
    inten = field * (0.78 + 0.30 * fine)
    inten = np.clip(inten + core * 0.85, 0.0, 1.0)

    rgb = colorize(inten ** 1.05)
    alpha = np.clip(smoothstep(0.16, 0.60, field) * 0.98, 0.0, 1.0)
    alpha = np.maximum(alpha, core * 0.80)

    # 盒式 2×2 降采样
    rgb = rgb.reshape(S, SS, S, SS, 3).mean(axis=(1, 3))
    alpha = alpha.reshape(S, SS, S, SS).mean(axis=(1, 3))
    return np.dstack([rgb, alpha])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", default="/tmp/curve_fit/fitted.json")
    ap.add_argument("--a", default="F1")
    ap.add_argument("--b", default="F5")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", default="")
    ap.add_argument("--cell", type=int, default=CELL)
    args = ap.parse_args()

    with open(args.fit) as f:
        fit = json.load(f)
    pa = fit[args.a]["params"]
    pb = fit[args.b]["params"]

    S = args.cell
    sheet = np.zeros((TILES_Y * S, TILES_X * S, 4), np.float32)
    for i in range(FRAMES):
        t = i / float(FRAMES - 1)          # 首帧 = A，末帧 = B
        img = frame_image(pa, pb, t, S)
        r, c = i // TILES_X, i % TILES_X
        rr = TILES_Y - 1 - r   # 行翻转：viewer 的 frame 0 采样 PNG 末行
        sheet[rr * S:(rr + 1) * S, c * S:(c + 1) * S] = img

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    rgba = (np.clip(sheet, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(rgba).save(args.out)
    print("wrote", args.out, rgba.shape,
          "alpha mean %.4f cov %.3f" % (float(sheet[..., 3].mean()),
                                        float((sheet[..., 3] > 0.1).mean())))

    if args.preview:
        idxs = [0, 10, 21, FRAMES - 1]
        ims = []
        for i in idxs:
            r, c = i // TILES_X, i % TILES_X
            rr = TILES_Y - 1 - r
            im = sheet[rr * S:(rr + 1) * S, c * S:(c + 1) * S]
            ims.append(im[..., :3] * im[..., 3:4])
        gap = 8
        W = len(ims) * S + (len(ims) - 1) * gap
        out = np.zeros((S, W, 3), np.float32)
        for k, im in enumerate(ims):
            out[:, k * (S + gap):k * (S + gap) + S] = im
        Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(args.preview)
        print("wrote", args.preview)


if __name__ == "__main__":
    main()
