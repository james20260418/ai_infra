from manta import *
import numpy as np
import ctypes, os, sys, json

# ---- 参数 ----
res = int(os.environ.get("RES", "64"))
frames = int(os.environ.get("FRAMES", "8"))
outdir = os.environ.get("OUT", "/tmp/firesim_out")
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

# 噪声
noise = s.create(NoiseField, loadFromFile=True)
noise.posScale = vec3(res * 0.7)
noise.clamp = True
noise.clampNeg = 0.0
noise.clampPos = 1.0
noise.valScale = 1.0
noise.valOffset = 0.75
noise.timeAnim = 0.2

gravity = vec3(0.0, -0.0981, 0.0)      # Y 向上
vortGlobal = 0.1
vortFlames = 0.5

flags.initDomain(boundaryWidth=1)
flags.fillGrid()
setOpenBound(flags, 1, 'yY', FlagOutflow | FlagEmpty)

# 源：底部中心盒
boxSize = vec3(res / 8.0, 0.05 * res, res / 8.0)
boxCenter = gs * vec3(0.5, 0.12, 0.5)
sourceBox = s.create(Box, center=boxCenter, size=boxSize)

SX, SY, SZ = density.getSizeX(), density.getSizeY(), density.getSizeZ()

def grid_np(g):
    p = g.getDataPointer()
    addr = int(p, 16) if isinstance(p, str) else int(p)
    n = SX * SY * SZ
    a = np.ctypeslib.as_array((ctypes.c_float * n).from_address(addr))
    return a.reshape(SZ, SY, SX)      # [z][y][x]，x 最快

meta = {"res": [SX, SY, SZ], "frames": frames, "up_axis": "y"}
open(os.path.join(outdir, "meta.json"), "w").write(json.dumps(meta))

while s.frame < frames:
    maxvel = vel.getMax()
    s.adaptTimestep(maxvel)

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
    flame.multConst(vortFlames)
    vorticityConfinement(vel=vel, flags=flags, strength=vortGlobal, strengthCell=flame)

    addBuoyancy(flags=flags, density=density, vel=vel, gravity=(gravity * -0.001))
    addBuoyancy(flags=flags, density=heat,    vel=vel, gravity=(gravity * 0.1))

    setWallBcs(flags=flags, vel=vel)
    solvePressure(flags=flags, vel=vel, pressure=pressure)

    updateFlame(react=react, flame=flame)

    # ---- 导出本帧：density + heat + flame（float32 .npy）----
    fn = os.path.join(outdir, "f%04d.npz" % s.frame)
    np.savez(fn,
             density=grid_np(density).astype(np.float32),
             heat=grid_np(heat).astype(np.float32),
             flame=grid_np(flame).astype(np.float32))
    mantaMsg("dumped %s  densMax=%.3f heatMax=%.3f" % (fn, density.getMax(), heat.getMax()))

    s.step()

print("FIRE_SIM_DONE frames=%d res=%dx%dx%d" % (s.frame, SX, SY, SZ))
