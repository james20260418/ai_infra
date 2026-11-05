#!/usr/bin/env python3
"""poses.json -> FBX（Mixamo23 骨架 + 逐帧关键帧）。**在 Blender 里跑**：

  blender --background --python tools/jpov/mocap/fit/export_fbx.py -- \
          --poses /tmp/pose_lift2d.json --out /tmp/out.fbx [--name lift2d]

为什么走 Blender 而不是自己写 FBX：
  FBX 的二进制格式 + 动画曲线 + 轴向/单位约定是坑海；Blender 的 io_scene_fbx 是工业级实现，
  ufbx（JPOV 的 loader）能读它导出的 FBX 7.4。我们只负责「把位姿塞进 Blender」。

坐标/朝向：
  我们的 pose 在**相机系**（x 右, y 上, z 朝相机）。Blender 是 z-up，所以写进去之前做
  (x, y, z)_ours -> (x, -z, y)_blender，即绕 X 轴 -90°。这样导出的 FBX 落到 y-up 的 JPOV 里
  正好还原。
"""
import argparse
import json
import math
import sys

import numpy as np
import bpy
from mathutils import Matrix, Quaternion, Vector

sys.path.insert(0, "/james_pm/ai_infra_3/tools/jpov/mocap/fit")
import mixamo23 as mk  # noqa: E402


def ours_to_blender(v):
    """相机系(y up, z 朝相机) -> Blender(z up)：x 不变，y_b = -z_o，z_b = y_o"""
    return Vector((v[0], -v[2], v[1]))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--poses", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="mocap")
    ap.add_argument("--scale-opt", default="FBX_SCALE_NONE")
    ap.add_argument("--global-scale", type=float, default=1.0)
    ap.add_argument("--apply-obj-scale", type=float, default=1.0)
    ap.add_argument("--axis-forward", default="-Y")
    ap.add_argument("--axis-up", default="Z")
    ap.add_argument("--armature-nodetype", default="NULL")
    ap.add_argument("--unit-scale", type=float, default=100.0,
                    help="几何缩放：100 = 让 FBX 里的数字是厘米（Mixamo 惯例，ufbx 报 unit_meters=0.01）")
    args = ap.parse_args(argv)

    data = json.load(open(args.poses))
    fps = float(data["fps"])
    scale = float(data.get("scale", 1.0))

    # ---- 清场 ----
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # ---- rest 骨架（identity pose 的 FK）----
    identity = [[0.0, 0.0, 0.0, 1.0] for _ in range(mk.NB)]
    pos, rot = mk.fk([Quaternion((1, 0, 0, 0))] * mk.NB, scale)
    pos = pos * args.unit_scale   # 写入单位（默认 cm）

    arm_data = bpy.data.armatures.new(args.name + "_arm")
    arm_obj = bpy.data.objects.new(args.name, arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")

    # 骨架：跳过 0 号（包装 Root），Hips 作为 Blender 的根骨。
    bones = {}
    for j in range(1, mk.NB):
        eb = arm_data.edit_bones.new(mk.NAMES[j])
        head = ours_to_blender(pos[j])
        # 骨长：优先到第一个子关节的距离（视觉/Blender 用），叶子骨用自身 rest 长度
        child = next((c for c in range(1, mk.NB) if mk.PAR[c] == j), None)
        length = float(np.linalg.norm(pos[child] - pos[j])) if child else max(
            0.02, float(np.linalg.norm(mk.REST[j] * scale)) * 0.8)
        ydir = ours_to_blender(np.asarray(rot[j]) @ np.array([0.0, 1.0, 0.0]))
        zdir = ours_to_blender(np.asarray(rot[j]) @ np.array([0.0, 0.0, 1.0]))
        eb.head = head
        eb.tail = head + ydir.normalized() * max(length, 0.02)
        eb.align_roll(zdir)
        bones[j] = eb
    for j in range(1, mk.NB):
        p = mk.PAR[j]
        if p >= 1:
            bones[j].parent = bones[p]
            bones[j].use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")

    # ---- 轴向：把骨架在 Blender 里转成 Y-up（FBX 惯例）再导出 ----
    # Blender 的 FBX 导出器会把「-90°X 的 Z-up→Y-up 转换」挂在**骨架对象节点**上，
    # 而 ufbx/JPOV 取的是骨节点的 local translation —— 结果是只把**根骨**转了，子骨
    # 仍是作者坐标 ⇒ 整个人躺下（实测）。所以这里自己把数据转成 Y-up（用 Blender
    # 的 NO 转换导出），让文件里的数字直接就是 Y-up。
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    arm_obj.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)

    # ---- 逐帧 pose ----
    scene = bpy.context.scene
    scene.render.fps = int(round(fps))
    arm_obj.animation_data_create()
    action = bpy.data.actions.new(args.name + "_anim")
    arm_obj.animation_data.action = action

    root0 = data["frames"][0]["root"]
    hips = arm_obj.pose.bones[mk.NAMES[1]]
    hips.rotation_mode = "QUATERNION"
    for j in range(2, mk.NB):
        arm_obj.pose.bones[mk.NAMES[j]].rotation_mode = "QUATERNION"

    # 相机系 -> Blender 的换轴是**真旋转**（绕 X +90°），所以每个 local 旋转都要共轭：
    #   R' = M R M^T  =>  q' = qM ⊗ q ⊗ qM^-1
    q_m = Quaternion((math.cos(math.pi / 4), math.sin(math.pi / 4), 0.0, 0.0))
    q_m_inv = q_m.inverted()

    for fi, fr in enumerate(data["frames"]):
        frame = fi + 1
        for j in range(1, mk.NB):
            q = fr["quat"][j]              # (x, y, z, w)
            pb = arm_obj.pose.bones[mk.NAMES[j]]
            qb = q_m @ Quaternion((q[3], q[0], q[1], q[2])) @ q_m_inv
            pb.rotation_quaternion = qb
            pb.keyframe_insert("rotation_quaternion", frame=frame)
        # 根平移（髋相对首帧的位移）
        d = (Vector(fr["root"]) - Vector(root0)) * args.unit_scale
        hips.location = ours_to_blender(d)
        hips.keyframe_insert("location", frame=frame)

    for fc in action.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"

    scene.frame_start = 1
    scene.frame_end = len(data["frames"])

    if args.apply_obj_scale != 1.0:
        bpy.context.view_layer.objects.active = arm_obj
        arm_obj.scale = (args.apply_obj_scale,) * 3
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    bpy.ops.export_scene.fbx(
        filepath=args.out,
        use_selection=False,
        add_leaf_bones=False,
        armature_nodetype=args.armature_nodetype,
        bake_anim=True,
        bake_anim_use_all_bones=True,
        bake_anim_use_nla_strips=False,
        bake_anim_simplify_factor=0.0,
        apply_scale_options=args.scale_opt,
        global_scale=args.global_scale,
        axis_forward=args.axis_forward,
        axis_up=args.axis_up,
    )
    print(f"[export_fbx] {args.name}: {len(data['frames'])} frames @ {fps}fps -> {args.out}")


if __name__ == "__main__":
    main()
