#!/usr/bin/env python3
"""fire_puff_sim.py — 程序化「小火团」体积 + flipbook 渲染（不依赖 Blender/Mantaflow）。

背景：本机 Blender 4.0.2 的 Mantaflow bake 会崩（bake_all → core dump），
docker 也不可用，所以先用**程序化体积**逼近目标火团：多簇火舌 + 白热核心 +
橙中段 + 暗红边 + 镂空 + 软边，且**32 帧无缝循环**（不点火/不熄灭，靠引擎端色带淡出）。

体积：6 cm 立方（目标火团按生活经验约 5 cm），格点默认 96³。
渲染：沿 +Z 前到后的 emission-absorption（与 make_flipbook.py 同一套公式/黑体色），
      输出 RGBA（RGB=发射色，A=1-透射率），8×4 = 32 帧。

用法：
  python3 fire_puff_sim.py --out /tmp/puff.png [--tiles 8] [--cell 256] \
      [--grid 96] [--frames 32] [--tongues 5] [--sigma 40] [--gain 3.2] \
      [--speed 1.0] [--turb 1.0] [--temp-bias 1.0]
"""

import argparse

import numpy as np

from make_flipbook import blackbody, box_blur


# ---------------- 可循环 3D value noise ----------------

def _lattice(seed, n):
    rng = np.random.default_rng(seed)
    return rng.random((n, n, n), dtype=np.float32)


def sample_noise(lat, x, y, z):
    """三线性采样，lattice 在三个方向都环绕（x/y/z 为 lattice 单位坐标）。"""
    n = lat.shape[0]
    xf = np.floor(x)
    yf = np.floor(y)
    zf = np.floor(z)
    fx = (x - xf)[None, None, :]
    fy = (y - yf)[None, :, None]
    fz = (z - zf)[:, None, None]
    x0 = (xf.astype(np.int64) % n)
    y0 = (yf.astype(np.int64) % n)
    z0 = (zf.astype(np.int64) % n)
    x1 = (x0 + 1) % n
    y1 = (y0 + 1) % n
    z1 = (z0 + 1) % n

    def g(zi, yi, xi):
        return lat[np.ix_(zi, yi, xi)]

    c000 = g(z0, y0, x0)
    c100 = g(z0, y0, x1)
    c010 = g(z0, y1, x0)
    c110 = g(z0, y1, x1)
    c001 = g(z1, y0, x0)
    c101 = g(z1, y0, x1)
    c011 = g(z1, y1, x0)
    c111 = g(z1, y1, x1)

    c00 = c000 * (1 - fx) + c100 * fx
    c10 = c010 * (1 - fx) + c110 * fx
    c01 = c001 * (1 - fx) + c101 * fx
    c11 = c011 * (1 - fx) + c111 * fx
    c0 = c00 * (1 - fy) + c10 * fy
    c1 = c01 * (1 - fy) + c11 * fy
    return (c0 * (1 - fz) + c1 * fz).astype(np.float32)


def fbm(lat, x, y, z, octaves=4):
    acc = np.zeros((len(z), len(y), len(x)), np.float32)
    amp = 0.5
    norm = 0.0
    fx, fy, fz = x, y, z
    for o in range(octaves):
        acc += amp * sample_noise(lat, fx, fy, fz)
        norm += amp
        amp *= 0.5
        fx = fx * 2.0 + 13.0
        fy = fy * 2.0 + 7.0
        fz = fz * 2.0 + 3.0
    return acc / norm


# ---------------- 体积生成 ----------------

