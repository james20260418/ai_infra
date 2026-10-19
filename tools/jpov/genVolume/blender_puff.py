#!/usr/bin/env python3
"""blender_puff.py — 用 Blender 内置 Mantaflow 烘一枚「小火团」并渲成 flipbook。

设计目标（对齐 Danis 的目标火团）：
  * **真实尺度**：domain 6 cm —— 目标火团按生活经验约 5 cm，细节才不会在物理上被抹平。
  * **多簇**：底部布置多个 inflow 发射点（默认 4 个，小圆环），才会长出多股火舌。
  * **透亮**：emission 走黑体色带（白热→黄→橙→暗红），density 压低 → 半透明。
  * 输出：8×4 = 32 帧的 sprite sheet（256px/帧，透明底）。

用法（headless）：
  blender -b -P blender_puff.py -- \
      --out /tmp/puff/out.png --cache /tmp/puff/cache \
      [--domain 0.06] [--res 80] [--frames 32] [--bake 48] [--px 256] \
      [--emitters 4] [--emit-rad 0.008] [--engine eevee|cycles] [--samples 32]
"""

import math
import os
import sys

import bpy


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    a = {
        "out": "/tmp/puff/out.png",
        "cache": "/tmp/puff/cache",
        "domain": 0.06,
        "res": 80,
        "frames": 32,
        "bake": 48,
        "px": 256,
        "emitters": 4,
        "emit_rad": 0.008,
        "engine": "eevee",
        "samples": 24,
        "density": 60.0,
        "emit": 4.0,
    }
    i = 0
    while i < len(argv):
        k = argv[i].lstrip("-")
        v = argv[i + 1]
        if k not in a:
            raise SystemExit("unknown arg " + k)
        a[k] = type(a[k])(v) if not isinstance(a[k], str) else v
        i += 2
    return a


def build_domain(args):
    """只建几何。**必须在任何 fluid modifier 之前把几何全建完**：
    本机 Blender 4.0.2 在已有 DOMAIN modifier 时再调 mesh.* op 会崩
    （PyImport_AppendInittab after Py_Initialize → core dump）。"""
    D = args["domain"]
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, D * 0.5))
    dom = bpy.context.object
    dom.name = "Domain"
    dom.scale = (D, D, D)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return dom


def attach_domain(dom, args):
    D = args["domain"]
    m = dom.modifiers.new("Fluid", 'FLUID')
    m.fluid_type = 'DOMAIN'
    ds = m.domain_settings
    ds.domain_type = 'GAS'
    ds.resolution_max = args["res"]
    ds.use_adaptive_domain = False
    ds.cache_directory = args["cache"]
    ds.cache_type = 'ALL'
    ds.cache_resumable = True
    ds.cache_frame_start = 1
    ds.cache_frame_end = args["bake"]
    ds.time_scale = 1.0

    # 火：低 ignition、低 smoke，让它以火焰为主
    ds.burning_rate = 1.2
    ds.flame_ignition = 1.1
    ds.flame_max_temp = 3.0
    ds.flame_smoke = 0.15
    ds.flame_vorticity = 0.35
    ds.flame_smoke_color = (0.05, 0.05, 0.06)
    ds.use_dissolve_smoke = True
    ds.dissolve_speed = 45
    ds.vorticity = 0.25
    ds.use_noise = False
    ds.timesteps_max = 4
    ds.timesteps_min = 1
    ds.cfl_condition = 2.0
    return dom


def build_emitters(args):
    D = args["domain"]
    n = args["emitters"]
    rad = D * 0.18          # 发射点分布半径（占比）：小圈 → 多股火舌
    size = args["emit_rad"]
    out = []
    for i in range(n):
        ang = 2.0 * math.pi * i / n
        x = rad * math.cos(ang)
        y = rad * math.sin(ang)
        bpy.ops.mesh.primitive_ico_sphere_add(radius=size, location=(x, y, D * 0.06))
        fl = bpy.context.object
        fl.name = "Flow%d" % i
        fl.hide_render = True          # 发射体本身不要出现在渲染里（否则是几个黑球）
        out.append(fl)
    return out


def attach_flows(flows, args):
    D = args["domain"]
    for fl in flows:
        m = fl.modifiers.new("Fluid", 'FLUID')
        m.fluid_type = 'FLOW'
        fs = m.flow_settings
        fs.flow_type = 'FIRE'
        fs.flow_behavior = 'INFLOW'
        fs.surface_distance = 0.0
        fs.temperature = 2.0
        fs.fuel_amount = 1.4
        fs.volume_density = 1.0
        fs.use_initial_velocity = True
        fs.velocity_factor = 0.0
        fs.velocity_normal = 0.0
        fs.velocity_coord = (0.0, 0.0, 0.0)


