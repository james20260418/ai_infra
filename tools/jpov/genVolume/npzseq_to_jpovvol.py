#!/usr/bin/env python3
"""把 Mantaflow 每帧导出的 .npz（density/heat/flame）→ 无缝循环 → .jpovvol 序列（JPVOL2）。

JPVOL2 格式定义（单一来源）见同目录 `jpvol.py`；此处只做 **Mantaflow npz → 数组 → 写盘**。

循环：取稳态窗口，对尾部 B 帧与开头 B 帧做交叉淡化，使首尾衔接。

用法：npzseq_to_jpovvol.py <npz_dir> <out.jpovvol> [--frames K] [--skip S] [--blend B]
                          [--dtype u8|f32] [--scale N] [--window F]
"""
import argparse
import glob
import os

import numpy as np

import jpvol


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
        src = i - (K - b)                   # 对应开头的帧
        dens[i] = (1.0 - t) * dens[i] + t * dens[src]
        temp[i] = (1.0 - t) * temp[i] + t * temp[src]
    return dens, temp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz_dir")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--skip", type=int, default=16)
    ap.add_argument("--blend", type=int, default=8)
    ap.add_argument("--dtype", choices=["u8", "f32"], default="u8")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--window", type=float, default=0.0,
                    help="边界淡出比例（0=关；如 0.18）——消除硬盒边")
    args = ap.parse_args()

    dens, temp = load_frames(args.npz_dir, args.skip, args.frames or 10 ** 9)
    dens = jpvol.downsample(dens, args.scale)
    temp = jpvol.downsample(temp, args.scale)
    dens, temp = make_loop(dens, temp, args.blend)
    dens, temp = jpvol.apply_window(dens, temp, args.window)

    dmax = float(dens.max()) or 1.0
    tmax = float(temp.max()) or 1.0
    dens = np.clip(dens / dmax, 0.0, 1.0)
    temp = np.clip(temp / tmax, 0.0, 1.0)

    X, Y, Z, F, nbytes = jpvol.write_jpovvol(args.out, dens, temp, dtype=args.dtype)
    print(f"wrote {args.out}: dims=({X},{Y},{Z}) frames={F} ch=2 dtype={args.dtype} "
          f"bytes={nbytes}  densmax={dmax:.3g} tempmax={tmax:.3g}")


if __name__ == "__main__":
    main()
