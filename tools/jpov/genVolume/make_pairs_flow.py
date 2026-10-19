#!/usr/bin/env python3
"""make_pairs_flow.py — 成对渐变 v2：两条渐变链 / 每颗粒子一条，中间帧用算法预烘。

与旧版 `flames_pairs_12x1.png` 的区别：
    旧版：每对只有 A、B 两帧，运行时靠 shader 线性混合（采样简单差值）；
    v2  ：每块 = **两段**渐变的 12 帧（S→M，M→E），运行时一个寿命走完整块
          （= 两段，速度是单段的 2 倍）。

链（起点/终点随机：每颗粒子随机选一条链）：
    (F1,F3,F5) (F3,F5,F1) (F5,F1,F3)     —— 正向
    (F1,F5,F3) (F5,F3,F1) (F3,F1,F5)     —— 反向
这样 6 种有序转移全部覆盖，且两段之间不跳（第二段从第一段的终点继续）。

同时生成两种插值版本（viewer 里有 toggle）：
    FLOW    → mp/flames_pairs_flow_2seg_12x6.png
    PROFILE → mp/flames_pairs_profile_2seg_12x6.png

用法：
  python3 make_pairs_flow.py
"""

import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morph_util  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets", "fire", "mp")
CELL = 256
SEG_N = 6                      # 每段 6 帧（含首尾）
BLOCK_N = SEG_N * 2            # 每块 12 帧（两段；接缝处那帧重复一次）
CHAINS = [(1, 3, 5), (3, 5, 1), (5, 1, 3), (1, 5, 3), (5, 3, 1), (3, 1, 5)]


def load_mask(n):
    p = os.path.join(ASSETS, f"mask_{n}.png")
    a = np.array(Image.open(p).convert("RGBA"), dtype=np.float32)[..., 3] / 255.0
    return np.array(Image.fromarray((a * 255).astype(np.uint8))
                    .resize((CELL, CELL), Image.BILINEAR), dtype=np.float32) / 255.0


def build(masks, method, out_name):
    rows, cols = len(CHAINS), BLOCK_N
    sheet = np.zeros((rows * CELL, cols * CELL, 4), np.uint8)
    for r, (s, m, e) in enumerate(CHAINS):
        seg1 = morph_util.morph_frames(masks[s], masks[m], SEG_N, method)
        seg2 = morph_util.morph_frames(masks[m], masks[e], SEG_N, method)
        frames = seg1 + seg2
        rr = rows - 1 - r              # JPOV CellUV 行序
        for c, f in enumerate(frames):
            cell = np.zeros((CELL, CELL, 4), np.uint8)
            cell[..., 0:3] = 255
            cell[..., 3] = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
            sheet[rr * CELL:(rr + 1) * CELL, c * CELL:(c + 1) * CELL] = cell
        print(f"chain {r}: F{s}->F{m}->F{e} ({method})")
    out = os.path.join(ASSETS, out_name)
    Image.fromarray(sheet).save(out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", out, sheet.shape, "alpha mean %.3f cov %.3f"
          % (a.mean(), (a > 0.1).mean()))


def main():
    masks = {n: load_mask(n) for n in (1, 3, 5)}
    build(masks, "flow", "flames_pairs_flow_2seg_12x6.png")
    build(masks, "profile", "flames_pairs_profile_2seg_12x6.png")


if __name__ == "__main__":
    main()
