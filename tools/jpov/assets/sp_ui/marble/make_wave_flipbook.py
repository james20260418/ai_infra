#!/usr/bin/env python3
"""make_wave_flipbook.py — 用**单张周期性纹理** + 随机水波变形烘 flipbook。

用法：
    python3 make_wave_flipbook.py --src marble_sync_01.png --method sine \\
        --out plate_wave_sine_4x4.png --frames 16 --tiles 4 --cell 256

为什么换掉「关键帧交叉淡化」：那不是水在流，是两个不同花纹的溶解（Danis 2026-10-27
「动起来了但是确实很丑」）。改为**把同一张石块往复位移**——特征在屏幕上滑、起伏，
观感是水面波动；而且因为是同一个源，任何时刻都不会出现两张石纹互相残影。

三种生成方式（--method）：
  sine    多方向正弦叠加（可预期、最像规则水波）
  ripple  多点同心涟漪（像有水滴在石面上，中心处最活跃）
  noise   周期噪声域扭曲（无序流动，最像自然水面）

三个共同保证：
1. **空间可平铺**：位移场必须是周期场（正弦用整数波数；涟漪用环面距离；
   噪声用环绕整数格点）。位移场周期 = 纹理周期 ⇒ 变形后的图依然可平铺：
   warped(x+P) = tex(x+P+d(x+P)) = tex(x+d(x))。
2. **时间可回环**：所有相位都走整数圈（相位 = 2π·f·n/frames），
   第 frames 帧的位移场**逐像素等于**第 0 帧 ⇒ 循环无跳变。
3. 采样一律用环绕（wrap）双线性，边缘不会出现黑边或拉丝。
"""

import argparse
import sys

import numpy as np
from PIL import Image


def load_source(path, cell, gain, gain_target):
    """读入单张源纹理并缩到 cell×cell（float32 [0,1] RGB）。

    gain > 0 时先做与关键帧链同一套「围绕中位亮度放大」的对比度处理：
    生图的白大理石 50% 像素挤在 ±0.005 的白平台里，不放大就看不到纹路；
    目标亮度取 0.9 让「tint 就是板材亮度」这个直觉成立。
    """
    im = Image.open(path).convert("RGB")
    if im.size != (cell, cell):
        im = im.resize((cell, cell), Image.LANCZOS)
    img = np.asarray(im, dtype=np.float32) / 255.0
    if gain > 0.0:
        lum = 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]
        median = float(np.median(lum))
        img = np.clip(gain_target + (img - median) * gain, 0.0, 1.0)
    return img


def sample_wrap(img, x, y):
    """环绕双线性采样。x/y 是浮点像素坐标（可超出 [0,w) 任意外）。"""
    h, w = img.shape[0], img.shape[1]
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    x0i = x0.astype(np.int64) % w
    y0i = y0.astype(np.int64) % h
    x1i = (x0i + 1) % w
    y1i = (y0i + 1) % h
    c00 = img[y0i, x0i]
    c10 = img[y0i, x1i]
    c01 = img[y1i, x0i]
    c11 = img[y1i, x1i]
    return (c00 * (1.0 - fx) * (1.0 - fy) + c10 * fx * (1.0 - fy)
            + c01 * (1.0 - fx) * fy + c11 * fx * fy)


def torus_dist(dx, dy, period):
    """环面距离：把 |偏移| 折到 [0, period/2]，保证场在平铺边界上取值连续。"""
    ax = np.abs(dx)
    ay = np.abs(dy)
    return np.sqrt(np.minimum(ax, period - ax) ** 2
                   + np.minimum(ay, period - ay) ** 2)


def hash01(ix, iy, seed):
    """整数格点哈希 → [0,1)（向量化）。"""
    h = (ix * 374761393 + iy * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) & 0xFFFFFFFF
    h = (h * 1274126177) & 0xFFFFFFFF
    h = (h ^ (h >> 16)) & 0xFFFFFFFF
    return h.astype(np.float32) / 4294967296.0


def pnoise(x, y, period, seed):
    """周期性值噪声：整数格点按 period 环绕，五次平滑插值。x/y 为格点坐标。"""
    ix = np.floor(x)
    iy = np.floor(y)
    fx = x - ix
    fy = y - iy
    ux = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    uy = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0)
    ix = ix.astype(np.int64) % period
    iy = iy.astype(np.int64) % period
    jx = (ix + 1) % period
    jy = (iy + 1) % period
    a = hash01(ix, iy, seed)
    b = hash01(jx, iy, seed)
    c = hash01(ix, jy, seed)
    d = hash01(jx, jy, seed)
    return ((a * (1.0 - ux) + b * ux) * (1.0 - uy)
            + (c * (1.0 - ux) + d * ux) * uy)


def field_sine(xx, yy, cell, amp, wavelength, phase01):
    """多方向正弦叠加。波数取整数 ⇒ 场以 cell 为周期（可平铺）。"""
    disp_x = np.zeros_like(xx)
    disp_y = np.zeros_like(yy)
    # (方向角, 波数倍数, 权重, 时间相位圈数) —— 固定表，保证可复现。
    waves = [
        (0.00, 1.0, 1.00, 1),
        (0.90, 1.0, 0.75, 1),
        (2.10, 2.0, 0.55, 2),
        (-0.70, 2.0, 0.40, 1),
    ]
    base_cycles = max(1.0, round(cell / max(wavelength, 1e-6)))
    for k, (ang, mult, weight, turns) in enumerate(waves):
        cycles = base_cycles * mult
        kx = np.cos(ang) * 2.0 * np.pi * cycles / cell
        ky = np.sin(ang) * 2.0 * np.pi * cycles / cell
        ph = 2.0 * np.pi * (phase01 * turns + k * 0.37)
        s = np.sin(kx * xx + ky * yy + ph) * weight
        disp_x += s * np.cos(ang) * amp
        disp_y += s * np.sin(ang) * amp
    return disp_x, disp_y


