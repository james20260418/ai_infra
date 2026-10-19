#!/usr/bin/env python3
"""火焰「原型族」生命周期烘焙：main(点火+持续) + ext(熄灭) → 单条 .jpovvol + sidecar。

生命周期（Danis 2026-10-15 定）：
  点火 0→1  |  持续 1→1（**真循环**）  |  熄灭 1→0

**无跳变**保证：
  · 点火段末帧 == 持续段首帧 == 熄灭段首帧（**同一检查点状态**）——
    所以「点火切循环」「循环切熄灭」都无跳变。
  · 持续段用 **环闭合工具**（--loop）做成**无缝循环**。

环闭合工具（--loop，业界标准 "offset + cross-dissolve"，= Houdini GameTools「Make Loop」思路）：
  取持续窗 S[0..N-1]，输出 L[t] = (1-c[t])·S[t] + c[t]·S[(t+N//2) % N]，
  c[t] = 0.5 + 0.5·cos(2π t/N)  （c[0]=c[N]=1 ⇒ **接缝处只显示"内部"帧 S[N/2]，缝被抹掉**）。
  代价：轻微重影（对混沌火可接受）。--loop none 则不做（保留原样，仅供对比）。

用法：
  python3 tools/fire_family_bake.py --sim /tmp/fam2_pool --out assets/prototypes/pool.jpovvol \
      --scale 2 --window 0.18 --loop half-xfade
"""
import argparse
import glob
import json
import os

import numpy as np

import jpvol


def load_dir(d):
    fs = sorted(glob.glob(os.path.join(d, "f*.npz")))
    dens, temp = [], []
    for f in fs:
        z = np.load(f)
        dens.append(z["density"].astype(np.float32))
        temp.append(z["heat"].astype(np.float32))
    if not fs:
        raise SystemExit("no npz in " + d)
    return np.stack(dens), np.stack(temp)   # [F][z][y][x]


def pick_best_window(dens, temp, a, nmax, navail, nmin=16):
    """扫描循环长度 N ∈ [nmin, nmax]，使 state[a+N] 最接近 state[a]（真循环）。
    返回 (best_n, residual, internal_mean)。subsample 加速。"""
    ds = dens[:, ::3, ::3, ::3]
    ts = temp[:, ::3, ::3, ::3]
    ref_d, ref_t = ds[a], ts[a]
    hi = min(nmax, navail - 1 - a)
    # 内部帧平均差（做基准）
    internal = float(np.abs(np.diff(ds[a:a + hi + 1], axis=0)).mean()
                     + np.abs(np.diff(ts[a:a + hi + 1], axis=0)).mean())
    best_n, best_v = nmin, 1e18
    for n in range(nmin, hi + 1):
        v = float(np.abs(ds[a + n] - ref_d).mean() + np.abs(ts[a + n] - ref_t).mean())
        if v < best_v:
            best_v, best_n = v, n
    return best_n, best_v, internal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True, help="仿真输出目录（含 main/ ext/ meta.json）")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--window", type=float, default=0.18)
    ap.add_argument("--loop", choices=["best", "half-xfade", "none"], default="best")
    ap.add_argument("--loop-frames", type=int, default=32,
                    help="half-xfade 的循环长度 N（持续段）；会向窗口起点回退 N/2")
    ap.add_argument("--dtype", choices=["u8", "f32"], default="u8")
    args = ap.parse_args()

    meta = json.load(open(os.path.join(args.sim, "meta.json")))
    S0 = meta["S0"]
    NEXT = meta["NEXT"]
    NMAX = int(meta.get("NMAX", 128))

    md, mt = load_dir(os.path.join(args.sim, "main"))   # 点火+持续
    ed, et = load_dir(os.path.join(args.sim, "ext"))    # 熄灭
    navail = md.shape[0]
    a0 = S0 - 1                                          # md[a0] = state[S0] = 循环入口

    if args.loop == "best":
        N, resid, internal = pick_best_window(md, mt, a0, NMAX, navail)
        w0 = a0
        print("  [loop=best] N=%d  residual=%.5f  internal=%.5f  ratio=%.2f"
              % (N, resid, internal, resid / max(1e-9, internal)))
    elif args.loop == "half-xfade":
        N = args.loop_frames
        w0 = a0 - N // 2     # 关键：窗口回退 N/2 ⇒ L[0] 内容 == state[S0]（与检查点对齐）
    else:
        N = NMAX
        w0 = a0
    if w0 < 0:
        w0 = 0

    # 段切片：md 的 dump 约定 md[j]=state[j+1]；ext dump[j]=从检查点(状态[S0])起第 j 帧。
    ig_d, ig_t = md[0:a0], mt[0:a0]                       # 点火：末帧 = state[S0-1]
    su_d, su_t = md[w0:w0 + N], mt[w0:w0 + N]             # 持续（循环窗）
    ex_d, ex_t = ed, et                                   # 熄灭：首帧 = state[S0] = 检查点
    n_ig, n_su, n_ex = ig_d.shape[0], su_d.shape[0], ex_d.shape[0]

    if args.loop == "half-xfade":
        su_d = jpvol.loop_half_xfade(su_d)
        su_t = jpvol.loop_half_xfade(su_t)

    # 拼接：点火 | 持续(循环) | 熄灭
    dens = np.concatenate([ig_d, su_d, ex_d], axis=0)
    temp = np.concatenate([ig_t, su_t, ex_t], axis=0)

    dens = jpvol.downsample(dens, args.scale)
    temp = jpvol.downsample(temp, args.scale)
    dens, temp = jpvol.apply_window(dens, temp, args.window)

    dmax = float(dens.max()) or 1.0
    tmax = float(temp.max()) or 1.0
    dens = np.clip(dens / dmax, 0.0, 1.0)
    temp = np.clip(temp / tmax, 0.0, 1.0)
    X, Y, Z, F, nbytes = jpvol.write_jpovvol(args.out, dens, temp, dtype=args.dtype)

    # sidecar：分段帧窗（供运行时：点火播一次 → 循环 loop 段 → 熄灭播一次）
    a0, a1 = 0, n_ig
    b0, b1 = n_ig, n_ig + n_su
    c0, c1 = n_ig + n_su, n_ig + n_su + n_ex
    sidecar = {
        "family": meta["family"],
        "dims": [X, Y, Z],
        "frames": F,
        "loopable": args.loop != "none",
        "loop_mode": args.loop,
        "loop_frames": n_su,
        "emission_axis": "+Y",
        "segments": {
            "ignite": [a0, a1],     # 0→1（播一次）
            "sustain": [b0, b1],    # 1→1（循环此窗；首帧==点火末帧的下一个连续态）
            "extinct": [c0, c1],    # 1→0（播一次；首帧==循环入口态）
        },
        "note": meta.get("note", ""),
    }
    with open(args.out + ".json", "w") as f:
        json.dump(sidecar, f, indent=2, ensure_ascii=False)

    print("wrote %s dims=(%d,%d,%d) frames=%d dtype=%s bytes=%d" %
          (args.out, X, Y, Z, F, args.dtype, nbytes))
    print("  segments: ignite[%d,%d] sustain[%d,%d] extinct[%d,%d] loop=%s" %
          (a0, a1, b0, b1, c0, c1, args.loop))


if __name__ == "__main__":
    main()
