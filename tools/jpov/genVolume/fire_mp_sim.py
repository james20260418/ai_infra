#!/usr/bin/env python3
"""fire_mp_sim.py — 复现 Astralax《In Flames》那套「手绘火舌贴图 + 粒子系统」的粒子火。

文章要点（https://astralax.com/articles/inflames）：
  1. 手绘 3–6 张 **黑底火舌贴图**（256²），用 Intense（= 屏幕/加法）混合，不需要 alpha；
  2. 粒子在生命周期内**不换贴图**（随机起始贴图），靠随机旋转/缩放/颜色做出变化；
  3. 线发射器 + 发射角 36°~144°；先按初速外扩，再靠「负重量」（浮力）往上飘；
  4. 尺寸随寿命先大后小；颜色 白黄 → 橙 → 暗红 → 黑（末端黑 = 加法淡出）；
  5. 粒子自旋，自旋量随寿命衰减（避免顶部鬼畜旋转）。

本脚本把上面这套离线跑出来，直接输出 flipbook（默认 8×4 = 32 帧，256px/帧）。
贴图默认用文章附带的 5 张（仅作**复现验证**）；`--tex-dir` 可换成自己的贴图。

用法：
  python3 fire_mp_sim.py --out /tmp/mp.png [--preset campfire|torch|hero] \
      [--tex-dir /tmp/inflames/zip/Textures] [--frames 32] [--fps 24] \
      [--cell 256] [--seed 1]
"""

import argparse
import glob
import os

import numpy as np
from PIL import Image

from make_flipbook import write_png

# ---------------- 贴图 ----------------

def load_textures(tex_dir, size):
    files = sorted(glob.glob(os.path.join(tex_dir, "*.jpg")) +
                   glob.glob(os.path.join(tex_dir, "*.png")))
    if not files:
        raise SystemExit("no textures in " + tex_dir)
    out = []
    for f in files:
        im = Image.open(f).convert("RGB").resize((size, size), Image.LANCZOS)
        out.append(np.array(im, dtype=np.float32) / 255.0)
    return out


# ---------------- 粒子系统 ----------------

class Particle:
    __slots__ = ("x", "y", "vx", "vy", "age", "life", "tex", "size0", "size1",
                 "rot", "spin", "spin_now", "bright", "buoy", "drag", "tph", "tfreq")

    def __init__(self):
        self.x = self.y = self.vx = self.vy = 0.0
        self.age = self.life = 1.0
        self.tex = 0
        self.size0 = self.size1 = 64.0
        self.rot = self.spin = 0.0
        self.bright = 1.0
        self.buoy = 0.0
        self.drag = 1.0


class Sim:
    def __init__(self, preset, tex_count, w, h, rng):
        self.p = preset
        self.rng = rng
        self.w, self.h = w, h
        self.tex_count = tex_count
        self.parts = []
        self.acc = 0.0

    def spawn(self):
        p = Particle()
        r = self.rng
        cfg = self.p
        # 线发射器：底部一条线
        half = cfg["emit_half"]
        p.x = self.w * (0.5 + r.uniform(-half, half))
        p.y = self.h * cfg["emit_y"]
        ang = np.deg2rad(r.uniform(cfg["ang_min"], cfg["ang_max"]))
        sp = r.uniform(cfg["speed_min"], cfg["speed_max"])
        p.vx = np.cos(ang) * sp
        p.vy = -np.sin(ang) * sp            # 屏幕 y 向下 → 向上为负
        p.life = r.uniform(cfg["life_min"], cfg["life_max"])
        p.age = 0.0
        p.tex = int(r.integers(0, self.tex_count))
        s0 = r.uniform(cfg["size_min"], cfg["size_max"])
        p.size0 = s0
        p.size1 = s0 * cfg["size_end"]
        p.rot = r.uniform(0.0, 2.0 * np.pi)
        p.spin = r.uniform(-1.0, 1.0) * cfg["spin"]
        p.spin_now = p.spin
        p.bright = r.uniform(0.75, 1.0)
        p.buoy = cfg["buoy"] * r.uniform(0.8, 1.2)
        p.drag = cfg["drag"]
        p.tph = r.uniform(0.0, 2.0 * np.pi)
        p.tfreq = r.uniform(4.0, 9.0)
        return p

    def step(self, dt):
        cfg = self.p
        # 发射
        self.acc += cfg["rate"] * dt
        while self.acc >= 1.0:
            self.acc -= 1.0
            if len(self.parts) < cfg["max_parts"]:
                self.parts.append(self.spawn())

        alive = []
        for p in self.parts:
            p.age += dt
            if p.age >= p.life:
                continue
            t = p.age / p.life
            # 浮力（越到后期越强 → 先外扩后上升）
            k = cfg["buoy_mix"]
            f = k + (1.0 - k) * t
            p.vy += p.buoy * f * dt
            # 湍流：让火舌左右撕扯（文章里靠手绘贴图 + 自旋，这里补一点程序化扰动）
            p.vx += cfg["turb"] * np.sin(p.age * p.tfreq + p.tph) * dt
            # 阻尼
            p.vx *= (1.0 - p.drag * dt)
            p.vy *= (1.0 - p.drag * dt)
            p.x += p.vx * dt
            p.y += p.vy * dt
            # 自旋随寿命衰减（文章做法）
            p.spin_now = p.spin * (1.0 - t) ** 1.5
            p.rot += p.spin_now * dt
            alive.append(p)
        self.parts = alive


# ---------------- 渲染 ----------------

