#!/usr/bin/env python3
"""poses.json -> FBX **直接写 ASCII 7.4**（不经 Blender）。

为什么不用 Blender 导出（实测结论，2026-11-03）：
  把官方 Mixamo FBX import 再 export，**bind 朝向会被改**：
    RightArm 的 bind +Y 从 (-1,0,0) 变成 (0.11,0.07,0.99)（≈120°）
    RightShoulder 差 12°，Hips 平移从 (0,1.043,0) 变 (0.044,1.019,0.010)
  火柴人看不出来（杆是轴对称的），但一上蒙皮（glb 蓝人）就拧成麻花。
  ⇒ 想要"我们算的是什么，文件里就是什么"，只能自己写。

约定（与 interface/skeleton_types.h 完全一致）：
  jointLocal(j) = T(rest_offset[j]) · R(bind[j]) · R(pose[j])
  FBX 节点：Lcl Translation = rest_offset，Lcl Rotation = bind（bind·pose 逐帧写入动画曲线）。
  单位：数值按**厘米**写（UnitScaleFactor=1 ⇒ ufbx 报 unit_meters=0.01，与 Mixamo 同）。
  轴：Y-up（UpAxis=1），与 JPOV 一致。

用法：
  export_fbx_ascii.py --poses pose.json --out out.fbx [--no-anim]
"""
import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mixamo23 as mk  # noqa: E402
import numpy as np

FBX_TICKS_PER_SEC = 46186158000


def euler_xyz(m):
    """旋转矩阵 -> 欧拉角（度），约定 M = Rz·Ry·Rx（FBX eEulerXYZ 的默认顺序）"""
    # R = Rz(c)·Ry(b)·Rx(a)
    sy = -m[2, 0]
    sy = max(-1.0, min(1.0, sy))
    b = math.asin(sy)
    if abs(sy) < 0.99999:
        a = math.atan2(m[2, 1], m[2, 2])
        c = math.atan2(m[1, 0], m[0, 0])
    else:
        a = math.atan2(-m[1, 2], m[1, 1])
        c = 0.0
    return math.degrees(a), math.degrees(b), math.degrees(c)


def euler_to_m(a_deg, b_deg, c_deg):
    a, b, c = (math.radians(v) for v in (a_deg, b_deg, c_deg))
    ca, sa = math.cos(a), math.sin(a)
    cb, sb = math.cos(b), math.sin(b)
    cc, sc = math.cos(c), math.sin(c)
    rx = np.array([[1, 0, 0], [0, ca, -sa], [0, sa, ca]])
    ry = np.array([[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]])
    rz = np.array([[cc, -sc, 0], [sc, cc, 0], [0, 0, 1]])
    return rz @ ry @ rx


