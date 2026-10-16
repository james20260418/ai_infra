#!/usr/bin/env python3
"""EmberGen VDB 序列 → JPVOL2（R=消光场, G=温度代理），含 Z-up→Y-up 旋转。

JangaFX free VDBs（CC0）示例：Small Camp Fire。
读入 smallCampfire_%04d.vdb，取 density/flames 两格 → 公共稠密盒 → 旋转(z→y) →
下采样 → 归一化 → 写 .jpovvol。

支持把**烟单独放第 3 通道**：`--b-grid density` ⇒ 写 3 通道
（R=火密度、G=温度、B=烟密度）。渲染侧据此把「火」（自发光）与「烟」
（只吸收 + 受光散射）分开处理；各场**独立归一**（flames≈1 vs density≈0.003，
不归一的话烟会被淹没）。

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
    ap.add_argument("--r-grid", default="density", help="R（消光/火密度）用的 grid（官方：flames）")
    ap.add_argument("--b-grid", default="",
                    help="第 3 通道（B=烟密度）用的 grid（官方：density）；空=不写第 3 通道（2 通道）")
    ap.add_argument("--b-scale", type=float, default=1.0, help="--b-grid 的强度权重（烟的浓淡）")
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

    # 公共体素盒 = 所有参与 grid 在**所有帧**上的活动区并集。
    # （不能只取 r_grid：若其它 grid 有体素落在其并集之外，会被 read_grid_dense 静默裁掉。）
    grid_names = [args.r_grid, args.g_grid]
    if args.b_grid:
        grid_names.append(args.b_grid)
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
        raise SystemExit("no readable grids in " + args.pattern)
    size = hi - lo + 1
    print("frames=%d  global bbox size (x,y,z)=%s" % (len(files), size))

    # 逐帧读取 →（全分辨率下累计 crop 包围盒）→ **立即降采样** → 存。
    # 逐帧降采样是为了**省内存**：整段全分辨率堆叠在大 VDB 盒上可达 GB 级（本机容器限额 6GB，曾 OOM）。
    # 三个场分开存；归一化推迟到最后（需整段的全局 max，且各场**独立**归一）。
    r_frames, b_frames, g_frames = [], [], []
    rmax = 0.0
    bmax = 0.0
    crop_lo = None
    crop_hi = None
    for i, f in enumerate(files):
        d = read_grid_dense(f, args.r_grid, lo, size)
        fl = read_grid_dense(f, args.g_grid, lo, size)
        s = None
        if args.b_grid:
            s = read_grid_dense(f, args.b_grid, lo, size)
            bmax = max(bmax, float(s.max()))
        rmax = max(rmax, float(d.max()))
        if args.crop > 0.0:
            # crop 包围盒在**全分辨率**上累计（与「先裁后降采样」等价）。
            m = (d > args.crop) | (fl > args.crop)
            if s is not None:
                m = m | (s > args.crop)
            if m.any():
                zs, ys, xs = np.where(m)
                b_lo = np.array([zs.min(), ys.min(), xs.min()], dtype=np.int64)
                b_hi = np.array([zs.max(), ys.max(), xs.max()], dtype=np.int64)
                crop_lo = b_lo if crop_lo is None else np.minimum(crop_lo, b_lo)
                crop_hi = b_hi if crop_hi is None else np.maximum(crop_hi, b_hi)
            del m
        r_frames.append(jpvol.downsample(d, args.ds))
        if s is not None:
            b_frames.append(jpvol.downsample(s, args.ds))
        g_frames.append(jpvol.downsample(fl, args.ds))
        if i % 25 == 0:
            print("  frame %d/%d rmax=%.4f bmax=%.4f gmax=%.4f"
                  % (i, len(files), d.max(), (s.max() if s is not None else 0.0), fl.max()))

    # 各场**独立归一**再存：不同 grid 的绝对量纲可差几个数量级（flames≈1 vs density≈0.003），
    # 不各自归一的话，小量纲的场（往往是烟）会被淹没。
    dens = np.stack(r_frames) / max(rmax, 1e-9)      # [F][z][y][x]（已降采样 + 归一）
    flam = np.stack(g_frames)
    smoke = (np.stack(b_frames) / max(bmax, 1e-9)) * args.b_scale if b_frames else None

    if args.crop > 0.0 and crop_lo is not None:
        # 全分辨率包围盒 → 降采样下标（降采样网格 i 覆盖全分辨率 [i*ds, (i+1)*ds)）。
        z0, y0, x0 = (int(v) for v in crop_lo // args.ds)
        z1, y1, x1 = (int(v) for v in crop_hi // args.ds + 1)
        print("  crop -> z[%d,%d) y[%d,%d) x[%d,%d)" % (z0, z1, y0, y1, x0, x1))
        dens = dens[:, z0:z1, y0:y1, x0:x1]
        flam = flam[:, z0:z1, y0:y1, x0:x1]

    # 注意：旋转必须放在 **降采样之后**（downsample 假定最后三轴是 (z,y,x)，
    # 提前转会把轴揉在一起 → 乱码）。

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
        if smoke is not None:
            smoke = smoke.transpose(0, 2, 1, 3)
        print("  z_up applied")
    if args.rot != "none":
        dens = dens.transpose(rot_perm[args.rot])
        flam = flam.transpose(rot_perm[args.rot])
        f = rot_flip[args.rot]
        if f is not None:
            dens = np.flip(dens, axis=f)
            flam = np.flip(flam, axis=f)
        if smoke is not None:
            smoke = smoke.transpose(rot_perm[args.rot])
            if f is not None:
                smoke = np.flip(smoke, axis=f)
        print("  rot=%s" % args.rot)

    if args.gnorm > 0.0:
        fmax = float(np.percentile(flam, args.gnorm)) or 1.0
    else:
        fmax = float(flam.max()) or 1.0
    dn = np.clip(dens, 0, 1)
    if args.g_hi > args.g_lo > 0.0:
        fn = np.clip((flam - args.g_lo) / (args.g_hi - args.g_lo), 0, 1)
    else:
        fn = np.clip(flam / fmax, 0, 1)
    sn = np.clip(smoke, 0, 1) if smoke is not None else None
    X, Y, Z, F, nbytes = jpvol.write_jpovvol(args.out, dn, fn, smoke=sn, dtype="u8")
    print("wrote %s dims=(%d,%d,%d) frames=%d channels=%d bytes=%d"
          % (args.out, X, Y, Z, F, 3 if sn is not None else 2, nbytes))

    # sidecar：记录真实物理尺度（voxel_m × dims = 世界盒），供查看器 1:1 摆放。
    voxel_m = args.unit * args.ds
    side = {
        "dims": [X, Y, Z], "frames": F, "voxel_m": voxel_m,
        "channels": 3 if sn is not None else 2,
        "world_size_m": [X * voxel_m, Y * voxel_m, Z * voxel_m],
        "segments": {"ignite": [0, 0], "sustain": [0, F], "extinct": [F, F]},
        "loopable": True, "emission_axis": "+Y", "note": "vdbseq",
    }
    with open(args.out + ".json", "w") as fp:
        json.dump(side, fp, indent=2)
    print("  world_size_m = %.3f x %.3f x %.3f (voxel_m=%.4f)" % tuple(side["world_size_m"] + [voxel_m]))


if __name__ == "__main__":
    main()
