#!/usr/bin/env python3
"""extract_keyframes.py — 把「一张多格关键帧图」拆成 F1..FN 单帧 alpha 图。

产线第一步：Danis 给的素材常常是**横排一张**（黑底白花/白烟，几格连排），
这里负责拆格 + 取 alpha + 归一化到同一画布，供 build_s5_atlas.py 使用。

自动拆格的依据：格的背景是黑的 ⇒ 列亮度均值会在格与格之间出现「低谷」。
用 1% 分位做背景阈值，找连续低谷的中位数当切割线；格数由 --count 指定（默认 5）。

输出：<out>/F1.png .. FN.png（白 RGB + alpha=形状，CELL×CELL，形状居中、最长边占 FILL）。

用法：
  python3 extract_keyframes.py --src /james_pm/danis/water_splash.png --out ./f
  # 背景不是纯黑 / 有水印：
  python3 extract_keyframes.py --src in.png --out ./f --count 5 --cut-bottom 160
"""

from __future__ import annotations

import argparse
import os

import numpy as np
from PIL import Image

CELL_DEFAULT = 512
FILL_DEFAULT = 0.80
SOFT_DEFAULT = (0.015, 0.85)   # alpha 软阈值 (lo, hi)：低于 lo 全透明，高于 hi 全不透明


def find_splits(alpha: np.ndarray, count: int) -> list[int]:
    """按列亮度低谷自动找 count-1 条切割线，返回 [0, s1, ..., W]。"""
    col = alpha.mean(axis=0)
    thr = float(np.percentile(col, 25)) * 0.5
    groups: list[list[int]] = []
    for x, v in enumerate(col):
        if v <= thr:
            if groups and x - groups[-1][-1] <= 2:
                groups[-1].append(x)
            else:
                groups.append([x])
    centers = [int(np.mean(g)) for g in groups if len(g) >= 4]
    if len(centers) < count - 1:
        raise SystemExit(
            f"只找到 {len(centers)} 个列低谷，不足 {count - 1} 条切割线；"
            "请用 --splits 手工指定（逗号分隔，含首尾 0 与 W）。")
    # 取最靠近等分位置的 count-1 条
    want = [ (i + 1) * alpha.shape[1] / count for i in range(count - 1) ]
    picks = sorted({min(centers, key=lambda c: abs(c - w)) for w in want})
    return [0] + picks + [alpha.shape[1]]


def extract_cell(rgb: np.ndarray, x0: int, x1: int, y0: int, y1: int,
                 cell: int, fill: float, soft: tuple[float, float]) -> np.ndarray:
    reg = rgb[y0:y1, x0:x1]
    lum = reg.max(axis=2)
    bg = float(np.percentile(lum, 1.0))
    lo = max(bg + 0.01, soft[0])
    a = np.clip((lum - lo) / (soft[1] - lo), 0.0, 1.0)
    a = a * a * (3 - 2 * a)                      # smoothstep：软边更自然
    ys, xs = np.where(a > 0.01)
    if len(ys) == 0:
        raise SystemExit("某一格是空的：检查切割线或阈值")
    sub = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h, w = sub.shape
    scale = fill * cell / max(h, w)              # 统一按「最长边」归一，形状相对大小不变
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    img = Image.fromarray((sub * 255).astype(np.uint8), "L").resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("L", (cell, cell), 0)
    canvas.paste(img, ((cell - nw) // 2, (cell - nh) // 2))   # 居中（bbox 中心对齐）
    out = np.zeros((cell, cell, 4), np.uint8)
    out[..., 0:3] = 255
    out[..., 3] = np.asarray(canvas)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="横排关键帧图（黑底）")
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=5)
    ap.add_argument("--cell", type=int, default=CELL_DEFAULT)
    ap.add_argument("--fill", type=float, default=FILL_DEFAULT)
    ap.add_argument("--splits", default="", help="手工切割线，逗号分隔（含 0 与 W）")
    ap.add_argument("--cut-top", type=int, default=0, help="先切掉顶部 N 像素（去标题/水印）")
    ap.add_argument("--cut-bottom", type=int, default=0, help="先切掉底部 N 像素（去水印）")
    args = ap.parse_args()

    rgb = np.asarray(Image.open(args.src).convert("RGB"), dtype=np.float32) / 255.0
    if args.cut_top or args.cut_bottom:
        rgb = rgb[args.cut_top: rgb.shape[0] - args.cut_bottom]
    lum = rgb.max(axis=2)

    if args.splits:
        splits = [int(v) for v in args.splits.split(",")]
    else:
        splits = find_splits(lum, args.count)
    print("splits:", splits)

    os.makedirs(args.out, exist_ok=True)
    for i in range(len(splits) - 1):
        rgba = extract_cell(rgb, splits[i], splits[i + 1], 0, rgb.shape[0],
                            args.cell, args.fill, SOFT_DEFAULT)
        Image.fromarray(rgba, "RGBA").save(os.path.join(args.out, f"F{i + 1}.png"))
        cov = float((rgba[..., 3] > 25).mean())
        print(f"F{i+1}: coverage {cov:.4f}")


if __name__ == "__main__":
    main()
