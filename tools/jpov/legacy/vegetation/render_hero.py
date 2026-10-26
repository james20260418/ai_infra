"""Render a hero image of a generated tree GLB (Blender Cycles, CPU).

Optional presentation step of the vegetation tool — produces a large, lit image
of a .glb asset for review. Uses Cycles on CPU so it works headless with no GPU
(this container's EEVEE cannot initialise EGL).

Usage (headless):
  blender -b --python render_hero.py -- <asset.glb> <out.png> [width] [height]

Example:
  blender -b --python render_hero.py -- out/weeping_willow_tex.glb hero.png 1600 1067

Notes:
- Cycles' OpenImageDenoise is not always compiled in; denoising is disabled.
- A ground plane + sun + area fill + sky world give readable shading.
"""
import bpy
import math
import mathutils
import sys

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    raise SystemExit("usage: blender -b --python render_hero.py -- <glb> <out.png> [w] [h]")

glb, out = argv[0], argv[1]
width = int(argv[2]) if len(argv) > 2 else 1500
height = int(argv[3]) if len(argv) > 3 else 1000

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
bpy.ops.import_scene.gltf(filepath=glb)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]

# world-space bounds
mn = mathutils.Vector((1e9, 1e9, 1e9))
mx = mathutils.Vector((-1e9, -1e9, -1e9))
for o in meshes:
    for corner in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(corner)
        for k in range(3):
            mn[k] = min(mn[k], w[k])
            mx[k] = max(mx[k], w[k])
center = (mn + mx) / 2
size = mx - mn

# ground
bpy.ops.mesh.primitive_plane_add(size=max(size) * 6, location=(center.x, center.y, mn.z))
ground = bpy.context.active_object
gmat = bpy.data.materials.new("Ground")
gmat.use_nodes = True
gmat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.18, 0.22, 0.14, 1)
ground.data.materials.append(gmat)

# key sun + area fill
sun = bpy.data.lights.new("Sun", "SUN")
sun.energy = 3.2
sun.angle = math.radians(3)
sun_obj = bpy.data.objects.new("Sun", sun)
bpy.context.scene.collection.objects.link(sun_obj)
sun_obj.rotation_euler = (math.radians(52), 0, math.radians(-40))

fill = bpy.data.lights.new("Fill", "AREA")
fill.energy = 8000
fill.size = max(size) * 3
fill_obj = bpy.data.objects.new("Fill", fill)
bpy.context.scene.collection.objects.link(fill_obj)
fill_obj.location = center + mathutils.Vector((-size.x, size.y * 2, size.z))
fill_obj.rotation_euler = (math.radians(70), 0, math.radians(-140))

# sky
world = bpy.data.worlds.new("World")
bpy.context.scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs[0].default_value = (0.55, 0.66, 0.82, 1)
bg.inputs[1].default_value = 0.9

# camera: fit the bounding sphere
cam_data = bpy.data.cameras.new("Cam")
cam_data.lens = 55
cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.scene.collection.objects.link(cam)
radius = max(size) * 0.62
fov = 2 * math.atan(0.5 * 36.0 / cam_data.lens)
dist = radius / math.tan(fov / 2) * 1.15
az = math.radians(-35)
elev = math.radians(12)
dirv = mathutils.Vector((math.sin(az) * math.cos(elev),
                         -math.cos(az) * math.cos(elev),
                         math.sin(elev)))
cam.location = center + dirv * dist
cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
bpy.context.scene.camera = cam

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 72
scene.cycles.use_denoising = False
scene.render.resolution_x = width
scene.render.resolution_y = height
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
