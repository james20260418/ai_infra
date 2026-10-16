#!/usr/bin/env python3
"""从 EmberGen 官方渲染视频抽「标准答案」参考帧（ground truth）入库。

官方素材 = JangaFX EmberGen 免费 VDB（**CC0，可商用**）套件自带的 `smallCampfire.mp4`
（200 帧，1626×1152，纯黑底、孤立火 + 烟柱）。它就是「我们手上这份 CC0 VDB 的官方
标准渲染」，因此天然是机器可比对的 ground truth。
下载页：https://jangafx.com/software/embergen/download/free-vdb-animations

用法::

    python3 fetch_reference.py --video smallCampfire.mp4 --outdir reference \\
        --frames 60 100 140 [--width 576]

输出：``<outdir>/embergen_smallCampfire_f<NNNN>.png``（--width 给定则等比缩略，
供肉眼比对与仓库体积控制）。帧号与 VDB 帧号同序，见
``docs/jpov_campfire_repro_plan.md`` §1.1。
"""

import argparse
import os
import sys
from typing import List

import imageio.v3 as iio
import numpy as np
from PIL import Image


def extract(video: str, outdir: str, frames: List[int], width: int) -> int:
    """从 video 抽指定帧号，缩略后写到 outdir；返回写出张数。"""
    os.makedirs(outdir, exist_ok=True)
    wanted = set(frames)
    last = max(wanted)
    written = 0
    for idx, frame in enumerate(iio.imiter(video, plugin="FFMPEG")):
        if idx in wanted:
            img = Image.fromarray(np.asarray(frame))
            if width > 0 and img.width > width:
                height = max(1, int(img.height * width / img.width))
                img = img.resize((width, height))
            path = os.path.join(outdir, f"embergen_smallCampfire_f{idx:04d}.png")
            img.save(path)
            print(f"wrote {path}  ({img.width}x{img.height})")
            written += 1
        if idx >= last:
            break
    return written


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Extract EmberGen reference frames (CC0)")
    ap.add_argument("--video", required=True, help="官方渲染视频（smallCampfire.mp4）")
    ap.add_argument("--outdir", required=True, help="输出目录（通常 reference/）")
    ap.add_argument(
        "--frames",
        type=int,
        nargs="+",
        required=True,
        help="要抽的帧号（与 VDB 帧同序）",
    )
    ap.add_argument("--width", type=int, default=576, help="缩略目标宽度，0 = 原尺寸")
    args = ap.parse_args(argv)

    written = extract(args.video, args.outdir, args.frames, args.width)
    if written == 0:
        print("错误：未抽出任何帧（帧号超界？）", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
