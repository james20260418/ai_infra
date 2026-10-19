#!/usr/bin/env python3
"""flame_curve_fit.py — 把手绘火舌掩膜（F1/F3/F5）反解成贝塞尔曲线参数。

背景（Danis）：Astralax《In Flames》里的 F1/F3/F5 是手绘 + 涂抹 + 高斯模糊，
文章没有给贝塞尔。要做「按曲线插值」的形状补间，第一步就是**从掩膜反解出
一组曲线参数**，让曲线版尽量逼近手绘版。

模型（与 flame_curve_gen.py 同族，略作扩展）：
  * 中心线：三次贝塞尔 p0→p3，三个横向自由度 x1/x2/x3（相对基点的偏移）
  * 宽度：w(u) = w0 * (1-u)^taper * (1 + bulge*sin(pi*u))
  * 侧摆：wig * sin(5.2u + phase) * u
  * 亮度：沿 u 的衰减 (1-u)^bt，横向高斯 exp(-d2*sharp)

拟合：随机搜索 + 逐参数坐标下降（只用 numpy，不依赖 scipy）。
目标：alpha 通道（没有 alpha 就用亮度）归一化到 [0,1]。

用法：
  python3 flame_curve_fit.py --out /tmp/curve_fit
"""

import argparse
import json
import math
import os

import numpy as np
from PIL import Image

# 参数顺序固定，方便坐标下降
PNAMES = ["h", "x1", "x2", "x3", "w0", "taper", "bulge",
          "wig", "phase", "sharp", "bt", "bx", "by"]

RANGES = {
    "h":     (0.40, 0.98),
    "x1":    (-0.30, 0.30),
    "x2":    (-0.35, 0.35),
    "x3":    (-0.40, 0.40),
    "w0":    (0.020, 0.170),
    "taper": (0.15, 1.80),
    "bulge": (-0.25, 0.70),
    "wig":   (0.000, 0.140),
    "phase": (0.0, 2.0 * math.pi),
    "sharp": (0.4, 5.0),
    "bt":    (0.0, 1.6),
    "bx":    (0.38, 0.62),
    "by":    (0.86, 0.99),
}


def load_alpha(path):
    a = np.array(Image.open(path).convert("RGBA"), dtype=np.float32) / 255.0
    al = a[..., 3]
    if al.max() < 1e-3:  # 没有 alpha 语义 → 用亮度
        al = a[..., :3].max(axis=2)
    mx = al.max()
    return al / mx if mx > 0 else al


def to_grid(alpha, S):
    """bbox 裁切 → 保持长宽比缩放到 S×S 的正方形（同比例居中）。"""
    ys, xs = np.nonzero(alpha > 0.02)
    if len(ys) == 0:
        return np.zeros((S, S), np.float32)
    y0, y1 = ys.min(), ys.max() + 1
    x0, x1 = xs.min(), xs.max() + 1
    crop = alpha[y0:y1, x0:x1]
    h, w = crop.shape
    side = max(h, w)
    pad = np.zeros((side, side), np.float32)
    oy = (side - h) // 2
    ox = (side - w) // 2
    pad[oy:oy + h, ox:ox + w] = crop
    im = Image.fromarray((pad * 255.0).astype(np.uint8)).resize((S, S), Image.BILINEAR)
    return np.array(im, dtype=np.float32) / 255.0


XX, YY = None, None


def _grid(S):
    global XX, YY
    if XX is None or XX.shape[0] != S:
        x = (np.arange(S, dtype=np.float32) + 0.5)
        XX = np.broadcast_to(x[None, :], (S, S)).copy()
        YY = np.broadcast_to(x[:, None], (S, S)).copy()
    return XX, YY


def render_field(p, S=64, n=96):
    xx, yy = _grid(S)
    h = p["h"]
    p0 = np.array([p["bx"] * S, p["by"] * S], np.float32)
    p1 = p0 + np.array([p["x1"] * S, -h * 0.38 * S], np.float32)
    p2 = p0 + np.array([p["x2"] * S, -h * 0.72 * S], np.float32)
    p3 = p0 + np.array([p["x3"] * S, -h * S], np.float32)

    t = np.linspace(0.0, 1.0, n, dtype=np.float32)[:, None]
    a = (1 - t) ** 3
    b = 3 * (1 - t) ** 2 * t
    c = 3 * (1 - t) * t ** 2
    d = t ** 3
    pts = a * p0 + b * p1 + c * p2 + d * p3
    u = t[:, 0]

    w = p["w0"] * S * (1.0 - u) ** p["taper"] * (1.0 + p["bulge"] * np.sin(np.pi * u))
    pts[:, 0] += p["wig"] * S * np.sin(u * 5.2 + p["phase"]) * u

    field = np.zeros((S, S), np.float32)
    amp = (1.0 - u) ** p["bt"]
    for k in range(n):
        r = max(float(w[k]), 0.7)
        d2 = ((xx - pts[k, 0]) ** 2 + (yy - pts[k, 1]) ** 2) / (r * r)
        field = np.maximum(field, amp[k] * np.exp(-d2 * p["sharp"]))
    mx = field.max()
    return field / mx if mx > 0 else field


