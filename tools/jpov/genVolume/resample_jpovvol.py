#!/usr/bin/env python3
"""把已有的 `.jpovvol` 体积序列**重采样**成"小体积、可入库、无缝循环"的样例。

为什么需要它：`data/fire_vdb/fire_stable.jpovvol`（viewer 默认样例）现在是从一份
**只有一根细柱**的 npz 烘出来的（48³×32 里 95% 体素为 0，只剩中间一条竖直细柱），
raymarch 出来就是"发光棒"，不是火。原始 96³×64 的 `fire_seq.jpovvol` 是正常的火焰，
但它 113MB，不能入库。本脚本负责把它压成 ~7MB 的 48³×32 循环样例。

链路（与 `npzseq_to_jpovvol.py` 同一套语义，只是源换成 jpovvol）：
  读 → 盒平均降采样 → 取稳态窗口 → 首尾交叉淡化（无缝循环）→ 边界淡出（去硬盒边）
  → 逐通道归一化 → 写 JPVOL2（u8，2 通道：R=密度 / G=温度）

用法：
  resample_jpovvol.py <src.jpovvol> <out.jpovvol> \
      [--scale 2] [--skip 16] [--frames 32] [--blend 6] [--window 0.18] [--dtype u8]

例（重烘仓库里的样例）：
  cd tools/jpov/genVolume
  python3 resample_jpovvol.py \
      /james_pm/fire_hero/assets/fire_seq.jpovvol \
      ../../../data/fire_vdb/fire_stable.jpovvol \
      --scale 2 --skip 16 --frames 32 --blend 6 --window 0.18 --dtype u8
"""
import argparse

import numpy as np

import jpvol