def build_material(dom, args):
    mat = bpy.data.materials.new("PuffFire")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()

    out = nt.nodes.new("ShaderNodeOutputMaterial")
    vol = nt.nodes.new("ShaderNodeVolumePrincipled")
    nt.links.new(vol.outputs["Volume"], out.inputs["Volume"])

    # 火焰强度（flame 属性；Mantaflow 的 fire grid 名就是 "flame"）
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_type = 'GEOMETRY'
    attr.attribute_name = "flame"
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    cr = ramp.color_ramp
    cr.elements[0].position = 0.05
    cr.elements[0].color = (0.0, 0.0, 0.0, 1.0)
    cr.elements[1].position = 0.25
    cr.elements[1].color = (0.55, 0.04, 0.0, 1.0)
    e = cr.elements.new(0.55)
    e.color = (1.0, 0.30, 0.02, 1.0)
    e = cr.elements.new(0.80)
    e.color = (1.0, 0.65, 0.12, 1.0)
    e = cr.elements.new(1.00)
    e.color = (1.0, 0.95, 0.80, 1.0)
    nt.links.new(attr.outputs["Fac"], ramp.inputs["Fac"])

    nt.links.new(ramp.outputs["Color"], vol.inputs["Emission Color"])
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = 'MULTIPLY'
    mul.inputs[1].default_value = args["emit"]
    nt.links.new(attr.outputs["Fac"], mul.inputs[0])
    nt.links.new(mul.outputs["Value"], vol.inputs["Emission Strength"])

    dens = nt.nodes.new("ShaderNodeMath")
    dens.operation = 'MULTIPLY'
    dens.inputs[1].default_value = args["density"]
    nt.links.new(attr.outputs["Fac"], dens.inputs[0])
    nt.links.new(dens.outputs["Value"], vol.inputs["Density"])

    # Principled Volume 默认会再乘一个叫 "density" 的属性；流体里没有这个 grid
    # → 结果就是「有自发光、但没有吸收（alpha=0）」。这里直接指到 flame grid。
    vol.inputs["Density Attribute"].default_value = "flame"
    vol.inputs["Anisotropy"].default_value = 0.0
    dom.data.materials.append(mat)


def setup_render(args):
    sc = bpy.context.scene
    sc.render.resolution_x = args["px"]
    sc.render.resolution_y = args["px"]
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    if args["engine"] == "cycles":
        sc.render.engine = 'CYCLES'
        sc.cycles.device = 'CPU'
        sc.cycles.samples = args["samples"]
        sc.cycles.use_denoising = True
        sc.cycles.volume_step_rate = 4.0
    else:
        # 4.2+ 把 Eevee 改名成 BLENDER_EEVEE_NEXT；3.x/4.0 仍是 BLENDER_EEVEE
        for name in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE'):
            try:
                sc.render.engine = name
                break
            except TypeError:
                continue
        try:
            sc.eevee.taa_render_samples = args["samples"]
        except Exception:
            pass
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'

    # 相机：正对火团，正交视角避免透视缩放的影响
    cam_data = bpy.data.cameras.new("Cam")
    cam_data.type = 'ORTHO'
    cam_data.ortho_scale = args["domain"] * 1.05
    cam = bpy.data.objects.new("Cam", cam_data)
    bpy.context.collection.objects.link(cam)
    cam.location = (0.0, -args["domain"] * 3.0, args["domain"] * 0.5)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    sc.camera = cam

    # 世界光全黑：只留火焰自发光
    world = bpy.data.worlds.new("W")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0, 0, 0, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
    sc.world = world


def main():
    args = parse_args()
    os.makedirs(args["cache"], exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)

    sc = bpy.context.scene
    sc.frame_start = 1
    sc.frame_end = args["bake"]

    dom = build_domain(args)
    flows = build_emitters(args)
    attach_domain(dom, args)
    attach_flows(flows, args)
    build_material(dom, args)
    setup_render(args)

    # 烘焙
    with bpy.context.temp_override(active_object=dom, selected_objects=[dom],
                                   object=dom):
        bpy.ops.fluid.bake_all()

    # 调试：看域上到底有哪些 grid（volume attribute）
    try:
        names = [a.name for a in dom.data.attributes]
    except Exception as e:  # noqa: BLE001
        names = ["<err %s>" % e]
    print("DOMAIN_ATTRIBUTES:", names)
    try:
        bpy.ops.wm.save_as_mainfile(
            filepath=os.path.join(os.path.dirname(args["out"]) or ".", "puff.blend"))
    except Exception as e:  # noqa: BLE001
        print("save blend failed:", e)

    # 渲染：跳过前 8 帧自旋上升，取连续 frames 帧
    start = 9
    outdir = os.path.dirname(args["out"]) or "."
    os.makedirs(outdir, exist_ok=True)
    for i in range(args["frames"]):
        f = start + i
        sc.frame_set(f)
        sc.render.filepath = os.path.join(outdir, "f%03d.png" % i)
        bpy.ops.render.render(write_still=True)
        print("RENDERED", sc.render.filepath)


main()
