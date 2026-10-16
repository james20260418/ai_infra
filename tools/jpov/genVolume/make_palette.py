#!/usr/bin/env python3
"""make_palette.py — 把色谱 JSON（Fire Color Ramp）烘成 **1D LUT 纹理**（PNG，宽 N 高 1）。

色谱是「强度/温度 → 颜色」的查表（EmberGen 的 Color Gradient / Houdini 的 Fire Color Ramp
同款）。单独放文件里管理，转换/渲染按需引用，不写死在 shader 里。

用法：
  python3 make_palette.py palettes/fire_reference.json out/fire_reference.png [--size 256]

JSON 格式：{ "colorspace": "srgb"|"linear", "interp": "linear", "stops": [[pos,[r,g,b]], ...] }
输出 PNG：宽=size、高=1、RGB8；像素 i 对应 pos = i/(size-1)。
"""
import argparse
import json
import os

import numpy as np
from PIL import Image


def sample(stops, x):
    """在 stops（按 pos 升序）上做分段线性插值，x∈[0,1] → [r,g,b]。"""
    pos = np.array([s[0] for s in stops], dtype=np.float64)
    col = np.array([s[1] for s in stops], dtype=np.float64)
    out = np.empty(3, dtype=np.float64)
    for c in range(3):
        out[c] = np.interp(x, pos, col[:, c])
    return out


def write_lut(palette_json, out_png, size=256):
    """读色谱 JSON → 写 1D LUT PNG（宽=size,高=1,RGB8）。返回 (size, stop 数)。"""
    with open(palette_json, encoding="utf-8") as f:
        pal = json.load(f)
    stops = sorted(pal["stops"], key=lambda s: s[0])
    if not stops:
        raise ValueError("色谱没有 stops：%s" % palette_json)
    xs = np.linspace(0.0, 1.0, size)
    lut = np.stack([sample(stops, float(x)) for x in xs])           # [size,3] ∈[0,1]
    rgb8 = np.clip(lut * 255.0 + 0.5, 0, 255).astype(np.uint8).reshape(1, size, 3)
    os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
    Image.fromarray(rgb8, "RGB").save(out_png)
    return size, len(stops)


def main():
    ap = argparse.ArgumentParser(description="色谱 JSON → 1D LUT PNG")
    ap.add_argument("palette_json")
    ap.add_argument("out_png")
    ap.add_argument("--size", type=int, default=256, help="LUT 宽度（像素）")
    args = ap.parse_args()
    n, stops = write_lut(args.palette_json, args.out_png, args.size)
    print("wrote %s  size=%d  stops=%d" % (args.out_png, n, stops))


if __name__ == "__main__":
    main()
