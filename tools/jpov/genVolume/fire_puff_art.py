#!/usr/bin/env python3
"""fire_puff_art.py — 图像空间程序化「火团」flipbook（丝状火舌 + 白热核心 + 镂空）。

为什么走这条路：本机 Blender 4.0.2 的 Mantaflow bake 会崩、docker 不可用，而体积光栅
（fire_puff_sim.py）在 80³ 网格上只能出「发光团块」，做不出目标火团那种**细丝状、
互相缠绕、带镂空**的火舌。目标图本质是一张 2D 特效贴图，直接按 2D 画更快更准。

模型：
  * 一簇 = N 根「火丝」(strand)：从底部一组基点出发，向上生长、带正弦摆动与卷曲，
    粗细沿长度收细，根部白热、末端暗红。
  * 每根火丝的参数随时间做 sin/cos 相位摆动 → **32 帧无缝循环**（无点火/熄灭）。
  * 叠加：RGB 用加法累积（HDR），alpha 用 1-(1-a) 累积（覆盖率）。
  * 镂空：火丝之间的空隙天然留空；另叠加一层噪声遮罩把部分细丝打断。

用法：
  python3 fire_puff_art.py --out /tmp/art.png [--cell 256] [--frames 32] \
      [--strands 46] [--size 384] [--seed 3] [--sharp 1.0] [--alpha-gain 2.2]
"""

import argparse

import numpy as np

from make_flipbook import blackbody, write_png


def gauss_patch(img, cx, cy, rad, amp, extra=None):
    """在 (cx,cy) 处叠加一个半径 rad 的高斯斑（加法）。img: (H,W) 或 (H,W,3)。"""
    H, W = img.shape[0], img.shape[1]
    x0 = int(max(0, np.floor(cx - rad)))
    x1 = int(min(W, np.ceil(cx + rad) + 1))
    y0 = int(max(0, np.floor(cy - rad)))
    y1 = int(min(H, np.ceil(cy + rad) + 1))
    if x1 <= x0 or y1 <= y0:
        return
    yy = np.arange(y0, y1, dtype=np.float32)[:, None] + 0.5
    xx = np.arange(x0, x1, dtype=np.float32)[None, :] + 0.5
    d2 = ((xx - cx) ** 2 + (yy - cy) ** 2) / max(rad * rad, 1e-6)
    w = np.exp(-d2 * 2.2).astype(np.float32)
    if img.ndim == 2:
        img[y0:y1, x0:x1] += w * amp
    else:
        img[y0:y1, x0:x1, :] += w[..., None] * (amp * extra)[None, None, :]


def render_frame(H, W, t, strands, core_blobs, sharp, alpha_gain):
    rgb = np.zeros((H, W, 3), np.float32)
    cov = np.zeros((H, W), np.float32)
    tau = 2.0 * np.pi * t

    for s in strands:
        n = s["samples"]
        u = np.linspace(0.0, 1.0, n, dtype=np.float32)
        # 相位摆动（2πt 保证 f 回绕到 0 时完全一致）
        phase = s["phase"] + tau
        wob = np.sin(u * s["freq"] * 2.0 * np.pi + phase)
        curl = np.sin(u * s["curl_freq"] * 2.0 * np.pi + phase * 0.7 + 1.3)
        x = (s["x0"] + s["amp"] * wob * (u ** 1.25)
             + s["curl"] * curl * (u ** 1.6) + s["lean"] * (u ** 1.5))
        y = s["y0"] - s["len"] * u
        width = s["w"] * (1.0 - u) ** 0.75 + 0.6
        # 温度：根部最热，末端冷却；再做一点随机（细丝之间冷热不同）
        # 末端不完全熄灭：留 0.12 的暗红，避免“尖端直接消失”
        temp = 0.12 + (s["temp"] - 0.12) * (1.0 - u) ** 1.05
        temp = np.clip(temp * (0.85 + 0.25 * np.sin(u * 5.0 + phase)), 0.0, 1.0)
        col = blackbody(temp) * (temp[..., None] ** sharp) * s["gain"]
        amp = width * s["w_amp"]
        for k in range(n):
            gauss_patch(rgb, float(x[k]), float(y[k]), float(width[k] * 2.6),
                        float(amp[k]), extra=col[k])
            gauss_patch(cov, float(x[k]), float(y[k]), float(width[k] * 2.0),
                        float(amp[k]) * 0.55)

    # 底部熔核：几个大软斑 → 火团“厚实的主体”（否则只有丝，像线团）
    for k, (cx, cy, rad, amp, tp) in enumerate(core_blobs):
        wob = 1.0 + 0.18 * np.sin(tau + k * 1.7)
        gauss_patch(rgb, cx + 6.0 * np.sin(tau + k), cy, rad * wob, amp,
                    extra=blackbody(np.array([tp], np.float32))[0])
        gauss_patch(cov, cx + 6.0 * np.sin(tau + k), cy, rad * wob * 0.85, amp * 0.55)

    # 簇形遮罩：把散得太远的丝收进一个“团”的剪影里
    yy = (np.arange(H, dtype=np.float32) + 0.5)[:, None]
    xx = (np.arange(W, dtype=np.float32) + 0.5)[None, :]
    rx = (xx - W * 0.5) / (W * 0.46)
    ry = (yy - H * 0.62) / (H * 0.44)
    rad2 = rx * rx + ry * ry
    mask = np.clip(1.25 - rad2, 0.0, 1.0) ** 0.8
    rgb *= mask[..., None]
    cov *= mask

    # 内部噪声：给火体内部挖一些暗斑（否则主体是一块均匀的亮斑）
    nz = (np.sin(xx * 0.11 + tau * 1.3) * np.cos(yy * 0.13 - tau) +
          np.sin((xx + yy) * 0.07 + 1.7))
    nz = 0.5 + 0.5 * np.tanh(nz * 0.9)
    internal = 0.62 + 0.55 * nz
    rgb *= internal[..., None]
    cov *= (0.75 + 0.45 * nz)

    # 覆盖率 → alpha（可调对比度）
    alpha = 1.0 - np.exp(-cov * alpha_gain)
    # 只保留有内容的部分，避免整张卡泛灰
    alpha = np.clip((alpha - 0.02) / 0.98, 0.0, 1.0)
    # 颜色归一化到 alpha（避免透明区还带色）
    out_rgb = np.clip(rgb, 0.0, 1.0)
    return out_rgb, alpha


