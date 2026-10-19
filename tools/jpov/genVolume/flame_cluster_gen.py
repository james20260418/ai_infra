#!/usr/bin/env python3
"""flame_cluster_gen.py — 用拟合出来的贝塞尔火舌拼一个「火团」（多舌簇）。

输入：flame_curve_fit.py 产出的 fitted.json（F1/F3/F5 三组参数）
输出：8×4 = 32 帧的无缝循环 flipbook（256px/格）
      外加一张 4 帧预览条，方便快速看形变。

设计要点：
  * 每根舌 = 拟合参数（h/x1/x2/x3/w0/taper/bulge）+ 独立的缩放/旋转/基点/相位；
  * 时间动画 = 参数上的低频正弦（频率取整数 → 一定无缝循环），
    不做位图位移/交叉淡化；
  * 焰心 = 贝塞尔舌叠加 + 基点白热高斯核；
  * 「镂空/细丝」= 周期性的多频正弦噪声（随 t 向上平流，仍是整周期）→ 覆盖率乘性衰减；
  * 输出 RGB = 黑体色带（白热→黄→橙→暗红），A = 覆盖。

用法：
  python3 flame_cluster_gen.py --fit /tmp/curve_fit/fitted.json \
      --out tools/jpov/assets/fire/mp/curve_cluster_8x4.png --preview /tmp/cluster_preview.png
"""

import argparse
import json
import math
import os

import numpy as np
from PIL import Image

CELL = 256
TILES_X = 8
TILES_Y = 4
FRAMES = TILES_X * TILES_Y

# ---- 颜色：黑体火色带（数值是亮度 → RGB，非 sRGB 精确解，视觉调过） ----
RAMP = [
    (0.00, (0.16, 0.010, 0.000)),
    (0.18, (0.55, 0.055, 0.004)),
    (0.36, (0.92, 0.200, 0.010)),
    (0.55, (1.00, 0.430, 0.035)),
    (0.72, (1.00, 0.660, 0.150)),
    (0.86, (1.00, 0.870, 0.450)),
    (0.95, (1.00, 0.965, 0.780)),
    (1.00, (1.00, 1.000, 0.980)),
]


def colorize(i):
    """i: (S,S) 强度 → (S,S,3) RGB。"""
    i = np.clip(i, 0.0, 1.0)
    out = np.zeros(i.shape + (3,), np.float32)
    for k in range(len(RAMP) - 1):
        t0, c0 = RAMP[k]
        t1, c1 = RAMP[k + 1]
        m = (i >= t0) & (i <= t1)
        w = (i[m] - t0) / max(t1 - t0, 1e-6)
        for ch in range(3):
            out[..., ch][m] = c0[ch] + (c1[ch] - c0[ch]) * w
    out[i > RAMP[-1][0]] = RAMP[-1][1]
    return out


def bezier(p0, p1, p2, p3, n):
    t = np.linspace(0.0, 1.0, n, dtype=np.float32)[:, None]
    a = (1 - t) ** 3
    b = 3 * (1 - t) ** 2 * t
    c = 3 * (1 - t) * t ** 2
    d = t ** 3
    return a * p0 + b * p1 + c * p2 + d * p3


def render_tongue(p, S, *, scale=1.0, rot_deg=0.0, base=(0.5, 0.92),
                  phase=0.0, bend_pulse=0.0, width_pulse=0.0, stretch=1.0,
                  width_scale=1.0, n=72):
    """单根舌：中心线贝塞尔 + 变宽度管（到折线的距离场，不是离散点高斯）。

    离散点高斯在细尖处会「串珠」，所以这里对每段算像素到线段的距离，
    宽度沿段线性插值，边缘用 smoothstep 软化 → 连续、可细到亚像素。
    """
    h = p["h"] * stretch
    p0 = np.array([0.0, 0.0], np.float32)
    p1 = np.array([p["x1"] + bend_pulse * 0.5, -h * 0.38], np.float32)
    p2 = np.array([p["x2"] + bend_pulse * 0.8, -h * 0.72], np.float32)
    p3 = np.array([p["x3"] + bend_pulse, -h], np.float32)
    pts = bezier(p0, p1, p2, p3, n) * (S * scale)

    u = np.linspace(0.0, 1.0, n, dtype=np.float32)
    w = (p["w0"] * width_scale * (1.0 + width_pulse) * S * scale
         * (1.0 - u) ** p["taper"] * (1.0 + p["bulge"] * np.sin(np.pi * u)))
    pts[:, 0] += p["wig"] * S * scale * np.sin(u * 5.2 + p["phase"] + phase) * u

    ca, sa = math.cos(math.radians(rot_deg)), math.sin(math.radians(rot_deg))
    bx, by = base[0] * S, base[1] * S
    X = pts[:, 0] * ca - pts[:, 1] * sa + bx
    Y = pts[:, 0] * sa + pts[:, 1] * ca + by

    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    field = np.zeros((S, S), np.float32)
    for k in range(n - 1):
        ax, ay = X[k], Y[k]
        vx, vy = X[k + 1] - ax, Y[k + 1] - ay
        L2 = vx * vx + vy * vy + 1e-6
        tt = np.clip(((xx - ax) * vx + (yy - ay) * vy) / L2, 0.0, 1.0)
        dx = xx - (ax + tt * vx)
        dy = yy - (ay + tt * vy)
        d = np.sqrt(dx * dx + dy * dy)
        ww = w[k] + (w[k + 1] - w[k]) * tt
        soft = np.maximum(ww * 0.35, 0.65)
        c = 1.0 - smoothstep(ww * 0.80, ww + soft, d)
        field = np.maximum(field, c)
    return field


