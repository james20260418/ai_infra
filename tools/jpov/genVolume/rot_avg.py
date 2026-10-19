#!/usr/bin/env python3
"""rot_avg.py — 旋转平均 / 角向低通：让火舌在任意旋转角下都不「露馅」。

输入单通道 alpha（256²），输出同尺寸。
提供 4 种方案（外加原始对照）：

  full      : 全角度平均（0..360，每 10° 一次）→ 最各向同性，但会丢火焰方向感
  limited   : 有限角度加权平均（±30°，Gaussian σ=12°）→ 保留向上，磨掉尖角
  ang_lp    : 极坐标角向低通（沿 θ 做 circular Gaussian，σ=10°）→ 只削高频角向结构
  harmonic  : 角向 Fourier 只留 DC + 1 次谐波（可选 2 次）→ 保留主轴、去掉尖角

用法：
  python3 rot_avg.py --src <png> --out /tmp/rotavg
"""

import argparse
import os

import cv2
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter1d


def load_alpha(path):
    im = Image.open(path).convert("RGBA")
    a = np.array(im, dtype=np.float32)[..., 3] / 255.0
    if a.max() < 1e-3:
        a = np.array(im.convert("L"), dtype=np.float32) / 255.0
    return a


def rotate(L, deg):
    h, w = L.shape
    M = cv2.getRotationMatrix2D((w / 2 - 0.5, h / 2 - 0.5), deg, 1.0)
    return cv2.warpAffine(L, M, (w, h), flags=cv2.INTER_LINEAR,
                          borderMode=cv2.BORDER_CONSTANT, borderValue=0.0)


def norm(L):
    m = L.max()
    return np.clip(L / m, 0, 1) if m > 0 else L


def avg_full(L, step=10):
    acc = np.zeros_like(L)
    n = 0
    for d in range(0, 360, step):
        acc += rotate(L, d)
        n += 1
    return norm(acc / n)


def avg_limited(L, half=30.0, sigma=12.0, step=2.0):
    acc = np.zeros_like(L)
    wsum = 0.0
    d = -half
    while d <= half + 1e-6:
        w = np.exp(-0.5 * (d / sigma) ** 2)
        acc += w * rotate(L, d)
        wsum += w
        d += step
    return norm(acc / max(wsum, 1e-6))


def avg_weighted_all(L, sigma_deg=60.0, step=5.0):
    """全角度加权平均（向上保留一点偏置）：σ 越大越各向同性。"""
    acc = np.zeros_like(L)
    wsum = 0.0
    d = -180.0
    while d < 180.0:
        w = np.exp(-0.5 * (d / sigma_deg) ** 2)
        acc += w * rotate(L, d)
        wsum += w
        d += step
    return norm(acc / max(wsum, 1e-6))


def _polar(L, R=None, A=720):
    h, w = L.shape
    R = R or int(min(h, w) // 2)
    return cv2.warpPolar(L, (R, A), (w / 2 - 0.5, h / 2 - 0.5), R,
                         cv2.WARP_POLAR_LINEAR)


def _unpolar(P, shape):
    h, w = shape
    R, A = P.shape[1], P.shape[0]
    return cv2.warpPolar(P, (w, h), (w / 2 - 0.5, h / 2 - 0.5), R,
                         cv2.WARP_POLAR_LINEAR + cv2.WARP_INVERSE_MAP)


def avg_ang_lp(L, sigma_deg=10.0, A=720):
    P = _polar(L, A=A)
    sig = sigma_deg / 360.0 * A
    P = gaussian_filter1d(P, sig, axis=0, mode="wrap")
    return norm(_unpolar(P, L.shape))


def avg_harmonic(L, keep=1, A=720):
    P = _polar(L, A=A)
    F = np.fft.rfft(P, axis=0)
    if keep is not None:
        F[keep + 1:, :] = 0
    P2 = np.fft.irfft(F, n=A, axis=0)
    return norm(_unpolar(P2.real, L.shape))


METHODS = [
    ("original", lambda L: L),
    ("full", avg_full),
    ("weighted60", lambda L: avg_weighted_all(L, 60)),
    ("weighted90", lambda L: avg_weighted_all(L, 90)),
    ("limited30", lambda L: avg_limited(L, 30, 12)),
    ("limited15", lambda L: avg_limited(L, 15, 8)),
    ("harmonic1", lambda L: avg_harmonic(L, 1)),
    ("harmonic2", lambda L: avg_harmonic(L, 2)),
    ("harmonic3", lambda L: avg_harmonic(L, 3)),
]


def to_rgb(L):
    v = (np.clip(L, 0, 1) * 255).astype(np.uint8)
    return np.dstack([v, v, v])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", default="/tmp/rotavg")
    ap.add_argument("--cell", type=int, default=256)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    L = load_alpha(args.src)
    if L.shape[0] != args.cell:
        L = cv2.resize(L, (args.cell, args.cell), interpolation=cv2.INTER_AREA)

    rots = [0, 45, 90, 135]
    rows = []
    for name, fn in METHODS:
        V = fn(L)
        strip = np.concatenate([to_rgb(rotate(V, d)) for d in rots], axis=1)
        Image.fromarray(strip).save(os.path.join(args.out, f"{name}.png"))
        rows.append((name, strip))
        print(f"{name:10s} max {V.max():.2f} mean {V.mean():.3f}")

    from PIL import ImageDraw
    lab = 110
    sheet = Image.new("RGB", (lab + args.cell * len(rots),
                              args.cell * len(rows)), (0, 0, 0))
    d = ImageDraw.Draw(sheet)
    for r, (name, strip) in enumerate(rows):
        sheet.paste(Image.fromarray(strip), (lab, r * args.cell))
        d.text((8, r * args.cell + args.cell // 2), name, fill=(255, 255, 255))
    out = os.path.join(args.out, "compare.png")
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