def make_strands(rng, H, W, n):
    strands = []
    for i in range(n):
        ang = 2.0 * np.pi * i / n + rng.normal(0, 0.25)
        ring = 0.13 * (0.35 + 0.65 * rng.random()) if n > 1 else 0.0
        # 基点：一簇（中间小圆环 + 抖动），火舌从不同位置长出
        bx = W * (0.5 + ring * np.cos(ang) + rng.normal(0, 0.035))
        by = H * (0.94 + rng.normal(0, 0.02))
        # 外倾：靠外的火丝往外飘，形成“一簇”而不是“一柱”
        lean = W * (0.10 * (bx / W - 0.5) * 2.0) + rng.normal(0, 0.02) * W
        strands.append({
            "x0": float(bx),
            "y0": float(by),
            "len": float(H * (0.40 + 0.50 * rng.random())),
            "amp": float(W * (0.04 + 0.08 * rng.random())),
            "curl": float(W * (0.02 + 0.06 * rng.random())),
            "freq": float(0.5 + 1.0 * rng.random()),
            "curl_freq": float(0.9 + 1.6 * rng.random()),
            "phase": float(rng.random() * 2 * np.pi),
            "w": float(3.0 + 4.5 * rng.random()),
            "w_amp": float(0.25 + 0.35 * rng.random()),
            "temp": float(0.35 + 0.55 * rng.random()),
            "gain": float(0.35 + 0.55 * rng.random()),
            "lean": float(lean),
            "samples": 72,
        })
    return strands


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--size", type=int, default=384)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--strands", type=int, default=46)
    p.add_argument("--seed", type=int, default=3)
    p.add_argument("--sharp", type=float, default=1.35)
    p.add_argument("--alpha-gain", type=float, default=1.6)
    args = p.parse_args()

    H = W = args.size
    rng = np.random.default_rng(args.seed)
    strands = make_strands(rng, H, W, args.strands)
    # 底部熔核（位置/粗细固定，随时间轻微呼吸）
    core_blobs = [
        (W * 0.50, H * 0.85, W * 0.085, 0.40, 1.00),
        (W * 0.43, H * 0.88, W * 0.070, 0.30, 0.86),
        (W * 0.57, H * 0.88, W * 0.075, 0.30, 0.90),
        (W * 0.47, H * 0.79, W * 0.060, 0.22, 0.78),
        (W * 0.54, H * 0.78, W * 0.055, 0.20, 0.72),
        (W * 0.50, H * 0.93, W * 0.080, 0.26, 0.55),   # 底部暗红
    ]

    tiles = args.tiles
    cell = args.cell
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for f in range(args.frames):
        t = f / float(args.frames)
        rgb, alpha = render_frame(H, W, t, strands, core_blobs, args.sharp, args.alpha_gain)
        img = np.zeros((H, W, 4), np.float32)
        img[..., :3] = rgb
        img[..., 3] = alpha
        # 等比缩到 cell（区域平均 = 抗锯齿）
        k = H // cell
        small = img.reshape(cell, k, cell, k, 4).mean(axis=(1, 3))
        r, c = f // tiles, f % tiles
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = small

    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
