"""把 Blender 里的一棵树导出成 ShapeProto 的**文本格式**（pb.txt）。

拓扑格式见 docs/vegetation_asset_format.md；proto 定义见 proto/shape.proto。

为什么手写文本格式而不是用 protobuf 库：Blender 自带 Python 里没有 protobuf runtime。
文本格式是纯文本，`protoc --decode` 或 C++ 的 TextFormat 都能读，且**可 diff、可 review**。

坐标转换：Blender 是 **+Z up**，格式约定 **+Y up** ⇒ (x, y, z) -> (x, z, -y)。

用法：
  blender -b --python export_vegetation_shape.py -- \\
      --species weeping_willow --out /tmp/willow.pbtxt \\
      --set scale=6 --set seed=5 --set leaves=10 --set leafScale=0.25 \\
      --per-seg 6 --leaf-type-desc "柳树冠层叶簇" --leaf-aspect 0.6
"""
import os
import sys

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "vegetation"))  # wind_bake 在老工具目录里
import wind_bake

# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not argv:
    raise SystemExit("usage: blender -b --python export_vegetation_shape.py -- "
                     "--species <name> --out <path.pbtxt> [--set k=v ...]")

species = argv[0] if not argv[0].startswith("--") else "weeping_willow"
out_path = None
per_seg = 6
leaf_desc = "叶簇"
leaf_aspect = 0.6   # 叶卡 宽/高：装配时 width = height × 这个值
overrides = {}

i = 0
if not argv[0].startswith("--"):
    i = 1
while i < len(argv):
    a = argv[i]
    if a == "--species":
        i += 1
        species = argv[i]
    elif a == "--out":
        i += 1
        out_path = argv[i]
    elif a == "--per-seg":
        i += 1
        per_seg = int(argv[i])
    elif a == "--leaf-type-desc":
        i += 1
        leaf_desc = argv[i]
    elif a == "--leaf-aspect":
        i += 1
        leaf_aspect = float(argv[i])
    elif a == "--set":
        i += 1
        k, _, v = argv[i].partition("=")
        overrides[k] = v
    else:
        raise SystemExit(f"unknown option: {a}")
    i += 1

if out_path is None:
    raise SystemExit("--out is required")

bpy.ops.preferences.addon_enable(module="add_curve_sapling")

PRESET_DIR = os.path.join(bpy.utils.system_resource("SCRIPTS"),
                          "addons", "add_curve_sapling", "presets")
_PROPS = bpy.ops.curve.tree_add.get_rna_type().properties
VALID = {p.identifier for p in _PROPS}
ARRLEN = {p.identifier: p.array_length for p in _PROPS if getattr(p, "array_length", 0)}
PROP_TYPE = {p.identifier: p.type for p in _PROPS}


def broadcast(value, name):
    n = ARRLEN.get(name)
    if n and not isinstance(value, (tuple, list)):
        value = tuple([value] * n)
    return value


def parse_override(name, raw):
    t = PROP_TYPE.get(name)
    if t in ("ENUM", "STRING"):
        return raw
    if t == "BOOLEAN":
        return raw.lower() in ("1", "true", "yes")
    parts = raw.split(",")
    cast = int if t == "INT" else float
    try:
        return tuple(cast(p) for p in parts) if len(parts) > 1 else cast(raw)
    except ValueError:
        return raw


def load_preset(name):
    path = os.path.join(PRESET_DIR, name + ".py")
    if not os.path.exists(path):
        raise SystemExit(f"unknown species preset: {name}")
    body = "\n".join(l for l in open(path).read().splitlines()
                     if not l.strip().startswith("#"))
    data = eval(body)  # 预设文件是裸 dict
    data = {k: v for k, v in data.items() if k in VALID}
    for k in list(data):
        data[k] = broadcast(data[k], k)
    return data


def clean_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


