#!/usr/bin/env python3
"""flame_curve_gen.py — 用**贝塞尔曲线 + 宽度剖面**构造火舌，并做形状补间（shape tween）。

动机（Danis）：Flash 的 Shape Tween 是「提轮廓 → 重采样 → 顶点配对 → 插值」；
与其混合两张位图，不如把火舌本身用曲线参数化，**插值控制点**就能得到干净的形变。

文章里**没有**写怎么从贝塞尔曲线生成贴图（原文是手绘 + 涂抹 + 高斯模糊），
所以这套参数化是我们自己设计的近似：
  * 中心线：三次贝塞尔（4 个控制点：起点/两个手柄/终点）
  * 宽度：沿弧长 u 的剖面  w(u) = w0 * (1-u)^taper * (1 + bulge*sin(pi*u))
  * 边缘：高频噪声轻微扰动（让轮廓不像数学曲线）

用法：
  python3 flame_curve_gen.py --morph strip --out /tmp/curve_morph.png
  python3 flame_curve_gen.py --shape curl --out /tmp/curve_single.png
"""

import argparse
import math

import numpy as np
from PIL import Image

from make_flipbook import write_png


# ---------------- 形状参数 ----------------

# 每个"火舌形态"= 一组参数；插值这些参数就是形状补间。
SHAPES = {
    # 细长、向右上方弯（类 F1）
    "curl": dict(h=0.86, bend=0.20, tip=0.16, w0=0.055, taper=0.85, bulge=0.25, wig=0.06),
    # 宽、双峰（类 F3）
    "wide": dict(h=0.66, bend=0.04, tip=0.02, w0=0.115, taper=0.55, bulge=0.45, wig=0.03),
    # 月牙/逗号（类 F5）
    "crescent": dict(h=0.80, bend=0.30, tip=-0.14, w0=0.075, taper=1.15, bulge=0.10, wig=0.09),
}


def bezier(p0, p1, p2, p3, n):
    t = np.linspace(0.0, 1.0, n, dtype=np.float32)[:, None]
    a = (1 - t) ** 3
    b = 3 * (1 - t) ** 2 * t
    c = 3 * (1 - t) * t ** 2
    d = t ** 3
    return a * p0 + b * p1 + c * p2 + d * p3


def render(shape, S=256, seed=0):
    """把一个形态参数渲成 alpha 掩膜（float [0,1]）。"""
    p = shape
    H = p["h"] * S
    x0 = S * 0.5
    y0 = S * 0.92
    # 控制点：起点 → 中部手柄 → 末端
    c0 = np.array([x0, y0], np.float32)
    c1 = np.array([x0 + p["bend"] * S * 0.25, y0 - H * 0.38], np.float32)
    c2 = np.array([x0 + p["bend"] * S * 0.75, y0 - H * 0.72], np.float32)
    c3 = np.array([x0 + p["tip"] * S, y0 - H], np.float32)

    n = 160
    pts = bezier(c0, c1, c2, c3, n)          # (n,2)
    u = np.linspace(0.0, 1.0, n, dtype=np.float32)
    # 宽度剖面
    w = p["w0"] * S * (1.0 - u) ** p["taper"] * (1.0 + p["bulge"] * np.sin(np.pi * u))
    # 轻微的"蛇形"侧摆
    xoff = p["wig"] * S * np.sin(u * 5.2 + 0.7) * u
    pts = pts.copy()
    pts[:, 0] += xoff

    # 距离场：对每个采样点叠加高斯核
    yy = (np.arange(S, dtype=np.float32) + 0.5)[:, None]
    xx = (np.arange(S, dtype=np.float32) + 0.5)[None, :]
    field = np.zeros((S, S), np.float32)
    for k in range(n):
        r = max(w[k], 0.8)
        d2 = ((xx - pts[k, 0]) ** 2 + (yy - pts[k, 1]) ** 2) / (r * r)
        field = np.maximum(field, np.exp(-d2 * 2.0))

    # 边缘噪声（让轮廓有机一点）
    rng = np.random.default_rng(seed)
    noise = rng.random((8, 8)).astype(np.float32)
    noise = np.array(Image.fromarray((noise * 255).astype(np.uint8)).resize((S, S), Image.BICUBIC),
                     dtype=np.float32) / 255.0
    field = np.clip(field * (0.85 + 0.30 * noise), 0.0, 1.0)
    return field


def lerp_shape(a, b, t):
    return {k: a[k] + (b[k] - a[k]) * t for k in a}


def to_rgba(alpha, color=(1.0, 1.0, 1.0)):
    img = np.ones((alpha.shape[0], alpha.shape[1], 4), np.float32)
    img[..., 0] *= color[0]
    img[..., 1] *= color[1]
    img[..., 2] *= color[2]
    img[..., 3] = alpha
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--morph", default="", help="a:b 例如 curl:wide；给了就出补间序列")
    ap.add_argument("--shape", default="curl")
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--cell", type=int, default=256)
    ap.add_argument("--tiles", type=int, default=8)
    args = ap.parse_args()

    if args.morph:
        a_name, b_name = args.morph.split(":")
        a, b = SHAPES[a_name], SHAPES[b_name]
        tiles, cell = args.tiles, args.cell
        sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
        for f in range(args.frames):
            t = f / float(args.frames)
            img = to_rgba(render(lerp_shape(a, b, t), S=cell, seed=f))
            r, c = f // tiles, f % tiles
            sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = img
        rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
        write_png(args.out, rgba)
        print("wrote sheet", args.out, rgba.shape)
    else:
        alpha = render(SHAPES[args.shape], S=args.cell, seed=0)
        rgba = (np.clip(to_rgba(alpha), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
        write_png(args.out, rgba)
        print("wrote single", args.out, rgba.shape)


if __name__ == "__main__":
    main()
