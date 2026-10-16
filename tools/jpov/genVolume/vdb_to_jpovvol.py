#!/usr/bin/env python3
"""vdb_to_jpovvol.py — **通用** OpenVDB → `.jpovvol` 体积转换器（不特化「火」）。

把 OpenVDB（单帧或 `%04d` 序列）里**任意命名的 scalar grid**，按调用者给的
「grid → 通道」映射，烘成引擎可读的 `.jpovvol`（JPVOL2，见同目录 `jpvol.py`）。

设计原则（2026-10-16 Danis 定）：**转换器只负责「把 VDB 的场装进通道」，不预设物理语义。**
「哪条通道是消光、哪条是温度/发射、哪条是烟/散射」是**渲染侧（材质）**的事，
本工具不做火焰假定——火只是它的一种用法。

用法::

    vdb_to_jpovvol.py <in.vdb | '%04d.vdb' 模式> <out.jpovvol> \
        --channel <grid>[:<scale>[:<norm>[:<gamma>]]]   # 可重复；顺序 = 通道 R,G,B
        [--start N --end N --ds N --unit M --z-up --rot <axis> --crop T --dtype u8|f32]

例子（篝火：R=火密度, G=温度代理, B=烟密度）::

    vdb_to_jpovvol.py '/tmp/cf/smallCampfire_%04d.vdb' campfire.jpovvol \
        --channel flames --channel flames --channel density:0.35 \
        --start 60 --end 140 --ds 2 --unit 0.01 --z-up

通道 <norm>（归一化口径，默认 `nzmax`）——**关键**：默认按「**非零体素**」统计，
避免在整块（多为 0 的）稀疏数组上取分位 → 除数过小 → 通道被**饱和**（历史 bug，
见 `docs/jpov_campfire_repro_plan.md` §颜色）：
    max        全局最大（含 0 体素）
    nzmax      非零体素的最大          （默认）
    p<X>       全体素的第 X 百分位      （如 p99.5）
    nzp<X>     非零体素的第 X 百分位    （如 nzp90）
"""
import argparse
import glob
import json
import os

import numpy as np
import pyopenvdb as vdb

import jpvol


def read_grid_dense(path, name, lo, size):
    """读一个 grid 到公共盒 [lo, lo+size) 的稠密数组 (nz,ny,nx)。缺该 grid → 全 0。"""
    arr = np.zeros((size[2], size[1], size[0]), dtype=np.float32)
    try:
        g = vdb.read(path, name)
    except Exception:
        return arr
    b = g.evalActiveVoxelBoundingBox()
    glo = np.array(b[0])
    gsize = np.array(b[1]) - glo + 1
    # ⚠️ pyopenvdb `copyToArray` 的 numpy 约定是 **array[i][j][k] = voxel(i,j,k)**，
    #    即**首个 numpy 轴是 x**（数组形状须为 (nx,ny,nz)），**不是** (nz,ny,nx)/[z][y][x]。
    #    若直接按 (nz,ny,nx) 分配，会 (a) 把 x 与 z 轴对调、(b) 在 nz>nx 时**静默截断 z 轴**
    #    （超出 nx 的 z 体素被丢弃）→ 体积“躺着 + 乱码”。故先按 (nx,ny,nz) 读，再 transpose 成 [z][y][x]。
    tmp_xyz = np.zeros((int(gsize[0]), int(gsize[1]), int(gsize[2])), dtype=np.float32)
    g.copyToArray(tmp_xyz, (int(glo[0]), int(glo[1]), int(glo[2])))
    tmp = np.ascontiguousarray(tmp_xyz.transpose(2, 1, 0))   # (x,y,z) -> (z,y,x)
    # 叠进公共盒
    oz, oy, ox = int(glo[2] - lo[2]), int(glo[1] - lo[1]), int(glo[0] - lo[0])
    z0, y0, x0 = max(0, oz), max(0, oy), max(0, ox)
    z1, y1, x1 = min(size[2], oz + gsize[2]), min(size[1], oy + gsize[1]), min(size[0], ox + gsize[0])
    if z1 > z0 and y1 > y0 and x1 > x0:
        arr[z0:z1, y0:y1, x0:x1] = tmp[z0 - oz:z1 - oz, y0 - oy:y1 - oy, x0 - ox:x1 - ox]
    return arr


