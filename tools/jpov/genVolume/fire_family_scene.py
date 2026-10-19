"""JPOV 火焰「原型族」—— 参数化 Mantaflow 场景 v2（非立方域 + 生命周期三段）。

四大家族（见 tools/jpov/docs/jpov_fire_family_system_design.md §3）：
  jet / area / pool / transient

v2 相对 v1 的改动（Danis 2026-10-15 验收意见）：
  1) **非立方域**（gridSize=vec3(nx,ny,nz)，**保持立方体素** ⇒ 物理各向同性）——
     火焰宽高比在**仿真侧**就对了，运行时不用拉伸。
  2) **生命周期三段**：点火(0→1) + 持续(1→1) + 熄灭(1→0)：
       · 一次调用内：先跑 main（点火→持续），在 S0 帧存**检查点**；
         再从检查点重载跑 ext（持续→熄灭）。这样 **ext 的第 0 帧状态 == main 的 S0 帧状态**
         ⇒ 循环切熄灭**无跳变**。
       · 持续段由烘焙侧做**环闭合**（见 npzseq_to_jpovvol.py --loop half-xfade），
         另点火段末尾 == 持续段首（同帧）⇒ 点火切循环**无跳变**。

输出：
  <OUT>/main/f%04d.npz   点火+持续（0..F1-1），含 density/heat/flame
  <OUT>/ext/f%04d.npz    熄灭（0..NE-1）

环境变量：FAMILY, OUT, IGNITE(默认16), S0(默认32), NLOOP(默认32), NEXT(默认20)，
          以及 NOISE_SCALE / VORT_* / BUOY_* / DISSIP（见下）。
"""
from manta import *
import numpy as np
import ctypes, os, json

FAMILY = os.environ.get("FAMILY", "jet")
outdir = os.environ.get("OUT", "/tmp/firefam2_" + FAMILY)
maindir = os.path.join(outdir, "main")
extdir = os.path.join(outdir, "ext")
ckptdir = os.path.join(outdir, "ckpt")
os.makedirs(maindir, exist_ok=True)
os.makedirs(extdir, exist_ok=True)
os.makedirs(ckptdir, exist_ok=True)

# ---- 每族参数 ----------------------------------------------------------------
# dims = 非立方域 (nx,ny,nz)（立方体素）；src/center 为**域分数**。
# burn_frames=-1 & 稳态：通过 envelope 控制源（v2 用 envelope，不用 burn_frames）。
PARAMS = {
    "jet": dict(dims=(64, 128, 64), src=(0.16, 0.02, 0.16), center=(0.50, 0.08, 0.50),
                vg=0.030, vf=0.100, diss=0.99,
                note="buoyant jet (candle/torch/column); aspect 1:2:1"),
    "area": dict(dims=(128, 64, 128), src=(0.52, 0.03, 0.52), center=(0.50, 0.05, 0.50),
                 vg=0.060, vf=0.220, diss=0.965,
                 note="campfire / fire pit (wood); turbulent tongues; aspect 2:1:2"),
    "pool": dict(dims=(144, 48, 144), src=(0.64, 0.05, 0.64), center=(0.50, 0.05, 0.50),
                 vg=0.080, vf=0.260, diss=0.955,
                 note="large-area pool fire (1m^2 class); aspect 3:1:3"),
    "transient": dict(dims=(96, 64, 96), src=(0.36, 0.16, 0.36), center=(0.50, 0.14, 0.50),
                      vg=0.150, vf=0.400, diss=0.97,
                      note="transient burst (explosion / flashover); aspect 1.5:1:1.5"),
}
if FAMILY not in PARAMS:
    raise SystemExit("unknown FAMILY=%s (jet|area|pool|transient)" % FAMILY)
P = PARAMS[FAMILY]
nx, ny, nz = P["dims"]

IGNITE = int(os.environ.get("IGNITE", "16"))     # 源 0→1 的帧数
S0 = int(os.environ.get("S0", "32"))             # 循环入口帧（= 点火段末帧）；检查点在此
NMAX = int(os.environ.get("NMAX", "128"))        # 持续段最长帧数（供最佳窗口搜索）
NEXT = int(os.environ.get("NEXT", "20"))         # 熄灭段帧数
F1 = S0 + NMAX + 2                               # main 段总帧数（多 1 帧给环闭合用）

NOISE_SCALE = float(os.environ.get("NOISE_SCALE", "1.0"))
VORT_GLOBAL = float(os.environ.get("VORT_GLOBAL", str(P["vg"])))
VORT_FLAME = float(os.environ.get("VORT_FLAME", str(P["vf"])))
BUOY_DENS = float(os.environ.get("BUOY_DENS", "-0.001"))
BUOY_HEAT = float(os.environ.get("BUOY_HEAT", "0.10"))
DISSIP = float(os.environ.get("DISSIP", str(P["diss"])))

# ---- 求解器（非立方域；立方体素）---------------------------------------------
gs = vec3(nx, ny, nz)
s = Solver(name="s", gridSize=gs, dim=3)
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
noise.posScale = vec3(nx * 0.7, ny * 0.7, nz * 0.7)   # 各轴等比 ⇒ 噪声各向同性
noise.clamp = True
noise.clampNeg = 0.0
noise.clampPos = 1.0
noise.valScale = NOISE_SCALE
noise.valOffset = 1.0
noise.timeAnim = 0.0