# ---------------------------------------------------------------------------
# 建树
# ---------------------------------------------------------------------------
clean_scene()
cfg = load_preset(species)
cfg["showLeaves"] = True
for k, v in overrides.items():
    cfg[k] = broadcast(parse_override(k, v), k)
# 叶卡必须是矩形，UV 才整格映射（与 build_tree 一致）
cfg["leafShape"] = "rect"
cfg.setdefault("leafScaleX", 1.0)
cfg["leafScaleX"] = 1.0
bpy.ops.curve.tree_add(**cfg)

curve_obj = next(o for o in bpy.data.objects if o.type == "CURVE")
# 叶卡对象：取顶点数最多的 mesh（默认立方体已被 clean_scene 删掉，Sapling 只留一个
# leaves mesh）。不按名字匹配——名字在不同 Sapling 版本里不稳，而"顶点最多的 mesh"
# 在"一棵树"这个场景里没有歧义。
leaves_obj = None
for _o in bpy.data.objects:
    if _o.type == "MESH" and (leaves_obj is None or
                              len(_o.data.vertices) > len(leaves_obj.data.vertices)):
        leaves_obj = _o

# ---------------------------------------------------------------------------
# 采样骨架（必须在 convert 之前）
# ---------------------------------------------------------------------------
polys, radii = wind_bake.sample_source_with_radius(curve_obj, per_seg=per_seg)
hier = wind_bake.build_hierarchy(polys)

# 展平成一张点表，用来给叶卡找归属（branch/s 只是调试溯源，不参与渲染）。
# 用 numpy 算最近点：442 张卡 × 1.2 万采样点，纯 Python 要几十秒。
flat_np = np.array([(p.x, p.z, -p.y) for poly in polys for p in poly], dtype=np.float64)
flat_branch = np.array([pi for pi, poly in enumerate(polys) for _ in poly], dtype=np.int64)
flat_local = np.array([k for poly in polys for k in range(len(poly))], dtype=np.int64)
flat_poly_len = np.array([len(poly) for poly in polys], dtype=np.int64)

# ---------------------------------------------------------------------------
# 坐标转换 Blender(+Z up) -> 格式(+Y up)
# ---------------------------------------------------------------------------
def to_up(v):
    """(x, y, z)_blender -> (x, z, -y)。"""
    return (v.x, v.z, -v.y)