def parse_channel(spec):
    """把 `<grid>[:<scale>[:<norm>[:<gamma>]]]` 解析成 (grid, scale, norm, gamma)。"""
    parts = spec.split(":")
    grid = parts[0]
    if not grid:
        raise ValueError("--channel 需要 grid 名：%r" % spec)
    scale = float(parts[1]) if len(parts) > 1 and parts[1] != "" else 1.0
    norm = parts[2] if len(parts) > 2 and parts[2] != "" else "nzmax"
    gamma = float(parts[3]) if len(parts) > 3 and parts[3] != "" else 1.0
    if gamma <= 0.0:
        raise ValueError("--channel gamma 必须 > 0：%r" % spec)
    return grid, scale, norm, gamma


def norm_divisor(a, norm):
    """算归一化除数。a 为 float 数组。空/全零 → 0（调用方据此跳过除法）。

    口径：max / nzmax / p<X>（全体素百分位）/ nzp<X>（非零体素百分位）。
    """
    if norm == "max":
        return float(a.max())
    if norm == "nzmax":
        nz = a[a > 0.0]
        return float(nz.max()) if nz.size else 0.0
    if norm.startswith("nzp"):
        nz = a[a > 0.0]
        return float(np.percentile(nz, float(norm[3:]))) if nz.size else 0.0
    if norm.startswith("p"):
        return float(np.percentile(a, float(norm[1:])))
    raise ValueError("未知 --channel norm 口径：%r（可选 max/nzmax/p<X>/nzp<X>）" % norm)


def resolve_files(inp, start, end):
    """输入是单个 .vdb 文件 → [该文件]；否则当作 `%04d` 模式 → glob 目录内全部 .vdb 再切片。"""
    if os.path.isfile(inp) and "%" not in inp:
        return [inp]
    files = sorted(glob.glob(os.path.join(os.path.dirname(inp), "*.vdb")))
    return files[start:end]


