#!/usr/bin/env python3
"""VDB -> JPVOL1 体积转换器（fire_fog 的共享 3D 火纹理）。

把一个（可含多 grid 的）OpenVDB 文件烘成引擎可读的**稠密 3D 体积文件** `.jpovvol`：

  header (32 bytes, little-endian):
    char   magic[8]   = "JPVOL1\\0\\0"
    uint32 version    = 1
    uint32 nx, ny, nz          // 体素维数（各 > 0）
    uint32 channels            // 通道数（本版固定 2：R=density, G=temperature）
    uint32 dtype               // 0 = float32
    uint32 reserved            // 0
  data: nx*ny*nz*channels 个 float32
        体素序：x 最快，然后 y，然后 z；每体素内通道按 r,g,... 交错。
        ⇒ 与 glTexImage3D(GL_RG*, ..., data) 的期望布局一致（可直接上传 3D 纹理）。

通道语义（归一化到 [0,1]）：
  R = density（-> 消光 σ = density_scale · R）
  G = temperature（-> 自发光 = emission_color · G · temp_scale）

用法：
  python3 vdb_to_jpovvol.py <in.vdb> <out.jpovvol> [--density-grid density] [--temp-grid temperature]
"""
import argparse
import sys

import numpy as np
import pyopenvdb as vdb

import jpvol


def grid_to_dense(grid, common_lo, common_hi):
    """把 grid 的活动区数据铺进全局索引盒 [common_lo, common_hi) 的稠密数组。

    仅在 grid 自身的活动 bbox 内填值，其余留 0（该处无场）。
    返回 float32 稠密数组（形如 (nz, ny, nx)）。
    """
    b = grid.evalActiveVoxelBoundingBox()
    lo = np.array([b[0][0], b[0][1], b[0][2]], dtype=np.int64)
    hi = np.array([b[1][0], b[1][1], b[1][2]], dtype=np.int64)
    # ⚠️ pyopenvdb copyToArray 约定：array[i][j][k] = voxel(i,j,k)，**首轴为 x** → 形状须 (nx,ny,nz)。
    # 旧代码按 (nz,ny,nx) 分配 → x/z 对调 + （nz>nx 时）静默截断 z 轴。
    arr_xyz = np.zeros((int(hi[0] - lo[0] + 1), int(hi[1] - lo[1] + 1),
                        int(hi[2] - lo[2] + 1)), dtype=np.float32)
    grid.copyToArray(arr_xyz, (int(lo[0]), int(lo[1]), int(lo[2])))
    arr = np.ascontiguousarray(arr_xyz.transpose(2, 1, 0))   # (x,y,z) -> (z,y,x)

    out = np.zeros((common_hi[2] - common_lo[2], common_hi[1] - common_lo[1],
                    common_hi[0] - common_lo[0]), dtype=np.float32)
    oz = int(lo[2] - common_lo[2])
    oy = int(lo[1] - common_lo[1])
    ox = int(lo[0] - common_lo[0])
    out[oz:oz + arr.shape[0], oy:oy + arr.shape[1], ox:ox + arr.shape[2]] = arr
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("in_vdb")
    ap.add_argument("out_jpovvol")
    ap.add_argument("--density-grid", default="density")
    ap.add_argument("--temp-grid", default="temperature")
    args = ap.parse_args()

    grids = vdb.readAll(args.in_vdb)[0]
    by_name = {g.name: g for g in grids}
    print("grids:", [(g.name,) for g in grids])
    for g in grids:
        b = g.evalActiveVoxelBoundingBox()
        print(f"  {g.name}: bbox={b}  voxel_size={g.transform.voxelSize()}")

    if args.density_grid not in by_name:
        sys.exit(f"missing density grid '{args.density_grid}' (have {list(by_name)})")
    gden = by_name[args.density_grid]
    gtmp = by_name.get(args.temp_grid)   # 允许缺（则温度全 0）

    # 公共索引盒 = 各 grid 活动 bbox 的并集（不做重采样：两 grid 共用索引系）。
    los = []
    his = []
    for g in (gden, gtmp):
        if g is None:
            continue
        b = g.evalActiveVoxelBoundingBox()
        los.append([b[0][0], b[0][1], b[0][2]])
        his.append([b[1][0] + 1, b[1][1] + 1, b[1][2] + 1])
    lo = np.min(np.array(los), axis=0).astype(np.int64)
    hi = np.max(np.array(his), axis=0).astype(np.int64)
    nx, ny, nz = int(hi[0] - lo[0]), int(hi[1] - lo[1]), int(hi[2] - lo[2])
    print(f"common index box: lo={tuple(lo)} hi={tuple(hi)}  dims=({nx},{ny},{nz})")

    den = grid_to_dense(gden, lo, hi)
    tmp = grid_to_dense(gtmp, lo, hi) if gtmp is not None \
        else np.zeros_like(den)

    # 归一化到 [0,1]（记录原始峰值供参考）。
    den_max = float(den.max()) if den.size else 0.0
    tmp_max = float(tmp.max()) if tmp.size else 0.0
    if den_max > 0:
        den = den / den_max
    if tmp_max > 0:
        tmp = tmp / tmp_max
    den = np.clip(den, 0.0, 1.0)
    tmp = np.clip(tmp, 0.0, 1.0)
    print(f"normalized: density_max={den_max:.4g}  temperature_max={tmp_max:.4g}")

    X, Y, Z, nbytes = jpvol.write_jpovvol1(args.out_jpovvol, den, tmp)
    print(f"wrote {args.out_jpovvol}: dims=({X},{Y},{Z}) channels=2 float32 bytes={nbytes}")


if __name__ == "__main__":
    main()