def loop_blend(arr, blend):
    """把尾部 blend 帧交叉淡化到开头 blend 帧，让 out[-1] 能无缝接 out[0]。

    arr: [F][z][y][x]（或多通道 [F][z][y][x][C]）。原地修改并返回。
    """
    if blend <= 0 or blend * 2 >= arr.shape[0]:
        return arr
    k = arr.shape[0]
    for i in range(k - blend, k):
        # t 从 1/(b+1) 递增到 b/(b+1)：最后一帧最接近第 0 帧。
        t = (i - (k - blend) + 1) / (blend + 1.0)
        src = i - (k - blend)
        arr[i] = (1.0 - t) * arr[i] + t * arr[src]
    return arr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="源 .jpovvol（JPVOL1/2）")
    ap.add_argument("out", help="目标 .jpovvol（JPVOL2）")
    ap.add_argument("--scale", type=int, default=2, help="降采样倍数（盒平均，>=1）")
    ap.add_argument("--skip", type=int, default=16, help="跳过前 N 帧（避开点火过程）")
    ap.add_argument("--frames", type=int, default=32, help="取多少帧（0=全部）")
    ap.add_argument("--blend", type=int, default=6, help="首尾交叉淡化帧数（0=关）")
    ap.add_argument("--window", type=float, default=0.18,
                    help="边界淡出比例（0=关）—— 消掉「火贴到盒壁被切」的硬边")
    ap.add_argument("--dtype", choices=["u8", "f16", "f32"], default="u8")
    ap.add_argument("--crop", type=float, default=0.0,
                    help="按密度阈值自动裁到内容 bbox（0=不裁）；只裁空边，不缩放")
    ap.add_argument("--crop-pad", type=int, default=1, help="裁剪时保留的体素边距")
    ap.add_argument("--voxel-m", type=float, default=0.02,
                    help="只用于打印裁后的世界尺寸（不写进文件）")
    args = ap.parse_args()

    arr = jpvol.read_jpovvol(args.src)     # [F][z][y][x][C]，float32 ∈ [0,1]
    nch = arr.shape[-1]
    if nch not in (2, 3):
        raise SystemExit("源体积需要 2 或 3 通道（R=密度, G=温度[, B=烟]），实际 %d" % nch)
    n_src_frames = arr.shape[0]
    print(f"src {args.src}: frames={n_src_frames} dims={arr.shape[1:4]} ch={nch}")

    dens = arr[..., 0]
    temp = arr[..., 1]
    smoke = arr[..., 2] if nch == 3 else None
    # 归一化基准取**降采样之前**的最大值：盒平均会把峰值削平，若之后再用新峰值归一化，
    # 整场密度会被放大（sigma×density 变大 → 火变胖、变亮，与源不一致）。
    dmax_src = float(dens.max()) or 1.0
    tmax_src = float(temp.max()) or 1.0
    smax_src = (float(smoke.max()) or 1.0) if smoke is not None else 1.0

    # 自动裁到内容 bbox：篝火资产是 66×96×66 的方盒，火焰只占下半 ~57%，
    # 不裁的话 viewer 的盒（=资产世界尺寸）有近一半是空的，火看起来小一半。
    if args.crop > 0.0:
        peak = dens.max(axis=0)                       # 逐体素时间峰值
        idx = np.nonzero(peak > args.crop)
        if idx[0].size:
            pad = max(0, args.crop_pad)
            z0, z1 = max(0, idx[0].min() - pad), idx[0].max() + pad + 1
            y0, y1 = max(0, idx[1].min() - pad), idx[1].max() + pad + 1
            x0, x1 = max(0, idx[2].min() - pad), idx[2].max() + pad + 1
            dens = dens[:, z0:z1, y0:y1, x0:x1]
            temp = temp[:, z0:z1, y0:y1, x0:x1]
            if smoke is not None:
                smoke = smoke[:, z0:z1, y0:y1, x0:x1]
            nz_, ny_, nx_ = dens.shape[1:]
            print(f"crop(thr={args.crop}): dims=({nx_},{ny_},{nz_}) "
                  f"world={nx_*args.voxel_m:.2f}x{ny_*args.voxel_m:.2f}x{nz_*args.voxel_m:.2f} m")
        else:
            print(f"crop(thr={args.crop}): 没有体素超阈值，跳过")

    dens = jpvol.downsample(dens, args.scale)
    temp = jpvol.downsample(temp, args.scale)
    if smoke is not None:
        smoke = jpvol.downsample(smoke, args.scale)
    print(f"downsample x{args.scale}: dims={dens.shape[1:]}")

    k = args.frames if args.frames > 0 else dens.shape[0]
    skip = max(0, min(args.skip, dens.shape[0] - 1))
    k = min(k, dens.shape[0] - skip)
    dens = dens[skip:skip + k].copy()
    temp = temp[skip:skip + k].copy()
    if smoke is not None:
        smoke = smoke[skip:skip + k].copy()
    print(f"window: skip={skip} frames={k}")

    dens = loop_blend(dens, args.blend)
    temp = loop_blend(temp, args.blend)
    if smoke is not None:
        smoke = loop_blend(smoke, args.blend)
    dens, temp = jpvol.apply_window(dens, temp, args.window)
    if smoke is not None:
        # 复用 jpvol.apply_window 的掩膜（第二路传全零只为拿到同一个 w）：烟通道也要淡边界。
        smoke, _ = jpvol.apply_window(smoke, np.zeros_like(smoke), args.window)

    # 逐通道归一化（基准 = 源最大值，见上）：引擎按 u8 的 0..255 读。
    dens = np.clip(dens / dmax_src, 0.0, 1.0)
    temp = np.clip(temp / tmax_src, 0.0, 1.0)
    if smoke is not None:
        smoke = np.clip(smoke / smax_src, 0.0, 1.0)

    nx, ny, nz, nf, nbytes = jpvol.write_jpovvol(args.out, dens, temp,
                                                 smoke=smoke, dtype=args.dtype)
    fill = 100.0 * float((dens > 0.1).mean())
    print(f"wrote {args.out}: dims=({nx},{ny},{nz}) frames={nf} ch={nch} "
          f"dtype={args.dtype} bytes={nbytes:,} | 密度>10%% 体素={fill:.1f}%")


if __name__ == "__main__":
    main()
