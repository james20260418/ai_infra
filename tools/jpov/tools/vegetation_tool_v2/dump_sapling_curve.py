"""Blender 侧**只做一件事**：把 Sapling 建出来的树原样倒出来（曲线控制点 + 叶卡网格）。

几何怎么采样、父枝怎么判定、叶卡姿态怎么算、拓扑序怎么排 —— 全部在 C++ 侧
（`gen/shape_builder.cc`），这里不做任何决策，只做「导出 Blender 里有什么」。

为什么这么切：这个脚本跑在 Blender 自带的 python 里，没有 protobuf、没有我们自己的
库；而导出逻辑里最贵的「找父枝」是个最近邻查询，Python 里 O(n²) 慢到 2 分钟。
把决策搬走之后，这个脚本就是一段没有分支的序列化代码，改它的风险最低。

用法：
  blender -b --python dump_sapling_curve.py -- <species> --out /tmp/x.dump \
      [--set k=v ...]

输出格式（文本，一行一条记录；浮点用 repr 保证二进制往返无损）：
  jpov_sapling_dump 1
  bevel_depth <f>
  spline bezier <n>
  p <co.x> <co.y> <co.z> <hl.x> <hl.y> <hl.z> <hr.x> <hr.y> <hr.z> <radius>
  ...
  spline poly <n>
  p ...
  leaves <nverts>
  v <x> <y> <z> <u> <v>

坐标系：**Blender 原生 +Z up**，曲线与叶片顶点都是**世界坐标**（对象矩阵已经乘进去）。
  +Z up → +Y up 的换算在 C++ 侧做（与资产格式的约定一致）。
  UV 缺失的顶点写 u = -1，由 C++ 侧按「这张卡没有 UV」处理。
"""

import os
import sys

import bpy

# ---------------------------------------------------------------------------
# 参数：只认 --species/--out/--set，别的都归 C++ 侧
# ---------------------------------------------------------------------------
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not argv:
    raise SystemExit("usage: blender -b --python dump_sapling_curve.py -- "
                     "<species> --out <path.dump> [--set k=v ...]")

species = argv[0]
out_path = None
overrides = {}

i = 1
while i < len(argv):
    arg = argv[i]
    if arg == "--out":
        i += 1
        out_path = argv[i]
    elif arg == "--species":
        i += 1
        species = argv[i]
    elif arg == "--set":
        i += 1
        key, _, value = argv[i].partition("=")
        overrides[key] = value
    else:
        raise SystemExit("unknown option: %s" % arg)
    i += 1

if out_path is None:
    raise SystemExit("--out is required")

# ---------------------------------------------------------------------------
# Sapling 建树（与 export_vegetation_shape.py 的取参逻辑保持一致）
# ---------------------------------------------------------------------------
bpy.ops.preferences.addon_enable(module="add_curve_sapling")

PRESET_DIR = os.path.join(bpy.utils.system_resource("SCRIPTS"),
                          "addons", "add_curve_sapling", "presets")
_PROPS = bpy.ops.curve.tree_add.get_rna_type().properties
VALID = {p.identifier for p in _PROPS}
ARRLEN = {p.identifier: p.array_length for p in _PROPS if getattr(p, "array_length", 0)}
PROP_TYPE = {p.identifier: p.type for p in _PROPS}


def broadcast(value, name):
    width = ARRLEN.get(name)
    if width and not isinstance(value, (tuple, list)):
        return tuple([value] * width)
    return value


def parse_override(name, raw):
    kind = PROP_TYPE.get(name)
    if kind in ("ENUM", "STRING"):
        return raw
    if kind == "BOOLEAN":
        return raw.lower() in ("1", "true", "yes")
    parts = raw.split(",")
    cast = int if kind == "INT" else float
    try:
        return tuple(cast(part) for part in parts) if len(parts) > 1 else cast(raw)
    except ValueError:
        return raw


