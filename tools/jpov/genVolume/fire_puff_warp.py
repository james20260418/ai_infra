#!/usr/bin/env python3
"""fire_puff_warp.py — 把一张**静态火团贴图**做成 32 帧无缝循环 flipbook（形变法）。

思路：手工/物理生成「透明斑纹」很难，但用户已经给了目标图。这里直接把目标图当素材，
用**周期性 curl 扰动 + 轻微上升漂移 + 透明度呼吸**做成动画，保持它本身的透明斑纹质感。

  * 扰动场全部是 sin/cos(2πt + …) → 第 32 帧与第 0 帧完全一致（无缝循环）。
  * 越靠上（火舌端）扰动越大，靠下（根部）几乎不动 → 不会“整张图在抖”。
  * alpha 也做轻微呼吸，避免看起来像“抖动的贴纸”。

用法：
  python3 fire_puff_warp.py --src target.png --out /tmp/warp.png \
      [--cell 256] [--frames 32] [--size 512] [--amp 0.10] [--rise 0.05]
"""

import argparse

import numpy as np
from PIL import Image

from make_flipbook import write_png


def bilinear(img, x, y):
    """img: (H,W,C) float；x/y: (H,W) 像素坐标（可越界，边缘 clamp）。"""
    H, W = img.shape[0], img.shape[1]
    x = np.clip(x, 0.0, W - 1.001)
    y = np.clip(y, 0.0, H - 1.001)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = x0 + 1
    y1 = y0 + 1
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    c00 = img[y0, x0]
    c10 = img[y0, x1]
    c01 = img[y1, x0]
    c11 = img[y1, x1]
    return (c00 * (1 - fx) + c10 * fx) * (1 - fy) + (c01 * (1 - fx) + c11 * fx) * fy


def render_frame(src, t, amp, rise, alpha_breath):
    H, W = src.shape[0], src.shape[1]
    yy = (np.arange(H, dtype=np.float32) + 0.5)[:, None]
    xx = (np.arange(W, dtype=np.float32) + 0.5)[None, :]
    u = xx / W
    v = yy / H
    tau = 2.0 * np.pi * t

    # 越靠上扰动越大（v=0 是顶部）
    strength = np.clip(1.0 - v, 0.0, 1.0) ** 1.6

    # 周期性扰动（水平 + 垂直），两个不同频率叠加 → 不规则但可循环
    du = (np.sin(v * 6.0 + tau + 1.7 * np.sin(u * 5.0)) * 0.65 +
          np.sin(v * 11.0 - tau * 2.0 + u * 3.0) * 0.35)
    dv = (np.cos(u * 7.0 - tau * 1.0) * 0.55 +
          np.sin(u * 12.0 + tau * 2.0 + v * 4.0) * 0.45)

    # 上升漂移：用 sin 而不是线性 scroll，保证循环；配合形变看起来像在升腾
    base_y = v + rise * (0.5 + 0.5 * np.sin(tau)) * strength
    x = (u + amp * du * strength) * W
    y = (base_y - 0.5 * amp * dv * strength) * H

    out = bilinear(src, x, y)
    # 透明度呼吸（10% 幅度），避免整张图“硬贴”
    breath = 1.0 - alpha_breath * (0.5 + 0.5 * np.sin(tau + np.sin(v * 4.0) * 1.1))
    out[..., 3] *= breath
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--src", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--size", type=int, default=512)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--amp", type=float, default=0.10)
    p.add_argument("--rise", type=float, default=0.05)
    p.add_argument("--alpha-breath", type=float, default=0.18)
    args = p.parse_args()

    im = Image.open(args.src).convert("RGBA")
    # 等比缩放进 size×size（保持居中，透明边）
    im.thumbnail((args.size, args.size), Image.LANCZOS)
    canvas = Image.new("RGBA", (args.size, args.size), (0, 0, 0, 0))
    canvas.alpha_composite(im, ((args.size - im.width) // 2,
                                (args.size - im.height) // 2))
    src = np.array(canvas, dtype=np.float32) / 255.0

    tiles, cell = args.tiles, args.cell
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for f in range(args.frames):
        t = f / float(args.frames)
        img = render_frame(src, t, args.amp, args.rise, args.alpha_breath)
        k = args.size // cell
        small = img.reshape(cell, k, cell, k, 4).mean(axis=(1, 3))
        r, c = f // tiles, f % tiles
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = small

    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
