"""Build a textured tree asset (GLB) from a Blender Sapling species preset.

Offline asset-generation step of the JPOV vegetation pipeline. Blender is used
here purely as an external authoring tool — it is NOT a runtime/build dependency
of JPOV (see README.md "GPL / dependency note").

Pipeline:  species preset -> Sapling curve -> mesh + leaf cards -> PBR materials
           (bark + leaf atlas, alpha cutout, double-sided) -> .glb

Usage (headless):
  blender -b --python build_tree.py -- <species> [options]

Positional:
  species                 Sapling preset name, e.g. japanese_maple.
                          Available: callistemon douglas_fir japanese_maple
                          quaking_aspen small_maple small_pine weeping_willow
                          white_birch willow  (shipped with add_curve_sapling).

Options:
  --out PATH              Output .glb path (default: <out-dir>/<species>[_tex].glb).
  --out-dir DIR           Output directory (default: ./out).
  --textured              Apply PBR textures (leaf atlas + bark). Without this the
                          leaves are plain untextured hexagon cards.
  --textures DIR          Texture root (default: ./textures).
  --leaf-atlas NAME       leaf001 | leafset024 (default: leaf001).
  --leaf-scale F          Sapling leafScale override (card size).
  --seed N                Sapling seed.
  --set KEY=VALUE         Any Sapling operator override (repeatable). Values may be
                          scalars or comma arrays, e.g. --set curveRes=6,3,2,1.

Examples:
  blender -b --python build_tree.py -- japanese_maple --textured
  blender -b --python build_tree.py -- weeping_willow --textured \
      --leaf-atlas leaf001 --set leaves=16 --set leafScale=0.3 --leaf-scale 0.3
"""
import bpy
import os
import sys

# 同目录的 wind_bake 需可被 import（`blender -b --python build_tree.py` 不会自动把脚本目录
# 加入 sys.path）。在模块级完成，避免函数内 import（general-dev-rules §7.3）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wind_bake

# ---------------------------------------------------------------------------
# argument parsing (everything after "--")
# ---------------------------------------------------------------------------
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not argv:
    raise SystemExit("usage: blender -b --python build_tree.py -- <species> [options]")

species = argv[0]
out_path = None
out_dir = "./out"
textured = False
bake_wind = False
textures_dir = "./textures"
leaf_atlas = "leaf001"
overrides = {}

i = 1
while i < len(argv):
    a = argv[i]
    if a == "--textured":
        textured = True
    elif a == "--out":
        i += 1
        out_path = argv[i]
    elif a == "--out-dir":
        i += 1
        out_dir = argv[i]
    elif a == "--textures":
        i += 1
        textures_dir = argv[i]
    elif a == "--leaf-atlas":
        i += 1
        leaf_atlas = argv[i]
    elif a == "--leaf-scale":
        i += 1
        overrides["leafScale"] = float(argv[i])
    elif a == "--seed":
        i += 1
        overrides["seed"] = int(argv[i])
    elif a == "--set":
        i += 1
        k, _, v = argv[i].partition("=")
        overrides[k] = v
    elif a == "--bake-wind":
        bake_wind = True
    else:
        raise SystemExit(f"unknown option: {a}")
    i += 1

bpy.ops.preferences.addon_enable(module="add_curve_sapling")

PRESET_DIR = os.path.join(bpy.utils.system_resource("SCRIPTS"),
                          "addons", "add_curve_sapling", "presets")

# Valid Sapling operator properties (presets may carry keys the operator lacks).
_PROPS = bpy.ops.curve.tree_add.get_rna_type().properties
VALID = {p.identifier for p in _PROPS}
# per-level properties are fixed-length arrays; some presets store a scalar.
ARRLEN = {p.identifier: p.array_length for p in _PROPS if getattr(p, "array_length", 0)}
PROP_TYPE = {p.identifier: p.type for p in _PROPS}

# leaf atlas id -> (color file, opacity file), relative to <textures>/
LEAF_ATLASES = {
    "leaf001": ("leaf001/Leaf001_1K-JPG_Color.jpg", "leaf001/Leaf001_1K-JPG_Opacity.jpg"),
    "leafset024": ("leafset024/LeafSet024_1K-JPG_Color.jpg",
                   "leafset024/LeafSet024_1K-JPG_Opacity.jpg"),
}
BARK_COLOR = "bark001/Bark001_1K-JPG_Color.jpg"


def broadcast(value, name):
    """Preset scalars -> per-level arrays (some presets store a scalar)."""
    n = ARRLEN.get(name)
    if n and not isinstance(value, (tuple, list)):
        value = tuple([value] * n)
    return value


def parse_override(name, raw):
    """Turn a --set KEY=VALUE string into the type the operator expects."""
    if not isinstance(raw, str):
        return raw
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
        raise SystemExit(f"unknown species preset: {name} (looked in {PRESET_DIR})")
    body = "\n".join(l for l in open(path).read().splitlines()
                     if not l.strip().startswith("#"))
    data = eval(body)  # the preset file is a bare dict literal
    data = {k: v for k, v in data.items() if k in VALID}
    for k in list(data):
        data[k] = broadcast(data[k], k)
    return data