def loss(p, target, S=64):
    f = render_field(p, S=S)
    mae = float(np.mean(np.abs(f - target)))
    thr = 0.15
    fa, ta = f > thr, target > thr
    inter = float(np.sum(fa & ta))
    union = float(np.sum(fa | ta)) + 1e-6
    iou = inter / union
    return mae + 0.60 * (1.0 - iou), iou, mae


def rand_params(rng):
    return {k: float(rng.uniform(*RANGES[k])) for k in PNAMES}


def clamp(p):
    return {k: float(np.clip(p[k], *RANGES[k])) for k in PNAMES}


def fit_one(target, seed=0, iters=2600, S=64):
    rng = np.random.default_rng(seed)
    best = []
    for _ in range(iters):
        p = rand_params(rng)
        l, iou, mae = loss(p, target, S=S)
        best.append((l, iou, mae, p))
    best.sort(key=lambda x: x[0])
    cands = best[:4]

    # 坐标下降：步长逐轮减半
    for l0, _, _, p in cands:
        cur = dict(p)
        lcur = loss(cur, target, S=S)[0]
        step = {k: (RANGES[k][1] - RANGES[k][0]) * 0.20 for k in PNAMES}
        for rnd in range(7):
            for k in PNAMES:
                for sgn in (1.0, -1.0):
                    q = dict(cur)
                    q[k] = float(np.clip(q[k] + sgn * step[k], *RANGES[k]))
                    lq = loss(q, target, S=S)[0]
                    if lq < lcur - 1e-5:
                        cur, lcur = q, lq
                        break
            for k in PNAMES:
                step[k] *= 0.55
        # 记录
        l, iou, mae = loss(cur, target, S=S)
        best.append((l, iou, mae, cur))

    best.sort(key=lambda x: x[0])
    l, iou, mae, p = best[0]
    return p, l, iou, mae


def save_preview(target, fitted, path, S=256):
    t = np.array(Image.fromarray((np.clip(target, 0, 1) * 255).astype(np.uint8))
                 .resize((S, S), Image.BILINEAR), dtype=np.float32) / 255.0
    f = np.clip(render_field(fitted, S=S), 0, 1)
    rgb = np.zeros((S, S * 3 + 16, 3), np.float32)
    rgb[:, :S] = t[..., None]
    rgb[:, S + 8:2 * S + 8] = f[..., None]
    over = np.zeros((S, S, 3), np.float32)
    over[..., 0] = t          # 红 = 目标
    over[..., 1] = f          # 绿 = 拟合
    rgb[:, 2 * S + 16:3 * S + 16] = over
    Image.fromarray((rgb * 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mask-dir", default="tools/jpov/assets/fire/mp")
    ap.add_argument("--out", default="/tmp/curve_fit")
    ap.add_argument("--iters", type=int, default=2600)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    names = [("F1", "mask_1.png"), ("F3", "mask_3.png"), ("F5", "mask_5.png")]
    out = {}
    for label, fn in names:
        alpha = load_alpha(os.path.join(args.mask_dir, fn))
        target = to_grid(alpha, 64)
        p, l, iou, mae = fit_one(target, seed=hash(label) & 0xFFFF, iters=args.iters)
        out[label] = {"params": p, "loss": l, "iou": iou, "mae": mae}
        save_preview(target, p, os.path.join(args.out, f"fit_{label}.png"))
        print(f"{label}: loss={l:.4f} IoU={iou:.3f} MAE={mae:.4f}")
        print("   ", {k: round(v, 4) for k, v in p.items()})

    with open(os.path.join(args.out, "fitted.json"), "w") as f:
        json.dump(out, f, indent=2)
    print("wrote", os.path.join(args.out, "fitted.json"))


if __name__ == "__main__":
    main()
