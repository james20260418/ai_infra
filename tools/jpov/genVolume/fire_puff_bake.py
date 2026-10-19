#!/usr/bin/env python3
"""fire_puff_bake.py — 把 Astralax 那套「手绘火舌贴图 + 粒子逻辑」烘成**单团 puff** 的 flipbook。

两种模式：
  single  —— 一个粒子（一张火舌贴图）的**整个生命周期**：
             尺寸先大后小、轻微自旋、颜色 白黄→橙→暗红→黑（末端黑=加法淡出）。
             配合引擎 `SheetTimeMode::kLifetime`（按粒子寿命取帧）。
  cluster —— 把 N 张火舌**拼成一个中等团**（错开相位/旋转/缩放），做成**无缝循环**：
             适合当“多一点内容”的 puff（我们引擎里叠几十张成火堆）。

输出：8×4 = 32 帧，256px/帧，黑底（加法语义）。

用法：
  python3 fire_puff_bake.py --mode single  --tex /tmp/inflames/zip/Textures/Flame_3.jpg --out /tmp/p1.png
  python3 fire_puff_bake.py --mode cluster --tex-dir /tmp/inflames/zip/Textures --tongues 3 --out /tmp/c3.png
"""

import argparse
import glob
import os

import numpy as np
from PIL import Image

from make_flipbook import write_png


def load_tex(path, size):
    im = Image.open(path).convert("RGB").resize((size, size), Image.LANCZOS)
    return np.array(im, dtype=np.float32) / 255.0


def splat(rgb, cov, tex, cx, cy, size, rot, color, gain=0.35, cov_gain=0.55):
    H, W = rgb.shape[0], rgb.shape[1]
    r = int(size * 0.72) + 2
    x0, x1 = max(0, int(cx - r)), min(W, int(cx + r) + 1)
    y0, y1 = max(0, int(cy - r)), min(H, int(cy + r) + 1)
    if x1 <= x0 or y1 <= y0:
        return
    yy = (np.arange(y0, y1, dtype=np.float32) + 0.5)[:, None]
    xx = (np.arange(x0, x1, dtype=np.float32) + 0.5)[None, :]
    dx = (xx - cx) / size
    dy = (yy - cy) / size
    c, s = np.cos(rot), np.sin(rot)
    u = (dx * c + dy * s + 0.5) * tex.shape[1]
    v = (-dx * s + dy * c + 0.5) * tex.shape[0]
    u = np.clip(u, 0.0, tex.shape[1] - 1.001)
    v = np.clip(v, 0.0, tex.shape[0] - 1.001)
    u0 = np.floor(u).astype(np.int32)
    v0 = np.floor(v).astype(np.int32)
    fu = (u - u0)[..., None]
    fv = (v - v0)[..., None]
    p00 = tex[v0, u0]
    p10 = tex[v0, u0 + 1]
    p01 = tex[v0 + 1, u0]
    p11 = tex[v0 + 1, u0 + 1]
    patch = (p00 * (1 - fu) + p10 * fu) * (1 - fv) + (p01 * (1 - fu) + p11 * fu) * fv
    rgb[y0:y1, x0:x1, :] += patch * gain * color[None, None, :]
    cov[y0:y1, x0:x1] += patch.max(axis=2) * cov_gain


def life_color(t):
    """文章里的粒子**颜色曲线**（Intense/加法下作用）。

    重要：Astralax 附带的 5 张火舌贴图是**灰度**的（实测 RGB 三通道几乎相等 ≈0.52–0.57），
    颜色完全来自这条曲线：出生白黄 → 橙 → 暗红 → 末端黑（黑=加法下消失）。
    """
    st = np.array([0.0, 0.12, 0.40, 0.72, 1.0], np.float32)
    r = np.interp(t, st, [1.00, 1.00, 1.00, 0.62, 0.00])
    g = np.interp(t, st, [0.97, 0.86, 0.44, 0.10, 0.00])
    b = np.interp(t, st, [0.82, 0.55, 0.10, 0.01, 0.00])
    return np.stack([r, g, b]).astype(np.float32)