def draw_sprite(rgb, cov, tex, cx, cy, size, rot, color, gain):
    """加法叠加一张贴图（旋转 + 缩放）。"""
    H, W = rgb.shape[0], rgb.shape[1]
    r = int(size * 0.75) + 2
    x0, x1 = int(cx - r), int(cx + r) + 1
    y0, y1 = int(cy - r), int(cy + r) + 1
    if x1 <= 0 or y1 <= 0 or x0 >= W or y0 >= H:
        return
    x0c, x1c = max(0, x0), min(W, x1)
    y0c, y1c = max(0, y0), min(H, y1)
    if x1c <= x0c or y1c <= y0c:
        return
    yy = (np.arange(y0c, y1c, dtype=np.float32) + 0.5)[:, None]
    xx = (np.arange(x0c, x1c, dtype=np.float32) + 0.5)[None, :]
    dx = (xx - cx) / size
    dy = (yy - cy) / size
    c, s = np.cos(rot), np.sin(rot)
    u = (dx * c + dy * s + 0.5) * tex.shape[1]
    v = (-dx * s + dy * c + 0.5) * tex.shape[0]
    ui = np.clip(u.astype(np.int32), 0, tex.shape[1] - 1)
    vi = np.clip(v.astype(np.int32), 0, tex.shape[0] - 1)
    patch = tex[vi, ui] * gain
    rgb[y0c:y1c, x0c:x1c, :] += patch * color[None, None, :]
    cov[y0c:y1c, x0c:x1c] += patch.max(axis=2) * cfg_cov_scale


cfg_cov_scale = 0.55


def render_frame(sim, texs, cell):
    H, W = cell, cell
    rgb = np.zeros((H, W, 3), np.float32)
    cov = np.zeros((H, W), np.float32)
    for p in sim.parts:
        t = min(1.0, p.age / p.life)
        # 尺寸：先大后小（文章：Size over life 由大到小）
        size = p.size0 + (p.size1 - p.size0) * (t ** 1.3)
        # 颜色：白黄 → 橙 → 暗红 → 黑
        col = life_color(t) * p.bright
        if col.max() < 0.01:
            continue
        draw_sprite(rgb, cov, texs[p.tex], p.x, p.y, size,
                    p.rot, col, 0.30)
    alpha = 1.0 - np.exp(-cov * 1.6)
    out = np.zeros((H, W, 4), np.float32)
    out[..., :3] = np.clip(rgb, 0.0, 1.0)
    out[..., 3] = np.clip(alpha, 0.0, 1.0)
    return out


def life_color(t):
    """白黄 → 橙 → 暗红 → 黑（Intense 模式下末端黑=淡出）。"""
    stops_t = np.array([0.0, 0.15, 0.45, 0.75, 1.0], np.float32)
    r = np.interp(t, stops_t, [1.00, 1.00, 0.92, 0.50, 0.00])
    g = np.interp(t, stops_t, [0.85, 0.68, 0.34, 0.07, 0.00])
    b = np.interp(t, stops_t, [0.55, 0.30, 0.06, 0.01, 0.00])
    return np.stack([r, g, b]).astype(np.float32)


PRESETS = {
    # 文章里的「小篝火」：短、宽、粒子多
    "campfire": dict(rate=80.0, max_parts=520, life_min=0.75, life_max=1.25,
                     speed_min=20.0, speed_max=55.0, ang_min=62.0, ang_max=118.0,
                     size_min=45.0, size_max=105.0, size_end=0.40,
                     buoy=-420.0, buoy_mix=0.35, drag=0.85, spin=1.4, turb=230.0,
                     emit_half=0.20, emit_y=0.97),
    # 喷火器：高、窄、方向性强
    "torch": dict(rate=85.0, max_parts=360, life_min=0.85, life_max=1.35,
                  speed_min=55.0, speed_max=120.0, ang_min=74.0, ang_max=106.0,
                  size_min=48.0, size_max=85.0, size_end=0.38,
                  buoy=-240.0, buoy_mix=0.6, drag=0.55, spin=1.0, turb=90.0,
                  emit_half=0.05, emit_y=0.98),
    # 主火：更密、更高、更亮
    "hero": dict(rate=170.0, max_parts=780, life_min=0.85, life_max=1.45,
                 speed_min=25.0, speed_max=75.0, ang_min=55.0, ang_max=125.0,
                 size_min=65.0, size_max=115.0, size_end=0.45,
                 buoy=-430.0, buoy_mix=0.32, drag=0.8, spin=1.6, turb=150.0,
                 emit_half=0.11, emit_y=0.97),
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--preset", default="campfire", choices=list(PRESETS))
    p.add_argument("--tex-dir", default="/tmp/inflames/zip/Textures")
    p.add_argument("--tex-size", type=int, default=256)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--fps", type=float, default=24.0)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--warmup", type=float, default=2.0)
    args = p.parse_args()

    texs = load_textures(args.tex_dir, args.tex_size)
    print("textures:", len(texs))
    rng = np.random.default_rng(args.seed)
    sim = Sim(PRESETS[args.preset], len(texs), args.cell, args.cell, rng)

    dt = 1.0 / args.fps
    # 预热到稳态
    for _ in range(int(args.warmup * args.fps)):
        sim.step(dt)

    tiles, cell = args.tiles, args.cell
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for f in range(args.frames):
        sim.step(dt)
        img = render_frame(sim, texs, cell)
        r, c = f // tiles, f % tiles
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = img

    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