def smoothstep(a, b, x):
    t = np.clip((x - a) / np.maximum(np.asarray(b) - np.asarray(a), 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


_NOISE_CACHE = {}


def _noise_grid(fx, fy, ft, seed):
    key = (fx, fy, ft, seed)
    if key not in _NOISE_CACHE:
        _NOISE_CACHE[key] = np.random.default_rng(seed).random(
            (ft, fy, fx), dtype=np.float32)
    return _NOISE_CACHE[key]


def sample_noise(nz, x, y, t):
    """三线性 value noise；x/y/t ∈ [0,1)，三个方向都按网格周期 wrap。"""
    ft, fy, fx = nz.shape
    gx, gy, gz = x * fx, y * fy, t * ft
    x0 = np.floor(gx).astype(np.int32) % fx
    y0 = np.floor(gy).astype(np.int32) % fy
    z0 = np.floor(gz).astype(np.int32) % ft
    x1, y1, z1 = (x0 + 1) % fx, (y0 + 1) % fy, (z0 + 1) % ft
    tx, ty, tz = gx - np.floor(gx), gy - np.floor(gy), gz - np.floor(gz)
    sx = tx * tx * (3 - 2 * tx)
    sy = ty * ty * (3 - 2 * ty)
    sz = tz * tz * (3 - 2 * tz)
    c00 = nz[z0, y0, x0] * (1 - sx) + nz[z0, y0, x1] * sx
    c10 = nz[z0, y1, x0] * (1 - sx) + nz[z0, y1, x1] * sx
    c01 = nz[z1, y0, x0] * (1 - sx) + nz[z1, y0, x1] * sx
    c11 = nz[z1, y1, x0] * (1 - sx) + nz[z1, y1, x1] * sx
    c0 = c00 * (1 - sy) + c10 * sy
    c1 = c01 * (1 - sy) + c11 * sy
    return c0 * (1 - sz) + c1 * sz


def fbm(S, t, seed=7):
    """3 octaves 的可循环 fBm（时间维也循环），返回 [0,1]。"""
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    x, y = xx / S, yy / S
    acc = np.zeros((S, S), np.float32)
    amp_sum = 0.0
    for i, (f, amp) in enumerate([(4, 0.50), (8, 0.30), (16, 0.20)]):
        nz = _noise_grid(f, f, 16, seed + i * 17)
        acc += amp * sample_noise(nz, x, y, t)
        amp_sum += amp
    return np.clip(acc / amp_sum, 0.0, 1.0)


def hole_noise(S, t):
    """湍流场：fBm + 低频 domain warp，返回 [0,1]。"""
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    x, y = xx / S, yy / S
    w = fbm(S, t, seed=41)
    xw = np.clip(x + 0.06 * (w - 0.5), 0.0, 1.0)
    yw = np.clip(y + 0.06 * (fbm(S, t, seed=73) - 0.5), 0.0, 1.0)
    n = np.zeros((S, S), np.float32)
    amp_sum = 0.0
    for i, (f, amp) in enumerate([(5, 0.40), (10, 0.34), (20, 0.26)]):
        nz = _noise_grid(f, f, 16, 101 + i * 23)
        n += amp * sample_noise(nz, xw, yw, t)
        amp_sum += amp
    return np.clip(n / amp_sum, 0.0, 1.0)


# (舌, 缩放, 旋转, 基点 dx, 基点 dy, 相位, 弯曲脉冲, 拉伸)
CLUSTER = [
    # (舌, 缩放, 旋转, 基点 dx, 基点 dy, 相位, 弯曲脉冲, 拉伸, 宽度系数)
    ("F1", 0.98,   1.0,  0.000, 0.000, 0.0,  0.070, 1.05, 0.50),
    ("F5", 0.70, -20.0, -0.085, 0.015, 1.9, -0.080, 0.95, 0.55),
    ("F3", 0.66,  22.0,  0.085, 0.012, 3.7,  0.075, 0.85, 0.50),
    ("F1", 0.44, -38.0, -0.055, 0.030, 5.1, -0.055, 0.75, 0.55),
    ("F5", 0.38,  40.0,  0.065, 0.035, 2.6,  0.055, 0.68, 0.55),
]


def render_frame(fitted, t, S=CELL):
    """一帧：细舌 max 合成 + 白热核 + fBm 撕洞 → RGBA。"""
    ph = 2.0 * math.pi * t
    cov = np.zeros((S, S), np.float32)
    for i, (name, scale, rot, dx, dy, phase, bend, stretch, wsc) in enumerate(CLUSTER):
        p = fitted[name]["params"]
        f = render_tongue(
            p, S, scale=scale * (1.0 + 0.030 * math.sin(ph + phase)),
            rot_deg=rot + 4.0 * math.sin(ph + phase * 0.7),
            base=(0.50 + dx, 0.95 + dy + 0.012 * math.sin(ph + phase)),
            phase=phase,
            bend_pulse=bend * math.sin(ph + phase),
            width_pulse=0.20 * math.sin(2 * ph + phase),
            stretch=stretch * (1.0 + 0.06 * math.sin(ph + phase * 1.3)),
            width_scale=wsc)
        cov = np.maximum(cov, f)

    # 白热核（小而密）
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    core = np.exp(-(((xx - 0.50 * S) ** 2 + (yy - 0.89 * S) ** 2)
                    / (2.0 * (0.042 * S) ** 2)))
    core *= 0.80 + 0.10 * math.sin(2 * ph)
    core *= 0.70 + 0.60 * fbm(S, t, seed=311)

    # fBm 撕洞：两级——稀疏撕开轮廓 + 高频细丝
    n = hole_noise(S, t)
    tear = smoothstep(0.455, 0.545, n)
    cov = cov * (0.22 + 0.78 * tear)
    fine = smoothstep(0.468, 0.556, fbm(S, t, seed=211))
    cov *= 0.30 + 0.70 * fine
    # 顶部越往上越稀
    up = np.clip((0.95 * S - yy) / S, 0.0, 1.0)
    cov *= np.clip(1.05 - 0.60 * smoothstep(0.30, 1.0, up), 0.0, 1.0)
    # 第三层：更细的侵蚀（让轮廓出现锯齿状火丝，而不是平滑晕开）
    wisp = smoothstep(0.46, 0.53, fbm(S, t, seed=877))
    cov *= 0.50 + 0.50 * wisp
    cov = np.maximum(cov, core * 0.72)

    # 亮度：核心/根部接近白热，其余按覆盖走橙红；
    # 再乘一层内部湍流 + 细亮丝（ridge noise），避免「平涂色块」。
    r = np.sqrt(((xx - 0.5 * S) / (0.60 * S)) ** 2 + ((yy - 0.88 * S) / (0.75 * S)) ** 2)
    inner = np.clip(1.15 - 1.05 * r, 0.0, 1.0)
    nin = fbm(S, t, seed=509)
    ridge = 1.0 - np.abs(2.0 * fbm(S, t, seed=601) - 1.0)
    ridge = ridge ** 6
    inten = cov * (0.28 + 0.80 * inner ** 1.5) * (0.55 + 0.85 * nin)
    inten += 0.55 * ridge * cov
    inten = np.clip(inten + core * 0.90, 0.0, 1.0)

    rgb = colorize(inten ** 1.05)
    alpha = smoothstep(0.16, 0.62, cov) * (0.50 + 0.50 * np.clip(inner, 0, 1))
    alpha = np.maximum(alpha, core * 0.70)
    return np.dstack([rgb, alpha])


def save_sheet(sheet, path):
    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(rgba).save(path)


def save_preview(sheet, path, idxs=(0, 6, 12, 18)):
    S = CELL
    imgs = []
    for i in idxs:
        r, c = i // TILES_X, i % TILES_X
        rr = TILES_Y - 1 - r
        imgs.append(sheet[rr * S:(rr + 1) * S, c * S:(c + 1) * S, :])
    gap = 8
    W = len(imgs) * S + (len(imgs) - 1) * gap
    out = np.zeros((S, W, 3), np.float32)
    for k, im in enumerate(imgs):
        a = im[..., 3:4]
        out[:, k * (S + gap):k * (S + gap) + S] = im[..., :3] * a
    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(path)


def main():
    global CELL
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", default="/tmp/curve_fit/fitted.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", default="")
    ap.add_argument("--cell", type=int, default=CELL)
    args = ap.parse_args()

    CELL = args.cell
    with open(args.fit) as f:
        fitted = json.load(f)

    sheet = np.zeros((TILES_Y * args.cell, TILES_X * args.cell, 4), np.float32)
    for f in range(FRAMES):
        t = f / float(FRAMES)
        img = render_frame(fitted, t, S=CELL)
        r, c = f // TILES_X, f % TILES_X
        rr = TILES_Y - 1 - r   # 行翻转（GL v=0 = PNG 顶行）
        sheet[rr * CELL:(rr + 1) * CELL, c * CELL:(c + 1) * CELL] = img

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    save_sheet(sheet, args.out)
    print("wrote", args.out, sheet.shape)
    mean_a = float(sheet[..., 3].mean())
    cov = float((sheet[..., 3] > 0.1).mean())
    print(f"alpha mean={mean_a:.4f}  covered(>0.1)={cov:.3f}")
    if args.preview:
        save_preview(sheet, args.preview)
        print("wrote", args.preview)


if __name__ == "__main__":
    main()