def load_preset(name):
    path = os.path.join(PRESET_DIR, name + ".py")
    if not os.path.exists(path):
        raise SystemExit("unknown species preset: %s" % name)
    body = "\n".join(line for line in open(path).read().splitlines()
                     if not line.strip().startswith("#"))
    data = eval(body)  # 预设文件是裸 dict
    data = {key: value for key, value in data.items() if key in VALID}
    for key in list(data):
        data[key] = broadcast(data[key], key)
    return data


bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

cfg = load_preset(species)
cfg["showLeaves"] = True
for key, raw in overrides.items():
    cfg[key] = broadcast(parse_override(key, raw), key)
# 叶卡必须是矩形，UV 才能整格映射（与资产格式的约定一致）
cfg["leafShape"] = "rect"
cfg.setdefault("leafScaleX", 1.0)
cfg["leafScaleX"] = 1.0
bpy.ops.curve.tree_add(**cfg)

curve_obj = next(obj for obj in bpy.data.objects if obj.type == "CURVE")
leaves_obj = None
for obj in bpy.data.objects:
    if obj.type == "MESH" and (leaves_obj is None or
                               len(obj.data.vertices) > len(leaves_obj.data.vertices)):
        leaves_obj = obj

# ---------------------------------------------------------------------------
# 写出
# ---------------------------------------------------------------------------
def fmt(value):
    """repr 保证 float ↔ 文本无损往返（C++ 侧用 strtod 读回同一个 double）。"""
    return repr(float(value))


curve_matrix = curve_obj.matrix_world
lines = []
lines.append("jpov_sapling_dump 1")
lines.append("bevel_depth %s" % fmt(curve_obj.data.bevel_depth))

for spline in curve_obj.data.splines:
    if spline.type == "BEZIER":
        points = spline.bezier_points
        lines.append("spline bezier %d" % len(points))
        for bp in points:
            co = curve_matrix @ bp.co
            handle_left = curve_matrix @ bp.handle_left
            handle_right = curve_matrix @ bp.handle_right
            lines.append("p %s %s %s %s %s %s %s %s %s %s" % (
                fmt(co.x), fmt(co.y), fmt(co.z),
                fmt(handle_left.x), fmt(handle_left.y), fmt(handle_left.z),
                fmt(handle_right.x), fmt(handle_right.y), fmt(handle_right.z),
                fmt(bp.radius)))
    else:
        points = spline.points
        lines.append("spline poly %d" % len(points))
        for point in points:
            co = curve_matrix @ point.co
            lines.append("p %s %s %s 0 0 0 0 0 0 %s" % (
                fmt(co.x), fmt(co.y), fmt(co.z), fmt(point.radius)))

leaf_vertices = 0
if leaves_obj is not None:
    mesh = leaves_obj.data
    uv_by_vertex = {}
    if mesh.uv_layers.active is not None:
        for loop in mesh.loops:
            uv = mesh.uv_layers.active.data[loop.index].uv
            uv_by_vertex[loop.vertex_index] = (uv[0], uv[1])
    mesh_matrix = leaves_obj.matrix_world
    leaf_vertices = len(mesh.vertices)
    lines.append("leaves %d" % leaf_vertices)
    for vertex in mesh.vertices:
        world = mesh_matrix @ vertex.co
        uv = uv_by_vertex.get(vertex.index)
        if uv is None:
            lines.append("v %s %s %s -1 -1" % (fmt(world.x), fmt(world.y), fmt(world.z)))
        else:
            lines.append("v %s %s %s %s %s" % (
                fmt(world.x), fmt(world.y), fmt(world.z), fmt(uv[0]), fmt(uv[1])))

os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
with open(out_path, "w") as handle:
    handle.write("\n".join(lines) + "\n")

print("DUMPED %s  (%d splines, %d leaf vertices, %.1f KB)"
      % (out_path, len(curve_obj.data.splines), leaf_vertices,
         os.path.getsize(out_path) / 1024.0))