def field_ripple(xx, yy, cell, amp, wavelength, phase01):
    """多点同心涟漪（环面距离 ⇒ 场周期）。相位走整数圈 ⇒ 时间可回环。"""
    disp_x = np.zeros_like(xx)
    disp_y = np.zeros_like(yy)
    centers = [(0.23, 0.31), (0.68, 0.22), (0.44, 0.74), (0.86, 0.63)]
    lam = max(wavelength, 1e-6)
    for k, (cx, cy) in enumerate(centers):
        px = cx * cell
        py = cy * cell
        dx = xx - px
        dy = yy - py
        r = torus_dist(dx, dy, float(cell))
        # 朝外的单位方向（环面折返后仍连续，只是导数有一个折点，视觉上是水波交汇）
        sx = np.sign(dx)
        sy = np.sign(dy)
        r_safe = np.maximum(r, 1e-6)
        ux = np.where(np.abs(dx) > cell * 0.5, sx, dx / r_safe)
        uy = np.where(np.abs(dy) > cell * 0.5, sy, dy / r_safe)
        s = np.sin(2.0 * np.pi * (r / lam) - 2.0 * np.pi * phase01 + k * 1.7)
        w = 0.55 + 0.45 * np.cos(k * 1.3)   # 每个中心的权重不同
        disp_x += s * ux * amp * w
        disp_y += s * uy * amp * w
    return disp_x, disp_y


def field_noise(xx, yy, cell, amp, wavelength, phase01):
    """周期噪声域扭曲：两个静态周期噪声场，按相位在它俩之间绕圈混合。

    d(f) = cos(2πf)·n1 + sin(2πf)·n2 —— 走满一圈回到起点（时间可回环），
    且过程中看起来像流场在推着石纹走。
    """
    scale = max(1.0, round(cell / max(wavelength, 1e-6)))
    period = int(max(2, scale))
    gx = xx / (cell / scale)
    gy = yy / (cell / scale)
    n1x = pnoise(gx, gy, period, 11) - 0.5
    n1y = pnoise(gx + 37.0, gy + 11.0, period, 12) - 0.5
    n2x = pnoise(gx, gy, period, 23) - 0.5
    n2y = pnoise(gx + 37.0, gy + 11.0, period, 24) - 0.5
    c = np.cos(2.0 * np.pi * phase01)
    s = np.sin(2.0 * np.pi * phase01)
    disp_x = (c * n1x + s * n2x) * amp * 2.0
    disp_y = (c * n1y + s * n2y) * amp * 2.0
    return disp_x, disp_y


FIELDS = {
    "sine": field_sine,
    "ripple": field_ripple,
    "noise": field_noise,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="单张**已无缝**的源纹理")
    ap.add_argument("--out", required=True, help="输出图集 PNG")
    ap.add_argument("--method", default="sine", choices=sorted(FIELDS),
                    help="水波生成方式（默认 sine）")
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--tiles", type=int, default=4)
    ap.add_argument("--cell", type=int, default=256, help="每格边长（默认 256）")
    ap.add_argument("--amp", type=float, default=10.0, help="位移幅度（像素）")
    ap.add_argument("--wavelength", type=float, default=96.0,
                    help="波长（像素，越大波越缓）")
    ap.add_argument("--contrast", type=float, default=8.0,
                    help="对比度增益（0 = 不处理；默认 8，同关键帧链）")
    ap.add_argument("--gain-target", type=float, default=0.90,
                    help="白平台落到的亮度（默认 0.90）")
    args = ap.parse_args()

    if args.tiles * args.tiles != args.frames:
        sys.exit(f"错误：frames({args.frames}) 必须等于 tiles²({args.tiles}²)")
    if args.amp < 0.0:
        sys.exit("错误：--amp 必须 >= 0")
    if args.wavelength <= 0.0:
        sys.exit("错误：--wavelength 必须 > 0")

    src = load_source(args.src, args.cell, args.contrast, args.gain_target)
    field = FIELDS[args.method]

    # 像素中心坐标网格（避免采样正好落在整数边界上）
    gy, gx = np.mgrid[0:args.cell, 0:args.cell].astype(np.float32)
    xx = gx + 0.5
    yy = gy + 0.5

    sheet = Image.new("RGB", (args.tiles * args.cell, args.tiles * args.cell))
    frames = []
    for f in range(args.frames):
        phase01 = float(f) / float(args.frames)   # 走满一圈 ⇒ 第 frames 帧回到第 0 帧
        dx, dy = field(xx, yy, float(args.cell), args.amp, args.wavelength, phase01)
        warped = sample_wrap(src, xx + dx, yy + dy)
        frames.append(warped)
        img = Image.fromarray(
            np.clip(warped * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8), "RGB")
        sheet.paste(img, ((f % args.tiles) * args.cell, (f // args.tiles) * args.cell))

    sheet.save(args.out)

    # 自检：帧间差 + 回环差（15→0 应落在同一区间，否则循环会跳）
    diffs = [float(np.abs(frames[(i + 1) % args.frames] - frames[i]).mean() * 255.0)
             for i in range(args.frames)]
    print(f"method={args.method} amp={args.amp} lambda={args.wavelength} "
          f"frames={args.frames} → {args.out}")
    print(f"  帧间平均差: min={min(diffs):.2f} max={max(diffs):.2f} "
          f"回环(末→首)={diffs[-1]:.2f}")


if __name__ == "__main__":
    main()
