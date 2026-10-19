#!/usr/bin/env python3
"""danis_soften.py — 把 Danis 手绘火舌（danis5.png）做「低对比 / 宽而糊 / 带拖尾」处理。

输入：/james_pm/danis5.png（1115×256，5 个火舌横排，黑底）
输出：5 种处理方法的对比图 + 每种的 5 格 strip。

5 种手段（纯图像处理）：
  M1 airbrush  ：提中间调 + 膨胀 + 大半径高斯（最直接：宽、糊、低对比）
  M2 ribbon    ：腐蚀背景（=加粗笔画）+ 强模糊 + 少量原核（宽软带）
  M3 freqsep   ：频率分离，压低高频（细亮线），抬低频（软团）
  M4 trail     ：各向异性拖尾（沿 +y 方向衰减传播）+ 轻模糊
  M5 wisp      ：DoG 取亮核 → 带摆动地向上拖尾 → 与软底合成（最像火焰丝）

用法：
  python3 danis_soften.py --src /james_pm/danis5.png --out /tmp/danis_soft
"""

import argparse
import os

import cv2
import numpy as np
from PIL import Image

CELL = 256
N = 5


def load_cells(src, margin=10):
    """按连通域拆 5 格：每格只保留**该连通域自己的像素**，不会带进邻居的尾巴。"""
    im = np.array(Image.open(src).convert("RGB"), dtype=np.float32)
    h, w, _ = im.shape
    # 右下角「豆包AI生成」小水印
    im[h - 22:h, w - 50:w] = 0
    L = im.max(axis=2) / 255.0

    m = (L > 0.04).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    idx = [i for i in range(1, n) if stats[i, 4] > 500]
    idx.sort(key=lambda i: stats[i, 0])
    assert len(idx) >= N, f"只找到 {len(idx)} 个形状"

    cells = []
    for i in idx[:N]:
        x0, y0, bw, bh = stats[i, 0], stats[i, 1], stats[i, 2], stats[i, 3]
        own = np.where(lab == i, L, 0.0).astype(np.float32)   # 只留自己
        side = int(max(bw, bh) + 2 * margin)
        x1 = x0 - (side - bw) // 2
        y1 = y0 - (side - bh) // 2
        crop = np.zeros((side, side), np.float32)
        sx0, sy0 = max(0, x1), max(0, y1)
        sx1, sy1 = min(w, x1 + side), min(h, y1 + side)
        crop[sy0 - y1:sy0 - y1 + (sy1 - sy0), sx0 - x1:sx0 - x1 + (sx1 - sx0)] = \
            own[sy0:sy1, sx0:sx1]
        cells.append(cv2.resize(crop, (CELL, CELL), interpolation=cv2.INTER_AREA))
    return cells


def compress(L, k=1.15):
    """降低对比：抬中间调、压高光。"""
    x = np.clip(L, 0, 1)
    y = x / (1.0 + k * x)
    return y / (1.0 / (1.0 + k))


def maybe_blur(L, sigma):
    return cv2.GaussianBlur(L, (0, 0), sigma) if sigma > 0 else L


# ---------------- 5 种处理 ----------------
def m1_airbrush(L):
    lift = L ** 0.72
    wide = cv2.dilate(lift, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)))
    wide = maybe_blur(wide, 5.5)
    out = 0.40 * lift + 0.80 * wide
    return compress(maybe_blur(out, 1.5), 1.3)


def m2_ribbon(L):
    bg = 1.0 - L
    bg = cv2.erode(bg, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9)))
    fat = 1.0 - bg                      # 加粗后的笔画
    fat = maybe_blur(fat, 6.0)
    core = maybe_blur(L, 1.2)
    out = 0.85 * fat + 0.30 * core
    return compress(maybe_blur(out, 1.0), 1.4)


def m3_freqsep(L):
    low = maybe_blur(L, 7.0)
    high = L - low
    out = 1.15 * low + 0.22 * high
    out = cv2.dilate(out, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3)))
    return compress(maybe_blur(out, 2.0), 1.25)


def _trail_up(L, steps=14, decay=0.78, wobble=0.0):
    T = L.copy()
    for k in range(steps):
        sh = np.zeros_like(T)
        dx = int(round(wobble * np.sin(k * 0.7)))
        if dx >= 0:
            sh[:-1, dx:] = T[1:, :T.shape[1] - dx] if dx else T[1:]
        else:
            sh[:-1, :dx] = T[1:, -dx:]
        T = np.maximum(T, decay * sh)
    return T


def m4_trail(L):
    # 先做宽软底（保证低对比/宽笔触），再把软底向上拖尾
    soft = m1_airbrush(L)
    trail = _trail_up(soft, steps=24, decay=0.86, wobble=0.8)
    out = np.maximum(0.70 * soft, 1.05 * maybe_blur(trail, 2.0))
    return compress(maybe_blur(out, 1.5), 1.35)


def m5_wisp(L):
    core = np.clip(L - maybe_blur(L, 3.0), 0, 1)
    core = core / max(core.max(), 1e-6)
    trail = _trail_up(core, steps=26, decay=0.88, wobble=1.6)
    trail = maybe_blur(trail, 1.2)
    fat = cv2.dilate(L, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9)))
    base = maybe_blur(fat, 7.5)
    out = np.maximum(0.62 * base, 1.05 * trail)
    return compress(out, 1.35)


METHODS = [
    ("M1 airbrush", m1_airbrush),
    ("M2 ribbon", m2_ribbon),
    ("M3 freqsep", m3_freqsep),
    ("M4 trail", m4_trail),
    ("M5 wisp", m5_wisp),
]


def to_rgba(L):
    v = (np.clip(L, 0, 1) * 255 + 0.5).astype(np.uint8)
    cell = np.zeros((CELL, CELL, 4), np.uint8)
    cell[..., 0:3] = 255
    cell[..., 3] = v
    return cell


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/james_pm/danis5.png")
    ap.add_argument("--out", default="/tmp/danis_soft")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    cells = load_cells(args.src)
    rows = [("original", cells)]
    for name, fn in METHODS:
        rows.append((name, [fn(c) for c in cells]))
        # 单条 strip
        strip = np.zeros((CELL, CELL * N, 4), np.uint8)
        for i, c in enumerate(rows[-1][1]):
            strip[:, i * CELL:(i + 1) * CELL] = to_rgba(c)
        Image.fromarray(strip).save(os.path.join(args.out, name.split()[0] + ".png"))

    # 对比图：每行一个方法
    lab_w = 150
    sheet = Image.new("RGB", (lab_w + CELL * N, CELL * len(rows)), (0, 0, 0))
    from PIL import ImageDraw
    d = ImageDraw.Draw(sheet)
    for r, (name, cs) in enumerate(rows):
        for c, L in enumerate(cs):
            v = (np.clip(L, 0, 1) * 255).astype(np.uint8)
            sheet.paste(Image.fromarray(np.dstack([v, v, v])),
                        (lab_w + c * CELL, r * CELL))
        d.text((8, r * CELL + CELL // 2), name, fill=(255, 255, 255))

    out = os.path.join(args.out, "compare.png")
    sheet.save(out)
    print("wrote", out, sheet.size)
    for name, cs in rows:
        a = np.array(cs)
        print(f"{name:12s} mean {a.mean():.3f} cov(.1) {(a>0.1).mean():.3f} "
              f"p99 {np.percentile(a,99):.2f}")


if __name__ == "__main__":
    main()