gravity = vec3(0.0, -0.0981, 0.0)
flags.initDomain(boundaryWidth=1)
flags.fillGrid()
setOpenBound(flags, 1, 'yY', FlagOutflow | FlagEmpty)

sx, sy, sz = P["src"]
cx, cy, cz = P["center"]
sourceBox = s.create(Box, center=gs * vec3(cx, cy, cz), size=vec3(nx * sx, ny * sy, nz * sz))

SX, SY, SZ = density.getSizeX(), density.getSizeY(), density.getSizeZ()


def grid_np(g):
    p = g.getDataPointer()
    addr = int(p, 16) if isinstance(p, str) else int(p)
    n = SX * SY * SZ
    return np.ctypeslib.as_array((ctypes.c_float * n).from_address(addr)).reshape(SZ, SY, SX)


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def dump(g, i):
    np.savez(os.path.join(g, "f%04d.npz" % i),
             density=grid_np(density).astype(np.float32),
             heat=grid_np(heat).astype(np.float32),
             flame=grid_np(flame).astype(np.float32))


def step_once(env):
    """推进一帧；env = 源强度包络 [0,1]。"""
    s.adaptTimestep(vel.getMax())
    if env > 0.0:
        sc = env
        densityInflow(flags=flags, density=density, noise=noise, shape=sourceBox, scale=sc, sigma=0.5)
        densityInflow(flags=flags, density=heat,    noise=noise, shape=sourceBox, scale=sc, sigma=0.5)
        densityInflow(flags=flags, density=fuel,    noise=noise, shape=sourceBox, scale=sc, sigma=0.5)
        densityInflow(flags=flags, density=react,   noise=noise, shape=sourceBox, scale=sc, sigma=0.5)
        processBurn(fuel=fuel, density=density, react=react, heat=heat)
    advectSemiLagrange(flags=flags, vel=vel, grid=density, order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=heat,    order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=fuel,    order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=react,   order=2)
    advectSemiLagrange(flags=flags, vel=vel, grid=vel,     order=2)
    resetOutflow(flags=flags, real=density)
    if DISSIP < 1.0:
        density.multConst(DISSIP)
        heat.multConst(DISSIP)
    flame.copyFrom(fuel)
    flame.multConst(VORT_FLAME)
    vorticityConfinement(vel=vel, flags=flags, strength=VORT_GLOBAL, strengthCell=flame)
    addBuoyancy(flags=flags, density=density, vel=vel, gravity=(gravity * BUOY_DENS))
    addBuoyancy(flags=flags, density=heat,    vel=vel, gravity=(gravity * BUOY_HEAT))
    setWallBcs(flags=flags, vel=vel)
    solvePressure(flags=flags, vel=vel, pressure=pressure)
    updateFlame(react=react, flame=flame)
    s.step()


# ---- 阶段 1：点火(0→1) + 持续(1) --------------------------------------------
os.makedirs(maindir, exist_ok=True)
while s.frame < F1:
    env = smoothstep(s.frame / float(IGNITE)) if s.frame < IGNITE else 1.0
    step_once(env)
    dump(maindir, s.frame - 1)              # s.frame 已 +1；写刚完成的帧
    if (s.frame - 1) % 8 == 0:
        mantaMsg("[%s/main] frame %d env=%.2f densMax=%.3f" % (FAMILY, s.frame - 1, env, density.getMax()))
    # 在 S0 帧存检查点（循环入口状态）
    if s.frame == S0:
        vel.save(os.path.join(ckptdir, "vel.uni"))
        density.save(os.path.join(ckptdir, "density.uni"))
        heat.save(os.path.join(ckptdir, "heat.uni"))
        fuel.save(os.path.join(ckptdir, "fuel.uni"))
        react.save(os.path.join(ckptdir, "react.uni"))
        mantaMsg("[%s] checkpoint saved at frame %d" % (FAMILY, S0))

# ---- 阶段 2：从检查点重载 → 熄灭(1→0) ---------------------------------------
vel.load(os.path.join(ckptdir, "vel.uni"))
density.load(os.path.join(ckptdir, "density.uni"))
heat.load(os.path.join(ckptdir, "heat.uni"))
fuel.load(os.path.join(ckptdir, "fuel.uni"))
react.load(os.path.join(ckptdir, "react.uni"))
s.frame = 0
while s.frame < NEXT:
    dump(extdir, s.frame)                   # 先 dump：ext[0] == 检查点状态（= 循环入口）
    env = 1.0 - smoothstep(s.frame / float(NEXT))
    step_once(env)
    if s.frame % 4 == 0:
        mantaMsg("[%s/ext] frame %d env=%.2f densMax=%.3f" % (FAMILY, s.frame, env, density.getMax()))

open(os.path.join(outdir, "meta.json"), "w").write(json.dumps({
    "family": FAMILY, "dims": [SX, SY, SZ], "S0": S0, "NMAX": NMAX,
    "F1": F1, "NEXT": NEXT, "IGNITE": IGNITE, "note": P["note"]}))
print("FAMILY_SIM3_DONE family=%s dims=(%d,%d,%d) F1=%d ext=%d" % (FAMILY, SX, SY, SZ, F1, NEXT))
