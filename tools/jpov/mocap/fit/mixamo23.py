#!/usr/bin/env python3
"""Mixamo23 骨架的 Python 侧镜像（与 tools/jpov/interface/mixamo23_skeleton.h 同源）。

为什么有一份 Python 镜像：
  动捕实验阶段要快速试算法（几秒一次迭代），C++ 重编译 + bazel 不适合做算法探索。
  **数值表只有一份**（下面的 BONES 逐字抄自 mixamo23_skeleton.h 的 kTable），
  最终落地产物时由 C++ 侧重新生成校验（见 mocap_gold_test 的 cross-check）。

约定（与 header 一致）：
  - 骨长轴 = 局部 +Y，rest_offset 单位米（1.600m 官方骨架基准）。
  - bind_rotation = (x, y, z, w)，四元数，相对父。
  - pose 全恒等 ⇒ T-pose。
"""
import math
import numpy as np

# (name, parent, rest_offset(m), bind_rotation(x,y,z,w))
BONES = [
    ("",                        -1, (0.0, 0.0, 0.0),                 (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:Hips",           0, (0.0, 1.042749, 0.0),            (0.006459, 0.0, 0.0, 0.999979)),
    ("mixamorig:Spine",          1, (0.0, 0.101824, 0.0),            (-0.080155, 0.0, 0.0, 0.996782)),
    ("mixamorig:Spine1",         2, (0.0, 0.100027, 0.0),            (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:Spine2",         3, (0.0, 0.093221, 0.0),            (0.012885, 0.0, 0.0, 0.999917)),
    ("mixamorig:Neck",           4, (0.0, 0.168653, 0.0),            (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:Head",           5, (0.0, 0.093419, 0.028410),       (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:LeftShoulder",   4, (0.045704, 0.111956, -0.008066), (-0.484423, -0.570970, 0.526162, -0.403090)),
    ("mixamorig:LeftArm",        7, (0.0, 0.108377, 0.0),            (-0.024607, -0.002562, 0.103504, 0.994321)),
    ("mixamorig:LeftForeArm",    8, (0.0, 0.278415, 0.0),            (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:LeftHand",       9, (0.0, 0.283288, 0.0),            (0.0, 0.0, 0.000001, 1.0)),
    ("mixamorig:RightShoulder",  4, (-0.045700, 0.111958, -0.008066), (-0.484430, 0.570964, -0.526163, -0.403087)),
    ("mixamorig:RightArm",      11, (0.0, 0.108382, 0.0),            (-0.024616, 0.002562, -0.103499, 0.994322)),
    ("mixamorig:RightForeArm",  12, (0.0, 0.278415, 0.0),            (0.0, 0.0, 0.0, 1.0)),
    ("mixamorig:RightHand",     13, (0.0, 0.283288, 0.0),            (0.0, 0.0, 0.000003, 1.0)),
    ("mixamorig:LeftUpLeg",      1, (0.082078, -0.067718, -0.015122), (0.0, 0.010368, 0.999946, 0.0)),
    ("mixamorig:LeftLeg",       15, (0.0, 0.443714, 0.0),            (-0.038112, 0.0, 0.0, 0.999273)),
    ("mixamorig:LeftFoot",      16, (0.0, 0.445278, 0.0),            (0.459749, 0.0, 0.0, 0.888049)),
    ("mixamorig:LeftToeBase",   17, (0.0, 0.138169, 0.0),            (0.335241, 0.0, 0.0, 0.942132)),
    ("mixamorig:RightUpLeg",     1, (-0.082078, -0.067718, -0.015122), (0.0, 0.010357, 0.999946, 0.0)),
    ("mixamorig:RightLeg",      19, (0.0, 0.443715, 0.0),            (-0.038091, 0.0, 0.0, 0.999274)),
    ("mixamorig:RightFoot",     20, (0.0, 0.445278, 0.0),            (0.459740, 0.0, 0.0, 0.888053)),
    ("mixamorig:RightToeBase",  21, (0.0, 0.138169, 0.0),            (0.335242, 0.0, 0.0, 0.942132)),
]

NB = len(BONES)
NAMES = [b[0] for b in BONES]
PAR = [b[1] for b in BONES]
REST = [np.array(b[2], dtype=np.float64) for b in BONES]
QUAT = [np.array(b[3], dtype=np.float64) for b in BONES]

# 骨架身高（脚底->头顶），用于把 mixamo 比例缩放到被测者的身高
MIXAMO_BONE_HEIGHT = 1.600


def q2m(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def m2q(m):
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s; x = (m[2, 1] - m[1, 2]) / s; y = (m[0, 2] - m[2, 0]) / s; z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w = (m[2, 1] - m[1, 2]) / s; x = 0.25 * s; y = (m[0, 1] + m[1, 0]) / s; z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w = (m[0, 2] - m[2, 0]) / s; x = (m[0, 1] + m[1, 0]) / s; y = 0.25 * s; z = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w = (m[1, 0] - m[0, 1]) / s; x = (m[0, 2] + m[2, 0]) / s; y = (m[1, 2] + m[2, 1]) / s; z = 0.25 * s
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + bw * ax + ay * bz - az * by,
                     aw * by + bw * ay + az * bx - ax * bz,
                     aw * bz + bw * az + ax * by - ay * bx,
                     aw * bw - ax * bx - ay * by - az * bz])


def qconj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]])


def swing(a, b):
    """最小旋转：把单位向量 a 转到单位向量 b（返回 3x3）"""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    d = float(np.dot(a, b))
    if d > 1 - 1e-9:
        return np.eye(3)
    if d < -1 + 1e-9:
        ax = np.cross(a, np.array([1.0, 0.0, 0.0]))
        if np.linalg.norm(ax) < 1e-6:
            ax = np.cross(a, np.array([0.0, 1.0, 0.0]))
        ax /= np.linalg.norm(ax)
        return q2m(np.array([ax[0], ax[1], ax[2], 0.0]))
    ax = np.cross(a, b)
    q = np.array([ax[0], ax[1], ax[2], 1.0 + d])
    return q2m(q / np.linalg.norm(q))


def rest_world():
    rb = [None] * NB
    for j in range(NB):
        p = PAR[j]
        rb[j] = q2m(QUAT[j]) if p < 0 else rb[p] @ q2m(QUAT[j])
    return rb


RB = rest_world()
REST_DIR = [RB[j] @ np.array([0.0, 1.0, 0.0]) for j in range(NB)]
BIND = [q2m(QUAT[j]) for j in range(NB)]


def pose_from_dirs(dirs, bone_height=1.75):
    """每骨目标世界方向 -> 每骨 local 旋转（四元数，相对 bind）。swing-only 拟合。

    W(j) = Swing(rest_dir_j -> dir_j) · Rb(j)      # 保留 bind 的 roll，零扭转为基准
    P(j) = B(j)^-1 · W(parent)^-1 · W(j)
    """
    w = [None] * NB
    w[0] = np.eye(3)
    out = [np.array([0.0, 0.0, 0.0, 1.0]) for _ in range(NB)]
    for j in range(1, NB):
        target = dirs.get(NAMES[j])
        if target is None or not np.all(np.isfinite(target)) or np.linalg.norm(target) < 1e-9:
            w[j] = w[PAR[j]] @ BIND[j]
        else:
            w[j] = swing(REST_DIR[j], target) @ RB[j]
        p = BIND[j].T @ w[PAR[j]].T @ w[j]
        out[j] = m2q(p)
    return out


def fk(poses, scale=1.0):
    """poses: 每骨四元数（相对 bind） -> 关节点位置（米）+ 世界旋转"""
    pos = [np.zeros(3) for _ in range(NB)]
    rot = [np.eye(3) for _ in range(NB)]
    for j in range(NB):
        p = PAR[j]
        rp = rot[p] if p >= 0 else np.eye(3)
        rot[j] = rp @ BIND[j] @ q2m(poses[j])
        if p >= 0:
            pos[j] = pos[p] + rp @ (REST[j] * scale)
        else:
            pos[j] = REST[j] * scale
    return np.array(pos), rot
