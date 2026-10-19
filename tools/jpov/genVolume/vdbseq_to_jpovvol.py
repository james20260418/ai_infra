#!/usr/bin/env python3
"""EmberGen VDB 序列 → JPVOL2（R=density, G=flames→色彩驱动），含 Z-up→Y-up 旋转。

JangaFX free VDBs（CC0）示例：Small Camp Fire。
读入 smallCampfire_%04d.vdb，取 density/flames 两格 → 公共稠密盒 → 旋转(z→y) →
下采样 → 归一化 → 写 .jpovvol。

用法：
  python3 vdbseq_to_jpovvol.py <dir_pattern> <out.jpovvol> [--start N --end N --ds 2 --no-rot]
    dir_pattern 例：/tmp/vdbtry/smallCampfire/smallCampfireVDB/smallCampfire_%04d.vdb
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
    #    （超出 nx 的 z 体素被丢弃）→ 火“躺着 + 乱码”。故先按 (nx,ny,nz) 读，再 transpose 成 [z][y][x]。
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("out")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--end", type=int, default=10 ** 9)
    ap.add_argument("--ds", type=int, default=2)
    ap.add_argument("--r-grid", default="density", help="消光/散射 channel 用的 grid（官方：density）")
    ap.add_argument("--g-grid", default="flames", help="当温度/发射用的 grid（官方：flames）")
    ap.add_argument("--gnorm", type=float, default=0.0, help="flames 按该百分位归一（0=用 max）；如 99.5 使核心→亮")
    ap.add_argument("--g-lo", type=float, default=0.0, help="flames 重映射下限（拉对比，如 0.10）")
    ap.add_argument("--g-hi", type=float, default=0.0, help="flames 重映射上限（拉对比，如 0.38）")
    ap.add_argument("--crop", type=float, default=0.0, help="紧裁剪阈值（>0 时按 dens|flames 的包围盒裁）")
    ap.add_argument("--unit", type=float, default=0.01, help="1 世界单位=多少米（EmberGen 常为 cm；默认 0.01）")
    ap.add_argument("--z-up", action="store_true", help="VDB 为 Z-up（火沿 +z）时加此选项转成 Y-up；默认不转（很多序列已是 Y-up）")
    ap.add_argument("--rot", default="none", choices=["none", "x90", "x-90", "y90", "y-90", "z90", "z-90"], help="额外整体旋转")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(os.path.dirname(args.pattern), "*.vdb")))
    files = files[args.start:args.end]
    if not files:
        raise SystemExit("no vdb frames for pattern " + args.pattern)

    # 公共体素盒 = r_grid 与 g_grid 在**所有帧**上的活动区并集。
    # （不能只取 r_grid：若 g_grid 有体素落在 r_grid 并集之外，会被 read_grid_dense 静默裁掉。）
    lo = None
    hi = None
    for name in (args.r_grid, args.g_grid):
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
        raise SystemExit("no readable grids in " + args.pattern)
    size = hi - lo + 1
    print("frames=%d  global bbox size (x,y,z)=%s" % (len(files), size))

    dens, flam = [], []
    for i, f in enumerate(files):
        d = read_grid_dense(f, args.r_grid, lo, size)
        fl = read_grid_dense(f, args.g_grid, lo, size)
        dens.append(d)
        flam.append(fl)
        if i % 25 == 0:
            print("  frame %d/%d densmax=%.4f flamemax=%.4f" % (i, len(files), d.max(), fl.max()))

    dens = np.stack(dens)   # [F][z][y][x]
    flam = np.stack(flam)

    if args.crop > 0.0:
        m = (dens.max(axis=0) > args.crop) | (flam.max(axis=0) > args.crop)
        zs, ys, xs = np.where(m)
        z0, z1 = int(zs.min()), int(zs.max()) + 1
        y0, y1 = int(ys.min()), int(ys.max()) + 1
        x0, x1 = int(xs.min()), int(xs.max()) + 1
        print("  crop -> z[%d,%d) y[%d,%d) x[%d,%d)" % (z0, z1, y0, y1, x0, x1))
        dens = dens[:, z0:z1, y0:y1, x0:x1]
        flam = flam[:, z0:z1, y0:y1, x0:x1]

    # 注意：旋转必须放在 **降采样之后**（downsample 假定最后三轴是 (z,y,x)，
    # 提前转会把轴揉在一起 → 乱码）。
    dens = jpvol.downsample(dens, args.ds)
    flam = jpvol.downsample(flam, args.ds)

    # 旋转（在降采样之后）：对 [F,z,y,x] 做轴置换 + 翻转，保持合法体积。
    # 世界轴：x=axis3, y=axis2, z=axis1。
    rot_perm = {
        "x90": (0, 2, 1, 3), "x-90": (0, 2, 1, 3),   # 绕 X：交换 y,z
        "y90": (0, 3, 2, 1), "y-90": (0, 3, 2, 1),   # 绕 Y：交换 x,z
        "z90": (0, 1, 3, 2), "z-90": (0, 1, 3, 2),   # 绕 Z：交换 x,y
    }
    rot_flip = {"x90": None, "x-90": 1, "y90": None, "y-90": 3,
                "z90": None, "z-90": 1}
    if args.z_up:
        dens = dens.transpose(0, 2, 1, 3)
        flam = flam.transpose(0, 2, 1, 3)
        print("  z_up applied")
    if args.rot != "none":
        dens = dens.transpose(rot_perm[args.rot])
        flam = flam.transpose(rot_perm[args.rot])
        f = rot_flip[args.rot]
        if f is not None:
            dens = np.flip(dens, axis=f)
            flam = np.flip(flam, axis=f)
        print("  rot=%s" % args.rot)

    dmax = float(dens.max()) or 1.0
    if args.gnorm > 0.0:
        fmax = float(np.percentile(flam, args.gnorm)) or 1.0
    else:
        fmax = float(flam.max()) or 1.0
    dn = np.clip(dens / dmax, 0, 1)
    if args.g_hi > args.g_lo > 0.0:
        fn = np.clip((flam - args.g_lo) / (args.g_hi - args.g_lo), 0, 1)
    else:
        fn = np.clip(flam / fmax, 0, 1)
    X, Y, Z, F, nbytes = jpvol.write_jpovvol(args.out, dn, fn, dtype="u8")
    print("wrote %s dims=(%d,%d,%d) frames=%d bytes=%d" % (args.out, X, Y, Z, F, nbytes))

    # sidecar：记录真实物理尺度（voxel_m × dims = 世界盒），供查看器 1:1 摆放。
    voxel_m = args.unit * args.ds
    side = {
        "dims": [X, Y, Z], "frames": F, "voxel_m": voxel_m,
        "world_size_m": [X * voxel_m, Y * voxel_m, Z * voxel_m],
        "segments": {"ignite": [0, 0], "sustain": [0, F], "extinct": [F, F]},
        "loopable": True, "emission_axis": "+Y", "note": "vdbseq",
    }
    with open(args.out + ".json", "w") as fp:
        json.dump(side, fp, indent=2)
    print("  world_size_m = %.3f x %.3f x %.3f (voxel_m=%.4f)" % tuple(side["world_size_m"] + [voxel_m]))


if __name__ == "__main__":
    main()
