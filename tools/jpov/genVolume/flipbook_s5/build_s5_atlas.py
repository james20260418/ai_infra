#!/usr/bin/env python3
"""build_s5_atlas.py — F5 → S5 → 6 条两段链 × 12 帧 → 12×6 flipbook 图集。

方法（火焰/烟/水花共用，已验收）：

  F1..F5                 5 张关键帧（extract_keyframes.py 产出）
  S_i = morph(F_i, F_{i+1}, 0.5)       相邻中点；S5 用 F5→F1 回绕
  链 = 6 条两段链（每条 12 帧 = 6 帧 + 6 帧，中点共享不重复）
  atlas = 12 列 × 6 行；**行序按 JPOV CellUV 翻转**（vrow = tilesY-1-row）

为什么绕一圈 S 而不是直接 F→F：直接两两补间时，每颗粒子看到的是"两三张原图之间的
硬过渡"；先把相邻帧的中点算出来当节点，再从节点出发走两段，**一整条 12 帧链的形变
速率更均匀**，且粒子随机选块时不会总是撞到同一段。

用法：
  python3 build_s5_atlas.py --frames ./f --out water_s5_flow_12x6.png
  python3 build_s5_atlas.py --frames ./f --out x.png --cell 256 --frames-per-seg 6
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from morph_flow import morph  # noqa: E402

# 6 条两段链（下标指向 S1..S5）。相邻链覆盖全部相邻段，第 6 条给一个跨跳变体。
CHAINS = [
    (0, 1, 2),
    (1, 2, 3),
    (2, 3, 4),
    (3, 4, 0),
    (4, 0, 1),
    (0, 2, 4),
]


def load_alpha(path: str) -> np.ndarray:
    """读白形 RGBA 的 alpha；没有 alpha 通道时退化成亮度。"""
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im, dtype=np.float32)[..., 3] / 255.0
    if a.max() < 1e-3:
        a = np.asarray(im.convert("L"), dtype=np.float32) / 255.0
    return a


def to_cell(a: np.ndarray, cell: int) -> np.ndarray:
    im = Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8), "L")
    return np.asarray(im.resize((cell, cell), Image.LANCZOS), dtype=np.float32) / 255.0


def make_sheet(frames: list[np.ndarray], tiles_x: int) -> np.ndarray:
    cell = frames[0].shape[0]
    tiles_y = int(np.ceil(len(frames) / tiles_x))
    sheet = np.zeros((tiles_y * cell, tiles_x * cell, 4), np.uint8)
    for i, f in enumerate(frames):
        r, c = divmod(i, tiles_x)
        rr = tiles_y - 1 - r          # ⚠️ JPOV CellUV: vrow = tilesY-1-row
        v = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
        y0, x0 = rr * cell, c * cell
        sheet[y0:y0 + cell, x0:x0 + cell, 0:3] = 255
        sheet[y0:y0 + cell, x0:x0 + cell, 3] = v
    return sheet


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", required=True, help="含 F1.png..F5.png 的目录")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cell", type=int, default=256, help="单格像素（正方形）")
    ap.add_argument("--frames-per-seg", type=int, default=6, help="每段帧数（6+6=12）")
    args = ap.parse_args()

    n = args.frames_per_seg
    F = [to_cell(load_alpha(os.path.join(args.frames, f"F{i}.png")), args.cell)
         for i in range(1, 6)]

    # S1..S5 = 相邻关键帧中点（S5 = F5/F1 回绕）
    S = [morph(F[i], F[(i + 1) % 5], 0.5) for i in range(5)]
    for i, s in enumerate(S):
        out = np.zeros((args.cell, args.cell, 4), np.uint8)
        out[..., 0:3] = 255
        out[..., 3] = (s * 255 + 0.5).astype(np.uint8)
        Image.fromarray(out, "RGBA").save(os.path.join(os.path.dirname(args.out) or ".",
                                                       f"S{i + 1}.png"))

    # 6 条链 × 每段 n 帧（第一段 t=0..(n-1)/n，第二段 t=1/n..1 → 中点不重复）
    frames: list[np.ndarray] = []
    for chain in CHAINS:
        a, b, c = (S[k] for k in chain)
        for i in range(n):
            frames.append(morph(a, b, i / float(n)))
        for i in range(1, n + 1):
            frames.append(morph(b, c, i / float(n)))

    sheet = make_sheet(frames, tiles_x=len(frames) // len(CHAINS))
    Image.fromarray(sheet, "RGBA").save(args.out)
    print(f"wrote {args.out} {sheet.shape} ({len(frames)} frames, "
          f"{len(CHAINS)} chains x {2 * n})")


if __name__ == "__main__":
    main()