def arr(vals):
    return ",".join(f"{v:.9g}" for v in vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--poses", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-anim", action="store_true")
    ap.add_argument("--cm", type=float, default=100.0)
    args = ap.parse_args()

    data = json.load(open(args.poses))
    fps = float(data["fps"])
    nframes = len(data["frames"])
    cm = args.cm

    out = []
    w = out.append
    w("; FBX 7.4.0 project file")
    w("; JPOV video-mocap exporter (ASCII)")
    w("FBXHeaderExtension:  {")
    w("\tFBXHeaderVersion: 1003")
    w("\tFBXVersion: 7400")
    w("\tCreator: \"jpov/mocap export_fbx_ascii\"")
    w("}")
    w("GlobalSettings:  {")
    w("\tVersion: 1000")
    w("\tProperties70:  {")
    w("\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1")
    w("\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1")
    w("\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2")
    w("\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1")
    w("\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0")
    w("\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1")
    w("\t\tP: \"OriginalUpAxis\", \"int\", \"Integer\", \"\",1")
    w("\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1")
    w("\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",1")
    w("\t}")
    w("}")
    w("Definitions:  {")
    w("\tVersion: 100")
    w(f"\tCount: {4 + mk.NB * 4}")
    w("\tObjectType: \"Model\" {")
    w(f"\t\tCount: {mk.NB - 1}")
    w("\t}")
    w("\tObjectType: \"AnimationStack\" {\n\t\tCount: 1\n\t}")
    w("\tObjectType: \"AnimationLayer\" {\n\t\tCount: 1\n\t}")
    w("\tObjectType: \"AnimationCurveNode\" {")
    w(f"\t\tCount: {(mk.NB - 1) * 3 + 3}")
    w("\t}")
    w("\tObjectType: \"NodeAttribute\" {")
    w(f"\t\tCount: {mk.NB - 1}")
    w("\t}")
    w("\tObjectType: \"AnimationCurve\" {")
    w(f"\t\tCount: {(mk.NB - 1) * 3 + 3}")
    w("\t}")
    w("}")
    w("Objects:  {")

    # ── 骨架节点 ─────────────────────────────────────────────
    for j in range(1, mk.NB):
        name = mk.NAMES[j]   # ⚠️ 必须保留 mixamorig: 前缀（glb 侧按同名对位，去掉就 0/23 命中）
        base = 100000 + j * 10
        t = mk.REST[j] * cm
        rx, ry, rz = euler_xyz(mk.BIND[j])
        w(f"\tModel: {base}, \"Model::{name}\", \"LimbNode\" {{")
        w("\t\tVersion: 232")
        w("\t\tProperties70:  {")
        w(f"\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",{arr(t)}")
        w(f"\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",{rx:.9g},{ry:.9g},{rz:.9g}")
        w("\t\t\tP: \"RotationOrder\", \"enum\", \"\", \"\",0")
        w("\t\t}")
        w("\t}")
    # ── NodeAttribute（FBX 7.x 里"这是根骨"是挂在 NodeAttribute 上的，不是 Model 上）──
    #    ufbx 只认这里的 "LimbNode"：Model 的第三个字段仅作显示用。
    for j in range(1, mk.NB):
        attr_id = 150000 + j * 10
        w(f"\tNodeAttribute: {attr_id}, \"NodeAttribute::\", \"LimbNode\" {{")
        w("\t\tVersion: 101")
        w("\t\tProperties70:  {")
        w("\t\t\tP: \"IsNull\", \"bool\", \"\", \"\",0")
        w("\t\t}")
        w("\t}")

    # ── 动画：Stack / Layer / CurveNode / Curve ──────────────
    stack_id, layer_id = 900000, 900010
    w(f"\tAnimationStack: {stack_id}, \"AnimStack::Take 001\", \"\" {{")
    w("\t\tProperties70:  {")
    _t0 = 0
    _t1 = int(round((nframes - 1) / fps * FBX_TICKS_PER_SEC))
    w(f"\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",{_t0}")
    w(f"\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\",{_t1}")
    w(f"\t\t\tP: \"ReferenceStart\", \"KTime\", \"Time\", \"\",{_t0}")
    w(f"\t\t\tP: \"ReferenceStop\", \"KTime\", \"Time\", \"\",{_t1}")
    w("\t\t}")
    w("\t}")
    w(f"\tAnimationLayer: {layer_id}, \"AnimLayer::BaseLayer\", \"\" {{")
    w("\t\tProperties70:  {")
    w("\t\t\tP: \"Weight\", \"Number\", \"\", \"A\",1")
    w("\t\t}")
    w("\t}")
    if not args.no_anim:
        for j in range(1, mk.NB):
            model_id = 100000 + j * 10
            node_id = 200000 + j * 10
            w(f"\tAnimationCurveNode: {node_id}, \"AnimCurveNode::R\", \"\" {{")
            w("\t\tProperties70:  {")
            for axis in "XYZ":
                w(f"\t\t\tP: \"d|{axis}\", \"Number\", \"\", \"A\",0")
            w("\t\t}")
            w("\t}")
            for k, axis in enumerate("XYZ"):
                curve_id = 300000 + j * 10 + k
                times, vals = [], []
                for fi, fr in enumerate(data["frames"]):
                    q = np.array(fr["quat"][j])
                    r = mk.BIND[j] @ mk.q2m(q)          # bind ∘ pose（Mixamo 约定）
                    e = euler_xyz(r)
                    times.append(int(round(fi / fps * FBX_TICKS_PER_SEC)))
                    vals.append(e[k])
                w(f"\tAnimationCurve: {curve_id}, \"AnimCurve::\", \"\" {{")
                w("\t\tDefault: 0")
                w("\t\tKeyVer: 4009")
                w(f"\t\tKeyTime: *{nframes} {{")
                w("\t\t\ta: " + ",".join(str(t) for t in times))
                w("\t\t}")
                w(f"\t\tKeyValueFloat: *{nframes} {{")
                w("\t\t\ta: " + arr(vals))
                w("\t\t}")
                w("\t\tKeyAttrFlags: *1 {")
                w("\t\t\ta: 24840")
                w("\t\t}")
                w("\t\tKeyAttrDataFloat: *4 {")
                w("\t\t\ta: 0,0,218434821,0")
                w("\t\t}")
                w("\t\tKeyAttrRefCount: *1 {")
                w(f"\t\t\ta: {nframes}")
                w("\t\t}")
                w("\t}")

        # ── Hips 平移（root motion）────────────────────────────────────
        # 为什么只给 Hips：本导出器没有单独的 Root 节点，Hips 是顶层骨；root
        # 在 poses.json 里是「世界系、相对首帧」的平移，正好等价于 Hips 的
        # Lcl Translation 增量（单位米 -> 文件用厘米）。
        hips = 1
        t_node_id = 400000 + hips * 10
        w(f"\tAnimationCurveNode: {t_node_id}, \"AnimCurveNode::T\", \"\" {{")
        w("\t\tProperties70:  {")
        for axis in "XYZ":
            w(f"\t\t\tP: \"d|{axis}\", \"Number\", \"\", \"A\",0")
        w("\t\t}")
        w("\t}")
        for k, axis in enumerate("XYZ"):
            t_curve_id = 500000 + hips * 10 + k
            times, vals = [], []
            for fi, fr in enumerate(data["frames"]):
                root = np.asarray(fr.get("root", [0.0, 0.0, 0.0]), dtype=np.float64)
                times.append(int(round(fi / fps * FBX_TICKS_PER_SEC)))
                vals.append((mk.REST[hips][k] + root[k]) * cm)
            w(f"\tAnimationCurve: {t_curve_id}, \"AnimCurve::\", \"\" {{")
            w("\t\tDefault: 0")
            w("\t\tKeyVer: 4009")
            w(f"\t\tKeyTime: *{nframes} {{")
            w("\t\t\ta: " + ",".join(str(t) for t in times))
            w("\t\t}")
            w(f"\t\tKeyValueFloat: *{nframes} {{")
            w("\t\t\ta: " + arr(vals))
            w("\t\t}")
            w("\t\tKeyAttrFlags: *1 {")
            w("\t\t\ta: 24840")
            w("\t\t}")
            w("\t\tKeyAttrDataFloat: *4 {")
            w("\t\t\ta: 0,0,218434821,0")
            w("\t\t}")
            w("\t\tKeyAttrRefCount: *1 {")
            w(f"\t\t\ta: {nframes}")
            w("\t\t}")
            w("\t}")
    w("}")

    # ── 连接 ────────────────────────────────────────────────
    w("Connections:  {")
    # 根骨要显式接到场景根（0），否则 ufbx 当作孤儿节点丢掉
    w(f"\tC: \"OO\",{100000 + 1 * 10},0")
    for j in range(2, mk.NB):
        p = mk.PAR[j]
        w(f"\tC: \"OO\",{100000 + j * 10},{100000 + p * 10}")
    for j in range(1, mk.NB):
        w(f"\tC: \"OO\",{150000 + j * 10},{100000 + j * 10}")
    if not args.no_anim:
        w(f"\tC: \"OO\",{layer_id},{stack_id}")
        w(f"\tC: \"OO\",{stack_id},0")
        for j in range(1, mk.NB):
            model_id = 100000 + j * 10
            node_id = 200000 + j * 10
            w(f"\tC: \"OO\",{node_id},{layer_id}")
            w(f"\tC: \"OP\",{node_id},{model_id}, \"Lcl Rotation\"")
            for k, axis in enumerate("XYZ"):
                w(f"\tC: \"OP\",{300000 + j * 10 + k},{node_id}, \"d|{axis}\"")
        hips = 1
        t_node_id = 400000 + hips * 10
        w(f"\tC: \"OO\",{t_node_id},{layer_id}")
        w(f"\tC: \"OP\",{t_node_id},{100000 + hips * 10}, \"Lcl Translation\"")
        for k, axis in enumerate("XYZ"):
            w(f"\tC: \"OP\",{500000 + hips * 10 + k},{t_node_id}, \"d|{axis}\"")
    w("}")

    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"[export_fbx_ascii] {nframes} frames @ {fps}fps, {mk.NB - 1} bones -> {args.out}")


if __name__ == "__main__":
    main()
