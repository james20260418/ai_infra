#!/usr/bin/env python3
"""动捕拟合：MediaPipe track -> Mixamo23 每帧 pose（三种算法可切换）。

算法（--algo）：
  worldz   直接用 MediaPipe 的 world landmarks（33 点近似米制 3D）算骨方向。
           它的 z 是网络回归出来的，噪声大；正对镜头/深度方向动作最容易错。
  lift2d   只用 2D 关键点 + 骨长先验反解深度（透视缩短）：
             2D 长度 u 已知，骨长 L 已知 => 出平面分量 sqrt(L²-u²)，
             符号由 world z 的差值给。深度方向"量"是几何推出来的，不是网络猜的。
  lift2d_t 在 lift2d 上再加：2D one-euro/savgol 平滑 + 深度符号时序一致（中值）+ 深度平滑。
            代价是响应变慢，收益是抖动小。

三种都走同一个「swing-only 拟合」（mixamo23.pose_from_dirs），差异只在**方向场**，
这样对比的是算法本身的差别，不掺 retarget 的差异。

输出（--out）：poses json
  { "algo":..., "fps":..., "bone_count":23,
    "frames":[ {"t":..., "quat":[ [x,y,z,w] x23 ], "root":[x,y,z] } ],
    "metrics": {...} }
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mixamo23 as mk

# ---------- MediaPipe 33 点 ----------
NOSE, LEAR, REAR = 0, 7, 8
LSHO, RSHO, LELB, RELB, LWRI, RWRI = 11, 12, 13, 14, 15, 16
LPINK, RPINK, LIDX, RIDX = 17, 18, 19, 20
LHIP, RHIP, LKNEE, RKNEE, LANK, RANK = 23, 24, 25, 26, 27, 28
LHEEL, RHEEL, LFOOT, RFOOT = 29, 30, 31, 32

# 骨 -> (父点, 子点, 该段的语义长度来源)
# 虚拟点用负数编码（见 point()）：-1 hip_center, -2 shoulder_center, -3 head_center, -4 neck_top
SEG = {
    "mixamorig:Hips":         (-1, -2),
    "mixamorig:Spine":        (-1, -2),
    "mixamorig:Spine1":       (-1, -2),
    "mixamorig:Spine2":       (-1, -2),
    "mixamorig:Neck":         (-2, -3),
    "mixamorig:Head":         (-3, -5),
    "mixamorig:LeftShoulder": (-2, LSHO),
    "mixamorig:LeftArm":      (LSHO, LELB),
    "mixamorig:LeftForeArm":  (LELB, LWRI),
    "mixamorig:LeftHand":     (LWRI, -6),
    "mixamorig:RightShoulder": (-2, RSHO),
    "mixamorig:RightArm":     (RSHO, RELB),
    "mixamorig:RightForeArm": (RELB, RWRI),
    "mixamorig:RightHand":    (RWRI, -7),
    "mixamorig:LeftUpLeg":    (LHIP, LKNEE),
    "mixamorig:LeftLeg":      (LKNEE, LANK),
    "mixamorig:LeftFoot":     (LANK, LFOOT),
    "mixamorig:LeftToeBase":  (LHEEL, LFOOT),
    "mixamorig:RightUpLeg":   (RHIP, RKNEE),
    "mixamorig:RightLeg":     (RKNEE, RANK),
    "mixamorig:RightFoot":    (RANK, RFOOT),
    "mixamorig:RightToeBase": (RHEEL, RFOOT),
}


def point(pts, idx):
    """取一个点（支持虚拟点）。pts: (33,3) 或 (33,2)"""
    if idx >= 0:
        return pts[idx]
    if idx == -1:
        return 0.5 * (pts[LHIP] + pts[RHIP])
    if idx == -2:
        return 0.5 * (pts[LSHO] + pts[RSHO])
    if idx == -3:
        return 0.5 * (pts[LEAR] + pts[REAR])
    if idx == -5:
        # 头顶：头心沿躯干轴再往上（无头顶点，用耳朵到肩的方向延长）
        hc = 0.5 * (pts[LEAR] + pts[REAR])
        sc = 0.5 * (pts[LSHO] + pts[RSHO])
        return hc + (hc - sc) * 0.8
    if idx == -6:
        return 0.5 * (pts[LIDX] + pts[LPINK])
    if idx == -7:
        return 0.5 * (pts[RIDX] + pts[RPINK])
    raise ValueError(idx)


def to_world(rec):
    """world landmarks -> Y-up（x 右, y 上, z 朝相机）"""
    w = np.array([[p["x"], -p["y"], p["z"]] for p in rec["world"]], dtype=np.float64)
    return w


def to_px(rec):
    """2D landmarks -> y 向上（与 world 同向，便于同一套相似变换）"""
    l = np.array([[p["x"], -p["y"]] for p in rec["landmarks"]], dtype=np.float64)
    return l


def _kernel(win=5, sigma=None):
    """高斯核（win 奇数）。用高斯而非 Savitzky-Golay：动捕去抖要的就是低通，
    而且不引 scipy 依赖（本工具的依赖只有 numpy）——代价是峰值略有削平，实验阶段可接受。"""
    if win % 2 == 0:
        win += 1
    if sigma is None:
        sigma = max(1.0, win / 6.0)
    r = win // 2
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    return k / k.sum()


def smooth_zero_phase(x, win=7):
    """零相位平滑：正反各滤一次 = 无时间滞后（动捕里滞后比噪声更致命）。

    普通滑动平均会把动作整体推后 (win-1)/2 帧；24fps 下 7 帧窗 = 0.13s 的滞后，
    足够让"拉满弓"和"刚起手"错位。正反滤波把相位抵消。
    """
    return savgol(savgol(x, win)[::-1], win)[::-1]


def savgol(x, win=7, poly=2):
    """名字沿用调用点：实际是边缘复制的 1D 高斯平滑（逐列）。"""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < 3:
        return x
    k = _kernel(win)
    r = len(k) // 2
    pad = np.pad(x, ((r, r),) + ((0, 0),) * (x.ndim - 1), mode="edge")
    out = np.zeros_like(x)
    for i in range(len(k)):
        out += k[i] * pad[i:i + len(x)]
    return out


def medfilt(x, k=5):
    x = np.asarray(x, dtype=np.float64)
    if len(x) < k or k < 3:
        return x
    r = k // 2
    pad = np.pad(x, ((r, r),) + ((0, 0),) * (x.ndim - 1), mode="edge")
    stack = np.stack([pad[i:i + len(x)] for i in range(k)])
    return np.median(stack, axis=0)


# ---------- 三种算法：产出每帧的骨方向场 ----------

def dirs_worldz(world):
    out = []
    for j in range(1, mk.NB):
        a, b = SEG[mk.NAMES[j]]
        out.append(point(world, b) - point(world, a))
    return out


def bone_lengths(world_frames):
    """每骨长度先验 = 各帧 world 长度的中位数（稳健）"""
    acc = {n: [] for n in SEG}
    for w in world_frames:
        for n, (a, b) in SEG.items():
            d = np.linalg.norm(point(w, b) - point(w, a))
            if d > 1e-6:
                acc[n].append(d)
    return {n: float(np.median(v)) if v else 0.3 for n, v in acc.items()}


def px_per_meter(px_frames, world_frames, lengths):
    """全局像素/米：2D 长度 / world 长度，跨帧取中位数（用躯干+四肢总和，抗单点噪声）"""
    ratios = []
    for p, w in zip(px_frames, world_frames):
        num = den = 0.0
        for n, (a, b) in SEG.items():
            if n.endswith(("Hand", "ToeBase")):
                continue
            d2 = np.linalg.norm(point(p, b) - point(p, a))
            d3 = np.linalg.norm(point(w, b) - point(w, a))
            if d2 > 4 and d3 > 1e-3:
                num += d2; den += d3
        if den > 1e-6:
            ratios.append(num / den)
    return float(np.median(ratios)) if ratios else 500.0


def dirs_lift2d(px, world, lengths, s_px_per_m, prev_signs=None, use_world_sign=True):
    """2D 方向 + 骨长反解深度。返回 (dirs, signs)"""
    dirs, signs = [], {}
    for j in range(1, mk.NB):
        name = mk.NAMES[j]
        a, b = SEG[name]
        d2 = point(px, b) - point(px, a)          # y 向上
        u = float(np.linalg.norm(d2)) / max(s_px_per_m, 1e-6)   # 投影长度（米）
        L = lengths[name]
        if u >= L * 0.995 or L <= 1e-6:
            z = 0.0
            sign = 0.0
        else:
            mag = math.sqrt(max(L * L - u * u, 0.0))
            dz = point(world, b)[2] - point(world, a)[2]        # 网络给的深度差（仅取符号）
            sign = 1.0 if dz >= 0 else -1.0
            if prev_signs is not None and name in prev_signs and abs(dz) < 0.02:
                sign = prev_signs[name]                          # 深度差太小 -> 沿用上帧符号
            z = sign * mag
        v = np.array([d2[0] / max(s_px_per_m, 1e-6), d2[1] / max(s_px_per_m, 1e-6), z])
        dirs.append(v)
        signs[name] = sign
    return dirs, signs


def build_dirs(track, algo):
    frames = track["frames"]
    world = [to_world(f) if f["world"] else None for f in frames]
    px = [to_px(f) if f["landmarks"] else None for f in frames]
    ok = [i for i in range(len(frames)) if world[i] is not None and px[i] is not None]
    if not ok:
        raise SystemExit("没有任何一帧检测到姿势")
    # 缺帧：用最近的可用帧顶替（动捕没有插值，交给上层 slerp）
    for i in range(len(frames)):
        if world[i] is None:
            j = min(ok, key=lambda k: abs(k - i))
            world[i] = world[j]; px[i] = px[j]

    lengths = bone_lengths([w for w in world])
    s = px_per_meter(px, world, lengths)

    if algo == "worldz":
        return [dirs_worldz(w) for w in world], None

    px_s = smooth_zero_phase(np.array(px), 5) if algo == "lift2d_t" else np.array(px)
    wz = medfilt(np.array([w[:, 2] for w in world]), 5) if algo == "lift2d_t" else np.array([w[:, 2] for w in world])
    world_s = [np.column_stack([w[:, 0], w[:, 1], wz[i]]) for i, w in enumerate(world)]

    out, prev = [], {}
    depths = {n: [] for n in SEG}
    for i in range(len(world_s)):
        d, prev = dirs_lift2d(px_s[i], world_s[i], lengths, s, prev if algo == "lift2d_t" else None)
        out.append(d)
        for k, n in enumerate(SEG):
            depths[n].append(d[k][2])
    if algo == "lift2d_t":
        # 深度分量再平滑一次，然后按原 3D 方向重新归一（投影方向保持不变按 2D 权重）
        for n in SEG:
            depths[n] = smooth_zero_phase(np.array(depths[n]).reshape(-1, 1), 9).ravel()
        for i in range(len(out)):
            for k, n in enumerate(SEG):
                v = np.array(out[i][k])
                v[2] = depths[n][i]
                out[i][k] = v
    return out, {"px_per_meter": s, "lengths": lengths}


def dirs_to_pose(dirs, scale_height=1.75):
    d = {}
    for k, j in enumerate(range(1, mk.NB)):
        v = np.asarray(dirs[k], dtype=np.float64)
        n = np.linalg.norm(v)
        if n > 1e-6:
            d[mk.NAMES[j]] = v / n
    return mk.pose_from_dirs(d, scale_height)


# ---------- 评价指标 ----------

PAIRS = [(7, LSHO), (11, RSHO), (8, LELB), (12, RELB), (9, LWRI), (13, RWRI),
         (15, LHIP), (19, RHIP), (16, LKNEE), (20, RKNEE), (17, LANK), (21, RANK),
         (18, LFOOT), (22, RFOOT)]


def dir_error_deg(poses, px, scale, bone_height):
    """2D 方向误差（度）：FK 骨方向 vs 画面里同一根骨的 2D 方向。

    为什么不用「关节点重投影 RMS」当主指标：那东西被**骨长比例差**支配
    （Mixamo23 是 1.6m 官方比例，被测者比例不同 ⇒ 肩/肘位置天生偏 50~100px，
    与算法好坏无关）。方向误差把比例差摘掉，只量「骨朝哪」。
    """
    errs = []
    for i, ps in enumerate(poses):
        pos, rot = mk.fk(ps, scale)
        for j in range(1, mk.NB):
            name = mk.NAMES[j]
            a_lm, b_lm = SEG[name]
            obs = point(px[i], b_lm) - point(px[i], a_lm)
            if np.linalg.norm(obs) < 8.0:      # 投影过短 => 方向不可信
                continue
            # 把 3D 骨方向用与 2D 同一套相似变换对齐（只取 z 丢弃后的方向，含全局自旋）
            d3 = rot[j][:, 1]
            fk2 = np.array([d3[0], d3[1]])
            if np.linalg.norm(fk2) < 1e-6:
                continue
            cos = float(np.dot(obs / np.linalg.norm(obs), fk2 / np.linalg.norm(fk2)))
            errs.append(math.degrees(math.acos(max(-1.0, min(1.0, abs(cos))))))
    return float(np.mean(errs)) if errs else float("nan")


def reproj_rms(poses, px, scale, bone_height):
    """FK 关节 -> 2D 关键点 的相似变换后 RMS（像素）。衡量"姿势和画面是否自洽"。"""
    errs = []
    for i, ps in enumerate(poses):
        pos, _ = mk.fk(ps, scale)
        src = np.array([pos[b][:2] for b, _ in PAIRS])
        dst = np.array([point(px[i], l)[:2] for _, l in PAIRS])
        z = src[:, 0] + 1j * src[:, 1]
        w = dst[:, 0] + 1j * dst[:, 1]
        zc = z - z.mean(); wc = w - w.mean()
        denom = (zc * zc.conj()).sum()
        if abs(denom) < 1e-9:
            continue
        a = (wc * zc.conj()).sum() / denom
        bq = w.mean() - a * z.mean()
        pred = a * z + bq
        errs.append(np.abs(pred - w))
    if not errs:
        return float("nan")
    e = np.concatenate(errs)
    return float(np.sqrt((e ** 2).mean()))


def jitter(poses, scale, bone_height):
    """逐关节加速度均方（米/帧²）：越小越顺"""
    P = np.array([mk.fk(ps, scale)[0] for ps in poses])
    if len(P) < 3:
        return float("nan")
    acc = P[2:] - 2 * P[1:-1] + P[:-2]
    return float(np.sqrt((acc ** 2).sum(axis=2).mean()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--algo", default="lift2d", choices=["worldz", "lift2d", "lift2d_t"])
    ap.add_argument("--height", type=float, default=1.78, help="被测者身高（米），用于把骨架缩放到他")
    args = ap.parse_args()

    track = json.load(open(args.track))
    dirs, info = build_dirs(track, args.algo)
    scale = args.height / mk.MIXAMO_BONE_HEIGHT

    poses = []
    for d in dirs:
        poses.append(dirs_to_pose(d, args.height))

    # 根平移：髋中心的 world 位置（相对首帧），x/y 用 2D 尺度重算以贴合画面
    px = [to_px(f) for f in track["frames"]]
    world = [to_world(f) for f in track["frames"]]
    roots = []
    hip0 = point(world[0], -1)
    for i in range(len(poses)):
        w = point(world[i], -1) - hip0
        roots.append([float(w[0]), float(w[1]), float(w[2])])

    metrics = {
        "dir_err_deg": dir_error_deg(poses, px, scale, args.height),
        "reproj_rms_px": reproj_rms(poses, px, scale, args.height),
        "jitter_m": jitter(poses, scale, args.height),
        "frames": len(poses),
    }
    if info:
        metrics["px_per_meter"] = info["px_per_meter"]

    out = {
        "algo": args.algo, "fps": track["fps"], "bone_count": mk.NB,
        "height": args.height, "scale": scale,
        "metrics": metrics,
        "frames": [{"t": i / track["fps"],
                    "quat": [list(map(float, q)) for q in poses[i]],
                    "root": roots[i]} for i in range(len(poses))],
    }
    with open(args.out, "w") as f:
        json.dump(out, f)
    print(f"[fit] {args.algo}: {len(poses)} frames  "
          f"dir_err={metrics['dir_err_deg']:.1f}deg  reproj={metrics['reproj_rms_px']:.1f}px  "
          f"jitter={metrics['jitter_m']*1000:.2f}mm  -> {args.out}")


if __name__ == "__main__":
    main()