def norm3(v):
    l = (v[0] ** 2 + v[1] ** 2 + v[2] ** 2) ** 0.5
    if l < 1e-12:
        return (0.0, 1.0, 0.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def sub3(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add3(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul3(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def cross3(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def dot3(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def len3(a):
    return (a[0] ** 2 + a[1] ** 2 + a[2] ** 2) ** 0.5


# ---------------------------------------------------------------------------
# 叶卡：每 4 个顶点一张卡（Sapling 不焊接）
# ---------------------------------------------------------------------------
cards = []
handedness = {"total": 0, "mirrored": 0, "no_uv": 0}
if leaves_obj is not None:
    mesh = leaves_obj.data
    # 叶片的 UV 层：Blender 侧已经按贴图把每个 loop 的 uv 存好了。
    # 我们**以前只读顶点位置**，所以"哪一边是 uv=0"是丢掉的 —— 手性其实一直拿得到。
    uv_by_vert = {}
    if mesh.uv_layers.active is not None:
        uvs = mesh.uv_layers.active.data
        for loop in mesh.loops:
            uv_by_vert[loop.vertex_index] = (uvs[loop.index].uv[0], uvs[loop.index].uv[1])
    vcount = len(mesh.vertices)
    if vcount % 4 != 0:
        raise SystemExit(f"leaves mesh 顶点数 {vcount} 不是 4 的倍数，无法按卡切分")
    mw = leaves_obj.matrix_world
    for c in range(vcount // 4):
        corners = [to_up(mw @ mesh.vertices[c * 4 + k].co) for k in range(4)]
        e01 = sub3(corners[1], corners[0])
        e23 = sub3(corners[3], corners[2])
        # 矩形的两组对边：|e01| 与 |e23| 相等；另一组是"高度"方向
        mid_a = mul3(add3(corners[0], corners[1]), 0.5)   # 边 0-1 的中点
        mid_b = mul3(add3(corners[2], corners[3]), 0.5)   # 边 2-3 的中点
        # 根边 = 离枝条最近的那条边（叶柄在根部）。先找卡片中心最近的骨架点。
        centre = mul3(add3(add3(corners[0], corners[1]),
                           add3(corners[2], corners[3])), 0.25)
        d2 = ((flat_np - np.array(centre)) ** 2).sum(axis=1)
        nearest = int(np.argmin(d2))
        nearest_pt = flat_np[nearest]
        which = 0 if len3(sub3(mid_a, nearest_pt)) < len3(sub3(mid_b, nearest_pt)) else 1
        root_mid, tip_mid = (mid_a, mid_b) if which == 0 else (mid_b, mid_a)
        up = norm3(sub3(tip_mid, root_mid))
        height = len3(sub3(tip_mid, root_mid))
        # Sapling 的叶卡是**居中**放在枝条上的（卡片中心在枝上），而格式里的
        # bottom_center 语义是「卡片底边中点 = anchor」。两者差半个 height：
        # 直接把 centre 当 anchor 写进去，整张卡会被抬高 h/2（实测 12.5cm）。
        # 所以在这里换算：anchor = centre − up·(h/2)，卡片实际位置与 Blender 完全一致。
        anchor_pt = (centre[0] - up[0] * height * 0.5,
                     centre[1] - up[1] * height * 0.5,
                     centre[2] - up[2] * height * 0.5)
        width_vec = sub3(corners[1], corners[0]) if which == 0 else sub3(corners[3], corners[2])
        normal = norm3(cross3(width_vec, up))
        branch_id = int(flat_branch[nearest])
        s_param = float(flat_local[nearest]) / max(1, int(flat_poly_len[branch_id]) - 1)
        if height <= 1e-6:
            continue

        # ---- 手性：贴图 u 轴在世界里指向哪一侧 ----
        # 引擎侧 right = cross(up, normal)。若贴图的 +u 与 right 同向 ⇒ 未镜像；
        # 反向 ⇒ 这张卡是镜像摆放的（叶子左右交替时就会这样）。
        mirrored = None
        quv = [uv_by_vert.get(c * 4 + k) for k in range(4)]
        if all(q is not None for q in quv):
            i_min = min(range(4), key=lambda k: quv[k][0])
            i_max = max(range(4), key=lambda k: quv[k][0])
            u_dir = sub3(corners[i_max], corners[i_min])
            right = cross3(up, normal)
            mirrored = dot3(norm3(u_dir), right) < 0.0
            handedness["total"] += 1
            handedness["mirrored"] += 1 if mirrored else 0
        else:
            handedness["no_uv"] += 1

        cards.append((anchor_pt, up, normal, height, branch_id, s_param,
                      bool(mirrored) if mirrored is not None else False))

# ---------------------------------------------------------------------------
# 写文本 proto
# ---------------------------------------------------------------------------
def fmt(x):
    return ("%.6g" % x)


lines = []
lines.append("version: 1")
lines.append('description: "%s（%s 导出，per_seg=%d，%d 枝 %d 卡）"'
             % (species, "Blender/Sapling", per_seg, len(polys), len(cards)))

# Sapling 的 spline 顺序不保证 parent < id，而格式要求拓扑序（父在前，一趟算完弧长
# 且天然无环）。这里按 BFS 重排一遍，输出顺序 = 新 id 顺序。
children = {}
for pi in range(len(polys)):
    par = hier["parent"][pi]
    children.setdefault(par, []).append(pi)
order = []
queue = list(children.get(-1, []))   # _build_hierarchy 用 -1 表示根
while queue:
    cur = queue.pop(0)
    if cur in order:
        continue
    order.append(cur)
    queue.extend(children.get(cur, []))
# 兜底：孤立分支（不连根的样条）也要导出，否则会静默丢几何
for pi in range(len(polys)):
    if pi not in order:
        order.append(pi)
new_id = {old: new for new, old in enumerate(order)}

for pi in order:
    poly = polys[pi]
    level = hier["level"][pi]
    parent = hier["parent"][pi]
    # 环细分：主干最细，越深越省
    ring = 16 if level == 0 else (12 if level == 1 else 8)
    lines.append("branches {")
    lines.append("  id: %d" % new_id[pi])
    lines.append("  parent: %d" % (new_id[parent] if parent >= 0 else -1))
    lines.append("  level: %d" % level)
    lines.append("  ring_count: %d" % ring)
    lines.append("  seam_index: 0")
    for k, pt in enumerate(poly):
        p = to_up(pt)
        r = radii[pi][k] if k < len(radii[pi]) else radii[pi][-1]
        lines.append("  samples {")
        lines.append("    position { x: %s y: %s z: %s }" % (fmt(p[0]), fmt(p[1]), fmt(p[2])))
        lines.append("    radius: %s" % fmt(max(r, 1e-4)))
        lines.append("    tilt: 0")
        lines.append("  }")
    lines.append("}")

mirrored_count = sum(1 for c in cards if c[6])
plain_count = len(cards) - mirrored_count
type_of = {}
if plain_count:
    type_of[False] = 0
if mirrored_count:
    type_of[True] = 1 if plain_count else 0

print("[export] 叶卡手性：未镜像 %d 张 → type %s，镜像 %d 张 → type %s"
      % (plain_count, type_of.get(False, "-"), mirrored_count, type_of.get(True, "-")))
print("[export] 原始统计：%d 张有 UV，镜像 %d 张（%.1f%%），缺 UV %d 张"
      % (handedness["total"], handedness["mirrored"],
         100.0 * handedness["mirrored"] / max(1, handedness["total"]),
         handedness["no_uv"]))

# 手性 → 两个 leaf type。
# 一种 leaf type 只对应一种对齐方式（Danis 定）：Blender 里 UV 镜像的那批卡
# 单独成一个 type，由用户提供一张**左右翻转**后的素材；引擎不做运行时镜像。
for flipped in (False, True):
    if flipped not in type_of:
        continue
    lines.append("leaf_types {")
    lines.append("  type: %d" % type_of[flipped])
    if flipped:
        lines.append('  description: "%s（镜像手性：请把素材左右翻转后使用）"' % leaf_desc)
    else:
        lines.append('  description: "%s"' % leaf_desc)
    lines.append('  anchor_mode: "bottom_center"')
    lines.append("  preferred_aspect: 0")
    lines.append("}")

for anchor, up, normal, height, branch_id, s_param, flipped in cards:
    lines.append("cards {")
    lines.append("  anchor { x: %s y: %s z: %s }" % (fmt(anchor[0]), fmt(anchor[1]), fmt(anchor[2])))
    lines.append("  up { x: %s y: %s z: %s }" % (fmt(up[0]), fmt(up[1]), fmt(up[2])))
    lines.append("  normal { x: %s y: %s z: %s }"
                 % (fmt(normal[0]), fmt(normal[1]), fmt(normal[2])))
    lines.append("  height: %s" % fmt(height))
    lines.append("  width: %s" % fmt(height * leaf_aspect))
    lines.append("  type: %d" % type_of[flipped])
    lines.append("  branch: %d" % new_id.get(branch_id, 0))
    lines.append("  s: %s" % fmt(s_param))
    lines.append("}")

os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
with open(out_path, "w") as handle:
    handle.write("\n".join(lines) + "\n")

print("EXPORTED %s  (%d branches, %d cards, %.1f KB)"
      % (out_path, len(polys), len(cards), os.path.getsize(out_path) / 1024.0))
