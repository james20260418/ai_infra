#!/usr/bin/env python3
"""把 Mantaflow 每帧导出的 .npz（density/heat/flame）→ 无缝循环 → .jpovvol 序列（JPVOL2）。

JPVOL2 文件格式（little-endian）：
  offset 0  : char   magic[8] = "JPVOL2\0\0"
  offset 8  : uint32 version  (= 2)
  offset 12 : uint32 nx, ny, nz          // 体积维数（各 > 0）
  offset 24 : uint32 nframes             // 帧数（>= 1）
  offset 28 : uint32 channels            // 通道数（本版 = 2：R=density, G=temperature）
  offset 32 : uint32 dtype               // 0 = float32, 1 = uint8(归一化)
  offset 36 : uint32 reserved
  offset 40 : 数据 = nframes*nx*ny*nz*channels
              体素序：每帧内 x 最快 → y → z；帧间顺序排列（frame 最慢）。
  ⇒ 与「每帧一张 glTexImage3D / 逐层上传一张 2D 数组」的期望布局一致。

循环：取稳态窗口，对尾部 B 帧与开头 B 帧做交叉淡化，使首尾衔接。

用法：vdbseq_to_jpovvol.py <npz_dir> <out.jpovvol> [--frames K] [--skip S] [--blend B] [--dtype u8|f32] [--scale N]
"""
import argparse
import glob
import json
import os
import struct

import numpy as np

MAGIC = b"JPVOL2\x00\x00"


def load_frames(d, skip, k):
    files = sorted(glob.glob(os.path.join(d, "f*.npz")))
    assert files, "no npz frames in " + d
    dens = []
    temp = []
    for f in files:
        z = np.load(f)
        dens.append(z["density"].astype(np.float32))
        temp.append(z["heat"].astype(np.float32))
    dens = np.stack(dens)   # [F][z][y][x]
    temp = np.stack(temp)
    F = dens.shape[0]
    skip = min(skip, max(0, F - 1))
    k = min(k, F - skip)
    return dens[skip:skip + k], temp[skip:skip + k]


def make_loop(dens, temp, blend):
    """尾部 blend 帧与开头 blend 帧交叉淡化，令 out[K-1] 衔接 out[0]。"""
    K = dens.shape[0]
    if blend <= 0 or blend * 2 >= K:
        return dens, temp
    b = blend
    for i in range(K - b, K):
        t = (i - (K - b) + 1) / (b + 1.0)
        w = t * dens[i - (K - b)].shape[0]  # noop, keep shape clarity
        src = i - (K - b)                   # 对应开头的帧
        dens[i] = (1.0 - t) * dens[i] + t * dens[src]
        temp[i] = (1.0 - t) * temp[i] + t * temp[src]
    return dens, temp


def downsample(a, s):
    if s <= 1:
        return a
    F, Z, Y, X = a.shape
    Z2, Y2, X2 = Z // s, Y // s, X // s
    return a[:, :Z2 * s, :Y2 * s, :X2 * s].reshape(F, Z2, s, Y2, s, X2, s).mean(axis=(2, 4, 6))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz_dir")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--skip", type=int, default=16)
    ap.add_argument("--blend", type=int, default=8)
    ap.add_argument("--dtype", choices=["u8", "f32"], default="u8")
    ap.add_argument("--scale", type=int, default=1)
    args = ap.parse_args()

    dens, temp = load_frames(args.npz_dir, args.skip, args.frames or 10 ** 9)
    dens = downsample(dens, args.scale)
    temp = downsample(temp, args.scale)
    dens, temp = make_loop(dens, temp, args.blend)

    F, Z, Y, X = dens.shape
    dmax = float(dens.max()) or 1.0
    tmax = float(temp.max()) or 1.0
    dens = np.clip(dens / dmax, 0.0, 1.0)
    temp = np.clip(temp / tmax, 0.0, 1.0)

    # 组帧：[F][z][y][x][2] → 扁平化：frame→z→y→x，x 最快，通道交错
    data = np.stack([dens, temp], axis=-1)   # [F][z][y][x][2]
    data = np.ascontiguousarray(data)

    if args.dtype == "u8":
        payload = (data * 255.0 + 0.5).astype(np.uint8)
        dtype_code = 1
    else:
        payload = data.astype(np.float32)
        dtype_code = 0

    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<6I", 2, X, Y, Z, F, 2))
        f.write(struct.pack("<2I", dtype_code, 0))
        f.write(payload.tobytes())
    print(f"wrote {args.out}: dims=({X},{Y},{Z}) frames={F} ch=2 dtype={args.dtype} "
          f"bytes={40 + payload.nbytes}  densmax={dmax:.3g} tempmax={tmax:.3g}")


if __name__ == "__main__":
    main()