def main():
    ap = argparse.ArgumentParser(description="通用 OpenVDB → jpovvol 转换器")
    ap.add_argument("inp", help="单个 .vdb 文件，或 '%04d.vdb' 序列模式")
    ap.add_argument("out", help="输出 .jpovvol")
    ap.add_argument("--channel", action="append", default=[], required=True,
                    metavar="GRID[:SCALE[:NORM[:GAMMA]]]",
                    help="grid→通道映射，可重复（顺序 = R,G,B）；SCALE 乘性权重，"
                         "NORM 见文档（max/nzmax/p<X>/nzp<X>），GAMMA 幂曲线（<1 抬中低值）")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--end", type=int, default=10 ** 9)
    ap.add_argument("--ds", type=int, default=2, help="空间降采样倍数")
    ap.add_argument("--unit", type=float, default=0.01, help="1 世界单位=多少米（EmberGen 常为 cm）")
    ap.add_argument("--z-up", action="store_true", help="VDB 为 Z-up 时转成 Y-up")
    ap.add_argument("--rot", default="none",
                    choices=["none", "x90", "x-90", "y90", "y-90", "z90", "z-90"], help="额外整体旋转")
    ap.add_argument("--crop", type=float, default=0.0, help="紧裁剪阈值（>0 时按任一场的包围盒裁）")
    ap.add_argument("--dtype", choices=["u8", "f32"], default="u8")
    args = ap.parse_args()

    channels = [parse_channel(c) for c in args.channel]
    if not 1 <= len(channels) <= 3:
        raise SystemExit("通道数须为 1..3，收到 %d" % len(channels))
    grid_names = [c[0] for c in channels]

    files = resolve_files(args.inp, args.start, args.end)
    if not files:
        raise SystemExit("找不到 VDB 帧：%s" % args.inp)

    # 公共体素盒 = 所有通道 grid 在**所有帧**上的活动区并集。
    # （不能只取第一个 grid：其它 grid 落在其并集之外的体素会被 read_grid_dense 静默裁掉。）
    lo = None
    hi = None
    for name in grid_names:
        for f in files:
            try:
                g = vdb.read(f, name)
            except Exception:
                continue
            bb = g.evalActiveVoxelBoundingBox()
            b0 = np.asarray(bb[0], dtype=np.int64)
            b1 = np.asarray(bb[1], dtype=np.int64)
            lo = b0 if lo is None else np.minimum(lo, b0)
            hi = b1 if hi is None else np.maximum(hi, b1)
    if lo is None:
        raise SystemExit("在 %s 里读不到任何通道 grid %s" % (args.inp, grid_names))
    size = hi - lo + 1
    print("frames=%d  channels=%s  global bbox size (x,y,z)=%s"
          % (len(files), grid_names, size))

    # 逐通道读取（**通道外层循环**，任意时刻只驻一通道的逐帧数据 → 省内存；ds=1 全分辨率下
    # 若把所有通道同时堆在内存可达 GB 级，容器限额 6GB 会 OOM）。
    # 每帧：全分辨率读取 →（累计 crop 包围盒）→ **立即降采样** → 存；归一化推迟到整段读完。
    out_chans = []
    crop_lo = None
    crop_hi = None
    for ci, (grid, scale, norm, gamma) in enumerate(channels):
        per_frame = []
        for i, f in enumerate(files):
            a = read_grid_dense(f, grid, lo, size)
            if args.crop > 0.0:
                # crop 包围盒在**全分辨率**上累计（与「先裁后降采样」等价）。
                m = a > args.crop
                if m.any():
                    zs, ys, xs = np.where(m)
                    b_lo = np.array([zs.min(), ys.min(), xs.min()], dtype=np.int64)
                    b_hi = np.array([zs.max(), ys.max(), xs.max()], dtype=np.int64)
                    crop_lo = b_lo if crop_lo is None else np.minimum(crop_lo, b_lo)
                    crop_hi = b_hi if crop_hi is None else np.maximum(crop_hi, b_hi)
                del m
            per_frame.append(jpvol.downsample(a, args.ds))
            del a
            if ci == 0 and i % 25 == 0:
                print("  frame %d/%d" % (i, len(files)))

        # 各通道**独立归一化**（不同 grid 的绝对量纲可差几个数量级，flames≈1 vs density≈0.003），
        # 归一化口径由 --channel 的 norm 决定（默认按**非零体素**，避免稀疏数组上取分位致饱和）。
        arr = np.stack(per_frame)
        per_frame = None
        div = norm_divisor(arr, norm)
        if div <= 0.0:
            print("  [warn] 通道 grid=%s 全为零，跳过归一" % grid)
            div = 1.0
        nz = arr[arr > 0.0]
        print("  channel grid=%-12s norm=%-8s gamma=%.2f div=%.5g  nz[p50=%.4g p90=%.4g max=%.4g]"
              % (grid, norm, gamma, div,
                 np.percentile(nz, 50) if nz.size else 0.0,
                 np.percentile(nz, 90) if nz.size else 0.0,
                 nz.max() if nz.size else 0.0))
        del nz
        # 归一 → [0,1]；gamma 曲线（<1 抬升中低值，用于把偏低的场拉满动态范围，
        # 如温度通道：flames 场分布偏低，线性映射会让大部分体素落到黑体下限）。
        # **全程 in-place**：ds=1 全分辨率下一个 f32 副本就 ~1GB，容器内存紧。
        if arr.dtype != np.float32:
            arr = arr.astype(np.float32)
        arr /= div
        np.clip(arr, 0.0, 1.0, out=arr)
        if gamma != 1.0:
            np.power(arr, gamma, out=arr)
        if scale != 1.0:
            arr *= scale
        np.clip(arr, 0.0, 1.0, out=arr)
        # u8 输出：**立即量化**，不再保留 f32 副本（大体积下省一半内存）。
        if args.dtype == "u8":
            arr *= 255.0
            arr += 0.5
            out_chans.append(arr.astype(np.uint8))
        else:
            out_chans.append(arr)
        del arr

    if args.crop > 0.0 and crop_lo is not None:
        # 全分辨率包围盒 → 降采样下标（降采样网格 i 覆盖全分辨率 [i*ds, (i+1)*ds)）。
        z0, y0, x0 = (int(v) for v in crop_lo // args.ds)
        z1, y1, x1 = (int(v) for v in crop_hi // args.ds + 1)
        print("  crop -> z[%d,%d) y[%d,%d) x[%d,%d)" % (z0, z1, y0, y1, x0, x1))
        out_chans = [a[:, z0:z1, y0:y1, x0:x1] for a in out_chans]

    # 注意：旋转必须放在 **降采样之后**（downsample 假定末尾三轴是 (z,y,x)，
    # 提前转会把轴揉在一起 → 乱码）。世界轴：x=axis3, y=axis2, z=axis1。
    rot_perm = {
        "x90": (0, 2, 1, 3), "x-90": (0, 2, 1, 3),   # 绕 X：交换 y,z
        "y90": (0, 3, 2, 1), "y-90": (0, 3, 2, 1),   # 绕 Y：交换 x,z
        "z90": (0, 1, 3, 2), "z-90": (0, 1, 3, 2),   # 绕 Z：交换 x,y
    }
    rot_flip = {"x90": None, "x-90": 1, "y90": None, "y-90": 3,
                "z90": None, "z-90": 1}
    if args.z_up:
        out_chans = [a.transpose(0, 2, 1, 3) for a in out_chans]
        print("  z_up applied")
    if args.rot != "none":
        f = rot_flip[args.rot]
        rotated = []
        for a in out_chans:
            a = a.transpose(rot_perm[args.rot])
            if f is not None:
                a = np.flip(a, axis=f)
            rotated.append(a)
        out_chans = rotated
        print("  rot=%s" % args.rot)

    dens = out_chans[0]
    temp = out_chans[1] if len(out_chans) > 1 else np.zeros_like(dens)
    smoke = out_chans[2] if len(out_chans) > 2 else None
    X, Y, Z, F, nbytes = jpvol.write_jpovvol(
        args.out, dens, temp, smoke=smoke, dtype=args.dtype,
        pre_quantized=(args.dtype == "u8"))
    print("wrote %s dims=(%d,%d,%d) frames=%d channels=%d dtype=%s bytes=%d"
          % (args.out, X, Y, Z, F, 3 if smoke is not None else 2, args.dtype, nbytes))

    # sidecar：记录真实物理尺度（voxel_m × dims = 世界盒），供查看器 1:1 摆放。
    voxel_m = args.unit * args.ds
    side = {
        "dims": [X, Y, Z], "frames": F, "voxel_m": voxel_m,
        "channels": 3 if smoke is not None else 2,
        "world_size_m": [X * voxel_m, Y * voxel_m, Z * voxel_m],
        "segments": {"ignite": [0, 0], "sustain": [0, F], "extinct": [F, F]},
        "loopable": True, "emission_axis": "+Y", "note": "vdb",
    }
    with open(args.out + ".json", "w") as fp:
        json.dump(side, fp, indent=2)
    print("  world_size_m = %.3f x %.3f x %.3f (voxel_m=%.4f)"
          % tuple(side["world_size_m"] + [voxel_m]))


if __name__ == "__main__":
    main()
