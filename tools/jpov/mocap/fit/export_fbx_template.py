#!/usr/bin/env python3
"""poses.json -> FBX，**以官方 Mixamo FBX 为模板**（在 Blender 里跑）。

    blender --background --python export_fbx_template.py -- \
            --poses pose.json --template assets/.../hip_hop_dance.fbx --out out.fbx

为什么不自己搭骨架（上一版 `export_fbx.py` 被这个坑咬过）：
  Blender 的 FBX **导出器**会给每根骨写 PreRotation（FBX 的 LimbNode 以骨长轴为 X，
  Blender 用 Y），而 ufbx / JPOV 读的是 `local_transform` 的组合值 —— 两边对这个
  组合的解释不一致，结果就是「每根骨的 Y 轴正负交替」= 骨架拧成麻花（实测）。
  而**一模一样地 import 再 export** 官方 Mixamo FBX 是好的（实测 bbox 1.75m 正确）。
  ⇒ 所以：拿官方 FBX 当模板，只改 pose，不重建骨架。

姿势的语义与 `mixamo23.py` / `interface/skeleton_types.h` 完全一致：
  jointLocal = T(rest) · R(bind) · R(pose)  ⇒  Blender 的 matrix_basis 就是 R(pose)。
"""
import argparse
import json
import sys

import bpy
from mathutils import Quaternion

sys.path.insert(0, "/james_pm/ai_infra_3/tools/jpov/mocap/fit")
import mixamo23 as mk  # noqa: E402

KEEP = [n for n in mk.NAMES if n]          # 23 骨里除空名 root 外的 22 根


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--poses", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--template",
                    default="/james_pm/ai_infra_3/tools/jpov/assets/models/characters/hip_hop_dance.fbx")
    ap.add_argument("--name", default="mocap")
    args = ap.parse_args(argv)

    data = json.load(open(args.poses))
    fps = float(data["fps"])

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=args.template, automatic_bone_orientation=False)

    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")

    # 删掉模板里我们不要的骨（手指/叶端）——保住 Mixamo23 的 23 骨语义。
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    missing = [n for n in KEEP if n not in arm.data.edit_bones]
    assert not missing, f"模板缺少骨: {missing[:5]}"
    for eb in list(arm.data.edit_bones):
        if eb.name not in KEEP:
            arm.data.edit_bones.remove(eb)
    bpy.ops.object.mode_set(mode="OBJECT")

    # 丢掉模板自带的动画，换成我们的
    if arm.animation_data:
        arm.animation_data_clear()
    bpy.context.scene.render.fps = int(round(fps))

    arm.animation_data_create()
    action = bpy.data.actions.new(args.name + "_anim")
    arm.animation_data.action = action

    for b in arm.pose.bones:
        b.rotation_mode = "QUATERNION"

    for fi, fr in enumerate(data["frames"]):
        frame = fi + 1
        for j in range(1, mk.NB):
            q = fr["quat"][j]                  # (x, y, z, w)
            pb = arm.pose.bones.get(mk.NAMES[j])
            if pb is None:
                continue
            pb.rotation_quaternion = Quaternion((q[3], q[0], q[1], q[2]))
            pb.keyframe_insert("rotation_quaternion", frame=frame)

    for fc in action.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = len(data["frames"])

    bpy.ops.export_scene.fbx(
        filepath=args.out, use_selection=False, add_leaf_bones=False,
        bake_anim=True, bake_anim_use_all_bones=True, bake_anim_use_nla_strips=False,
        bake_anim_simplify_factor=0.0, apply_scale_options="FBX_SCALE_NONE",
        axis_forward="-Z", axis_up="Y",
        primary_bone_axis="Y", secondary_bone_axis="X")
    print(f"[export_fbx_template] {args.name}: {len(data['frames'])} frames @ {fps}fps -> {args.out}")


if __name__ == "__main__":
    main()