def render_single(tex, t, S, args):
    rgb = np.zeros((S, S, 3), np.float32)
    cov = np.zeros((S, S), np.float32)
    # 尺寸：出生略小 → 中段最大 → 末端收缩（与 MP 的 size over life 一致）
    grow = np.sin(np.pi * min(1.0, t / 0.45) * 0.5) ** 0.6 if t < 0.45 else 1.0
    shrink = (1.0 - 0.45 * max(0.0, (t - 0.55) / 0.45))
    size = args.size0 * (0.55 + 0.45 * grow) * shrink
    rot = args.spin * t
    col = life_color(t)
    splat(rgb, cov, tex, S * 0.5, S * 0.56, size, rot, col,
          gain=args.gain, cov_gain=args.cov_gain)
    return rgb, cov


def render_cluster(texs, t, S, args):
    rgb = np.zeros((S, S, 3), np.float32)
    cov = np.zeros((S, S), np.float32)
    n = len(texs)
    for k, tex in enumerate(texs):
        ph = k / n
        tt = (t + ph) % 1.0
        # 每根火舌：位置绕中心分布、尺寸/旋转各不相同
        ang = 2.0 * np.pi * k / n + args.spread_rot
        rr = args.spread * S * (0.35 + 0.65 * abs(np.sin(np.pi * (0.15 + 0.7 * tt))))
        cx = S * 0.5 + rr * np.cos(ang)
        cy = S * (0.92 - args.rise * tt) + 0.04 * S * np.sin(2 * np.pi * tt + k)
        size = args.size0 * (0.75 + 0.35 * np.sin(np.pi * tt)) * (1.0 - 0.25 * tt)
        rot = args.spin * tt + k * 1.7
        col = life_color((tt + 0.15) % 1.0)
        splat(rgb, cov, tex, cx, cy, size, rot, col,
              gain=args.gain, cov_gain=args.cov_gain)
    return rgb, cov


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--mode", choices=["single", "cluster"], default="single")
    p.add_argument("--tex", default="/tmp/inflames/zip/Textures/Flame_3.jpg")
    p.add_argument("--tex-dir", default="/tmp/inflames/zip/Textures")
    p.add_argument("--tex-pick", default="")     # cluster: 逗号分隔的序号，如 "1,3,5"
    p.add_argument("--tongues", type=int, default=3)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--size", type=int, default=512)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--size0", type=float, default=460.0)
    p.add_argument("--spin", type=float, default=0.5)
    p.add_argument("--spread", type=float, default=0.22)
    p.add_argument("--spread-rot", type=float, default=0.6)
    p.add_argument("--rise", type=float, default=0.18)
    p.add_argument("--gain", type=float, default=0.85)
    p.add_argument("--cov-gain", type=float, default=0.55)
    p.add_argument("--cov-k", type=float, default=1.6)
    args = p.parse_args()

    S = args.size
    if args.mode == "single":
        texs = [load_tex(args.tex, S)]
    else:
        files = sorted(glob.glob(os.path.join(args.tex_dir, "*.jpg")))
        if args.tex_pick:
            idx = [int(x) - 1 for x in args.tex_pick.split(",")]
            files = [files[i] for i in idx]
        else:
            files = files[: args.tongues]
        texs = [load_tex(f, S) for f in files]
    print("textures:", len(texs))

    tiles, cell = args.tiles, args.cell
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for f in range(args.frames):
        t = f / float(args.frames)
        if args.mode == "single":
            rgb, cov = render_single(texs[0], t, S, args)
        else:
            rgb, cov = render_cluster(texs, t, S, args)
        alpha = 1.0 - np.exp(-cov * args.cov_k)
        img = np.zeros((S, S, 4), np.float32)
        img[..., :3] = np.clip(rgb, 0.0, 1.0)
        img[..., 3] = np.clip(alpha, 0.0, 1.0)
        if S % cell == 0:
            k = S // cell
            small = img.reshape(cell, k, cell, k, 4).mean(axis=(1, 3))
        else:
            small = np.asarray(
                Image.fromarray((np.clip(img, 0.0, 1.0) * 255).astype(np.uint8), "RGBA")
                .resize((cell, cell), Image.LANCZOS), dtype=np.float32) / 255.0
        r, c = f // tiles, f % tiles
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = small

    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
