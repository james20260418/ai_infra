from manta import *
import numpy as np
import ctypes, os, json

# ---- 参数（可用环境变量覆盖：稳定燃烧用恒定源 + 低涡度）----
res = int(os.environ.get("RES", "96"))
frames = int(os.environ.get("FRAMES", "96"))
outdir = os.environ.get("OUT", "/tmp/firestable_out")
NOISE_SCALE = float(os.environ.get("NOISE_SCALE", "0.0"))   # 0 = 源恒定（无闪烁）
VORT_GLOBAL = float(os.environ.get("VORT_GLOBAL", "0.02"))  # 全局涡度约束
VORT_FLAME = float(os.environ.get("VORT_FLAME", "0.05"))    # 火焰区涡度约束
SRC_W = float(os.environ.get("SRC_W", "0.16"))              # 源盒宽（×res）
BUOY_DENS = float(os.environ.get("BUOY_DENS", "-0.001"))
BUOY_HEAT = float(os.environ.get("BUOY_HEAT", "0.1"))
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
react = s.create(RealGrid)
fuel = s.create(RealGrid)
heat = s.create(RealGrid)
flame = s.create(RealGrid)
pressure = s.create(RealGrid)

noise = s.create(NoiseField, loadFromFile=True)
noise.posScale = vec3(res * 0.7)
noise.clamp = True
noise.clampNeg = 0.0
noise.clampPos = 1.0
noise.valScale = NOISE_SCALE            # 0 ⇒ 源恒定
noise.valOffset = 1.0
noise.timeAnim = 0.0                    # 不随时间变 ⇒ 无间歇

gravity = vec3(0.0, -0.0981, 0.0)
flags.initDomain(boundaryWidth=1)
flags.fillGrid()
setOpenBound(flags, 1, 'yY', FlagOutflow | FlagEmpty)

boxSize = vec3(res * 0.5 * SRC_W, 0.04 * res, res * 0.5 * SRC_W)
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
    densityInflow(flags=flags, density=fuel,    noise=noise, shape=sourceBox, scale=1, sigma=0.5)
    densityInflow(flags=flags, density=react,   noise=noise, shape=sourceBox, scale=1, sigma=0.5)

    processBurn(fuel=fuel, density=density, react=react, heat=heat)
    advectSemiLagrange(flags=flags, vel=vel, grid=density, order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=heat,    order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=fuel,    order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=react,   order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=vel,     order=2)
    resetOutflow(flags=flags, real=density)

    flame.copyFrom(fuel)
    flame.multConst(VORT_FLAME)
    vorticityConfinement(vel=vel, flags=flags, strength=VORT_GLOBAL, strengthCell=flame)

    addBuoyancy(flags=flags, density=density, vel=vel, gravity=(gravity * BUOY_DENS))
    addBuoyancy(flags=flags, density=heat,    vel=vel, gravity=(gravity * BUOY_HEAT))
    setWallBcs(flags=flags, vel=vel)
    solvePressure(flags=flags, vel=vel, pressure=pressure)
    updateFlame(react=react, flame=flame)

    np.savez(os.path.join(outdir, "f%04d.npz" % s.frame),
             density=grid_np(density).astype(np.float32),
             heat=grid_np(heat).astype(np.float32),
             flame=grid_np(flame).astype(np.float32))
    if s.frame % 8 == 0:
        mantaMsg("frame %d densMax=%.3f" % (s.frame, density.getMax()))
    s.step()

print("STABLE_SIM_DONE frames=%d res=%d" % (s.frame, SX))