def build_volume(args):
    n = args.grid
    F = args.frames
    g = (np.arange(n, dtype=np.float32) + 0.5) / n
    zz = g[:, None, None]          # 高度 0..1（底 → 顶）
    yy = g[None, :, None]
    xx = g[None, None, :]

    lat_lo = _lattice(101, 16)     # 低频：火舌粗细/摆动
    lat_hi = _lattice(202, 24)     # 高频：镂空/纹理

    dens = np.zeros((F, n, n, n), np.float32)
    temp = np.zeros_like(dens)

    rng = np.random.default_rng(7)
    tongues = []
    for k in range(args.tongues):
        a = 2.0 * np.pi * k / args.tongues + 0.4
        r = 0.20 if args.tongues > 1 else 0.0
        tongues.append({
            "x": 0.5 + r * np.cos(a),
            "y": 0.5 + r * np.sin(a),
            "h": 0.50 + 0.38 * rng.random(),
            "r0": 0.055 + 0.02 * rng.random(),
            "ph": 6.28 * rng.random(),
            "sw": 0.10 + 0.06 * rng.random(),
        })

    for f in range(F):
        t = f / float(F)                       # 0..1，周期（无缝循环）
        gz1 = ((g - args.speed * t) % 1.0) * 5.0
        gy1 = g * 5.0
        gx1 = g * 5.0
        n_lo = fbm(lat_lo, gx1, gy1, gz1, octaves=3)      # (nz, ny, nx)
        n_hi = fbm(lat_hi, gx1 * 2.2, gy1 * 2.2, gz1 * 2.2, octaves=3)

        field = np.zeros((n, n, n), np.float32)
        for tg in tongues:
            cz = np.clip(zz / max(tg["h"], 1e-3), 0.0, 1.0)
            # 越往上摆动越大（火舌尖端飘）
            sway_x = tg["sw"] * np.sin(zz * 8.0 + tg["ph"] + 2.0 * np.pi * t) * cz
            sway_y = tg["sw"] * np.cos(zz * 6.5 + tg["ph"] * 1.7 + 2.0 * np.pi * t) * cz
            rad = tg["r0"] * np.clip(1.0 - cz, 0.0, 1.0) ** 0.45
            rad = rad * (0.75 + 0.55 * n_lo)               # 粗细随低频噪声起伏
            rr = np.sqrt((xx - (tg["x"] + sway_x)) ** 2 + (yy - (tg["y"] + sway_y)) ** 2)
            d = np.exp(-(rr / np.maximum(rad, 1e-4)) ** 2)
            d *= np.clip(1.0 - cz, 0.0, 1.0) ** 0.55
            field = np.maximum(field, d)                   # max：保持火舌分离

        # 镂空：用高频噪声挖缝（阈值以上的才留下）
        hole = np.clip((n_hi - (0.34 - 0.16 * args.holes)) / 0.42, 0.0, 1.0)
        field = field * (0.35 + 1.25 * hole)
        field = np.clip((field - 0.10) / 0.55, 0.0, 1.0) ** 1.25

        dens[f] = field
        # 温度：密度高的地方最热（白核），往外/往上迅速变冷（暗红边）
        temp[f] = np.clip(field ** 0.75 * (1.0 - 0.62 * zz) * args.temp_bias, 0.0, 1.0)

    return dens, temp


# ---------------- 渲染（无生命包络；循环） ----------------

def render(dens, temp, args):
    F, nz, ny, nx = dens.shape
    ds = 1.0 / float(nz)
    frames = []
    for f in range(F):
        d = dens[f]
        tp = temp[f]
        T = np.ones((ny, nx), np.float32)
        L = np.zeros((ny, nx, 3), np.float32)
        for z in range(nz):
            dz = d[z]
            a = 1.0 - np.exp(-dz * args.sigma * ds)
            col = blackbody(np.clip(tp[z] * 1.2, 0.0, 1.0))
            L += T[..., None] * a[..., None] * col * args.gain
            T = T * (1.0 - a)
        alpha = np.clip(1.0 - T, 0.0, 1.0) * args.alpha
        img = np.zeros((ny, nx, 4), np.float32)
        img[..., :3] = np.clip(L, 0.0, 1.0)
        img[..., 3] = np.clip(alpha, 0.0, 1.0)
        img = box_blur(img, 1, 1)
        frames.append(img[::-1, :, :])   # 行 0 = 顶部

    # 全序列统一 bbox 裁剪 + 居中
    any_a = np.zeros((ny, nx), np.float32)
    for img in frames:
        any_a = np.maximum(any_a, img[..., 3])
    ys, xs = np.nonzero(any_a > 0.02)
    if len(ys) == 0:
        ys, xs = np.arange(ny), np.arange(nx)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    side = max(y1 - y0, x1 - x0)
    cy = (y0 + y1) // 2
    cx = (x0 + x1) // 2
    y0 = max(0, cy - side // 2)
    x0 = max(0, cx - side // 2)

    cell = args.cell
    tiles = args.tiles
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for i, img in enumerate(frames):
        crop = img[y0:y0 + side, x0:x0 + side, :]
        crop = np.clip(crop, 0.0, 1.0)
        # 等比缩放到 cell（最近邻 + 轻微模糊由前面 box_blur 负责）
        idx_y = (np.linspace(0, side - 1, cell)).astype(np.int32)
        idx_x = (np.linspace(0, side - 1, cell)).astype(np.int32)
        big = crop[np.ix_(idx_y, idx_x)]
        row, col = i // tiles, i % tiles
        sheet[row * cell:(row + 1) * cell, col * cell:(col + 1) * cell, :] = big
    return (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--grid", type=int, default=96)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--tongues", type=int, default=5)
    p.add_argument("--sigma", type=float, default=22.0)
    p.add_argument("--gain", type=float, default=2.6)
    p.add_argument("--speed", type=float, default=1.0)
    p.add_argument("--turb", type=float, default=1.0)
    p.add_argument("--temp-bias", type=float, default=1.0)
    p.add_argument("--alpha", type=float, default=1.0)
    p.add_argument("--holes", type=float, default=1.0)
    args = p.parse_args()

    dens, temp = build_volume(args)
    rgba = render(dens, temp, args)
    from make_flipbook import write_png
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
