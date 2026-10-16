from manta import *
import numpy as np
import ctypes, os, json

# ---- 黑烟仿真（升腾 + 卷曲；无燃烧）----
# 复用 fire_scene_stable 的框架：去掉 processBurn，保留恒定密度源 + 浮力 + 涡度约束，
# 让密度像烟柱一样上升、翻滚。输出 density + heat（heat 仅用于浮力，可当温度通道占位）。
res = int(os.environ.get("RES", "96"))
frames = int(os.environ.get("FRAMES", "96"))
outdir = os.environ.get("OUT", "/tmp/smoke_out")
VORT_GLOBAL = float(os.environ.get("VORT_GLOBAL", "0.06"))
BUOY_HEAT = float(os.environ.get("BUOY_HEAT", "0.09"))
SRC_W = float(os.environ.get("SRC_W", "0.12"))
os.makedirs(outdir, exist_ok=True)

gs = vec3(res, res, res)
s = Solver(name='s', gridSize=gs, dim=3)
s.frameLength = 1.0
s.timestepMin = 0.2
s.timestepMax = 1.0
s.cfl = 3.0
s.timestep = (s.timestepMin + s.timestepMax) * 0.5

flags = s.create(FlagGrid)
vel = s.create(MACGrid)
density = s.create(RealGrid)
heat = s.create(RealGrid)
pressure = s.create(RealGrid)

noise = s.create(NoiseField, loadFromFile=True)
noise.posScale = vec3(res * 0.7)
noise.clamp = True
noise.clampNeg = 0.0
noise.clampPos = 1.0
noise.valScale = 1.0            # 源恒定（无闪烁）
noise.valOffset = 1.0
noise.timeAnim = 0.0

gravity = vec3(0.0, -0.0981, 0.0)
flags.initDomain(boundaryWidth=1)
flags.fillGrid()
setOpenBound(flags, 1, 'yY', FlagOutflow | FlagEmpty)

boxSize = vec3(res * 0.5 * SRC_W, 0.05 * res, res * 0.5 * SRC_W)
boxCenter = gs * vec3(0.5, 0.1, 0.5)
sourceBox = s.create(Box, center=boxCenter, size=boxSize)

SX, SY, SZ = density.getSizeX(), density.getSizeY(), density.getSizeZ()

def grid_np(g):
    p = g.getDataPointer()
    addr = int(p, 16) if isinstance(p, str) else int(p)
    n = SX * SY * SZ
    return np.ctypeslib.as_array((ctypes.c_float * n).from_address(addr)).reshape(SZ, SY, SX)

open(os.path.join(outdir, "meta.json"), "w").write(json.dumps({"res": [SX, SY, SZ], "frames": frames}))

while s.frame < frames:
    s.adaptTimestep(vel.getMax())

    densityInflow(flags=flags, density=density, noise=noise, shape=sourceBox, scale=1, sigma=0.5)
    densityInflow(flags=flags, density=heat,    noise=noise, shape=sourceBox, scale=1, sigma=0.5)

    advectSemiLagrange(flags=flags, vel=vel, grid=density, order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=heat,    order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=vel,     order=2)
    resetOutflow(flags=flags, real=density)

    vorticityConfinement(vel=vel, flags=flags, strength=VORT_GLOBAL)
    addBuoyancy(flags=flags, density=heat, vel=vel, gravity=(gravity * BUOY_HEAT))
    setWallBcs(flags=flags, vel=vel)
    solvePressure(flags=flags, vel=vel, pressure=pressure)

    np.savez(os.path.join(outdir, "f%04d.npz" % s.frame),
             density=grid_np(density).astype(np.float32),
             heat=grid_np(heat).astype(np.float32))
    if s.frame % 8 == 0:
        mantaMsg("frame %d densMax=%.3f" % (s.frame, density.getMax()))
    s.step()

print("SMOKE_SIM_DONE frames=%d res=%d" % (s.frame, SX))
