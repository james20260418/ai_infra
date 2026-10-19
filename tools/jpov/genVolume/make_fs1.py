#!/usr/bin/env python3
"""make_fs1.py — 从 Seedance 2.0 内插值视频生成 FS1 单团 flipbook（32 帧 8×4）。

输入：/james_pm/图像内插值.mp4（720×720, 24fps, 121 帧, 黑底白色火舌）
输出：tools/jpov/assets/fire/mp/fs1_32f_8x4.png（2048×1024 RGBA）

处理：
  1) 去掉视频水印（左上「豆包AI生成」、右下同款）：cv2.inpaint。
  2) 全片亮度求并集 bbox → 正方形裁切（留 8% 边距）→ 256×256。
  3) alpha = (L/255)^1.6（压暗辉光，保留白热核）；RGB=白（掩膜契约，颜色交给粒子色带）。
  4) 32 帧等距抽取（首帧→末帧），8×4 排布，并按 JPOV CellUV 行序翻转。

用法：
  python3 make_fs1.py --video "/james_pm/图像内插值.mp4" \
      --out tools/jpov/assets/fire/mp/fs1_32f_8x4.png
"""

import argparse
import math

import cv2
import numpy as np
from PIL import Image

TILES_X, TILES_Y = 8, 4
N_FRAMES = TILES_X * TILES_Y
CELL = 256

# 视频里「豆包AI生成」水印的两处位置（含少量 margin）
WM_RECTS = [(10, 10, 150, 60), (570, 650, 710, 715)]   # x0,y0,x1,y1


def remove_watermarks(bgr):
    mask = np.zeros(bgr.shape[:2], np.uint8)
    for (x0, y0, x1, y1) in WM_RECTS:
        roi = bgr[y0:y1, x0:x1]
        lum = roi.max(axis=2)
        m = (lum > 35).astype(np.uint8) * 255
        m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=1)
        mask[y0:y1, x0:x1] = np.maximum(mask[y0:y1, x0:x1], m)
    if mask.any():
        return cv2.inpaint(bgr, mask, 3, cv2.INPAINT_TELEA)
    return bgr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--gamma", type=float, default=1.6, help="alpha 的亮度 gamma")
    args = ap.parse_args()

    cap = cv2.VideoCapture(args.video)
    if not cap.isOpened():
        raise SystemExit(f"打不开视频：{args.video}")
    frames = []
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        frames.append(remove_watermarks(bgr))
    cap.release()
    total = len(frames)
    if total < N_FRAMES:
        raise SystemExit(f"视频帧数不足：{total} < {N_FRAMES}")
    print(f"read {total} frames")

    # 全片并集 bbox（亮度 > 10，排除水印区）
    x0s, x1s, y0s, y1s = [], [], [], []
    for bgr in frames[::4]:
        lum = bgr.max(axis=2)
        for (x0, y0, x1, y1) in WM_RECTS:
            lum[y0:y1, x0:x1] = 0
        ys, xs = np.nonzero(lum > 10)
        if len(xs) == 0:
            continue
        x0s.append(xs.min()); x1s.append(xs.max())
        y0s.append(ys.min()); y1s.append(ys.max())
    bx0, bx1 = min(x0s), max(x1s)
    by0, by1 = min(y0s), max(y1s)
    cx, cy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
    side = max(bx1 - bx0, by1 - by0) * 1.10
    side = min(side, min(frames[0].shape[0], frames[0].shape[1]))
    left = int(round(cx - side / 2))
    top = int(round(cy - side / 2))
    left = max(0, min(left, frames[0].shape[1] - int(side)))
    top = max(0, min(top, frames[0].shape[0] - int(side)))
    side = int(side)
    print(f"crop {left},{top} {side}x{side}")

    ids = [int(round(i * (total - 1) / (N_FRAMES - 1))) for i in range(N_FRAMES)]
    sheet = np.zeros((TILES_Y * CELL, TILES_X * CELL, 4), np.uint8)
    for i, fid in enumerate(ids):
        bgr = frames[fid][top:top + side, left:left + side]
        gray = bgr.max(axis=2).astype(np.float32) / 255.0
        gray = cv2.resize(gray, (CELL, CELL), interpolation=cv2.INTER_AREA)
        alpha = np.clip(gray, 0.0, 1.0) ** args.gamma
        cell = np.zeros((CELL, CELL, 4), np.uint8)
        cell[..., 0:3] = 255
        cell[..., 3] = (alpha * 255.0 + 0.5).astype(np.uint8)
        r, c = divmod(i, TILES_X)
        rr = TILES_Y - 1 - r          # JPOV CellUV 行序
        sheet[rr * CELL:(rr + 1) * CELL, c * CELL:(c + 1) * CELL] = cell

    Image.fromarray(sheet).save(args.out)
    a = sheet[..., 3].astype(np.float32) / 255.0
    print("wrote", args.out, sheet.shape,
          "alpha mean %.4f cov(>0.1) %.3f" % (a.mean(), (a > 0.1).mean()))


if __name__ == "__main__":
    main()