def clean_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for coll in (bpy.data.curves, bpy.data.meshes, bpy.data.materials,
                 bpy.data.objects, bpy.data.images):
        for block in list(coll):
            if block.users == 0:
                coll.remove(block)


def make_material(name, color_file, alpha_file=None, rough=0.6, alpha_clip=False):
    """Principled material. If alpha_clip, the (grayscale) opacity map drives the
    fragment alpha and the material is exported as glTF alphaMode=MASK.

    NB: the opacity map is a grayscale image WITHOUT an alpha channel, so its
    *Color* output must feed the Alpha socket. Feeding its Alpha output yields a
    constant 1.0 -> nothing is cut and the cards render as solid rectangles.
    """
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(color_file)
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    if alpha_file:
        ta = nt.nodes.new("ShaderNodeTexImage")
        ta.image = bpy.data.images.load(alpha_file)
        nt.links.new(ta.outputs["Color"], bsdf.inputs["Alpha"])
    bsdf.inputs["Roughness"].default_value = rough
    mat.use_backface_culling = False  # -> glTF doubleSided = true
    if alpha_clip and hasattr(mat, "blend_method"):
        mat.blend_method = "CLIP"      # -> glTF alphaMode = MASK
        if hasattr(mat, "alpha_threshold"):
            mat.alpha_threshold = 0.5
    return mat


def main():
    global out_path
    clean_scene()
    cfg = load_preset(species)
    cfg["showLeaves"] = True
    for k, v in overrides.items():
        cfg[k] = broadcast(parse_override(k, v), k)
    if textured:
        # Sapling only writes leaf UVs for rect/hex shapes; rect maps the atlas 1:1.
        cfg["leafShape"] = "rect"
        if cfg.get("leaves", 0) < 1:
            cfg["leaves"] = 6
        # presets often set a tiny leafScaleX (sliver cards) -> full-width card.
        cfg.setdefault("leafScaleX", 1.0)
        cfg["leafScaleX"] = 1.0

    bpy.ops.curve.tree_add(**cfg)

    trunk = next(o for o in bpy.data.objects if o.type == "CURVE")
    curve_obj = trunk   # 保留曲线对象（convert 后它的数据会被 mesh 取代）
    wind_polys = None
    if bake_wind:
        wind_polys = wind_bake.sample_source(curve_obj)   # 必须在 convert 前采样
    leaves = next((o for o in bpy.data.objects if o.type == "MESH"), None)

    bpy.ops.object.select_all(action="DESELECT")
    trunk.select_set(True)
    bpy.context.view_layer.objects.active = trunk
    bpy.ops.object.convert(target="MESH")
    trunk = bpy.context.view_layer.objects.active

    if textured:
        # trunk: UV unwrap + bark material
        bpy.ops.object.select_all(action="DESELECT")
        trunk.select_set(True)
        bpy.context.view_layer.objects.active = trunk
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=0.02)
        bpy.ops.object.mode_set(mode="OBJECT")
        bark_path = os.path.join(textures_dir, BARK_COLOR)
        if not os.path.exists(bark_path):
            raise SystemExit(f"missing bark texture: {bark_path}")
        bark = make_material("Bark", bark_path, rough=0.8)
        trunk.data.materials.clear()
        trunk.data.materials.append(bark)

        if leaves:
            color_rel, alpha_rel = LEAF_ATLASES[leaf_atlas]
            color_path = os.path.join(textures_dir, color_rel)
            alpha_path = os.path.join(textures_dir, alpha_rel)
            if not os.path.exists(color_path):
                raise SystemExit(f"missing leaf atlas: {color_path}")
            leaf_mat = make_material("Leaf", color_path, alpha_path,
                                     rough=0.5, alpha_clip=True)
            leaves.data.materials.clear()
            leaves.data.materials.append(leaf_mat)

    tris = sum(len(p.vertices) - 2 for p in trunk.data.polygons)
    if leaves:
        tris += sum(len(p.vertices) - 2 for p in leaves.data.polygons)
    dims = trunk.dimensions
    print(f"[{species}] trunk_polys={len(trunk.data.polygons)} "
          f"leaf_polys={len(leaves.data.polygons) if leaves else 0} "
          f"tris~{tris} dims=({dims.x:.2f},{dims.y:.2f},{dims.z:.2f})")

    if out_path is None:
        os.makedirs(out_dir, exist_ok=True)
        tag = species + ("_tex" if textured else "")
        out_path = os.path.join(out_dir, tag + ".glb")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    if bake_wind:
        mesh_objs = [o for o in (trunk, leaves) if o is not None]
        wind_joints = wind_bake.bake_meshes(wind_polys, mesh_objs,
                                            card_objs=[leaves] if leaves else [])
        # 关节表随资产走：作为 trunk 对象的自定义属性导出到 glTF node extras
        # （export_extras=True）。阶 = 每关节 (pivot.x, pivot.y, pivot.z, parent)；
        # parent = 父关节下标，-1 = 根。加载器读回 MeshData.wind_joints。
        flat = []
        for j in wind_joints:
            p = j['pivot']
            flat.extend([float(p.x), float(p.y), float(p.z), float(j['parent'])])
        trunk["jpov_wind_joints"] = flat

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLB", use_selection=True,
                              export_extras=True)
    print("EXPORTED", out_path, os.path.getsize(out_path), "bytes")


main()
