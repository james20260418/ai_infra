#!/usr/bin/env python3
"""make_flipbook.py — 把 .jpovvol 体积（density+temperature 或 density-only）离线渲染成
广告牌粒子用的 flipbook 序列帧贴图（PNG）。

用途：给 `jpov_fire_fog_viewer` 的「粒子火」场景产出**真火纹理**（源自 Mantaflow 烘的
火体积），或产出**黑烟**用的 puff flipbook。所有重活留在离线；引擎运行期只读 PNG。

体数据格式（JPVOL1/JPVOL2）见 tools/jpov/src/fire_fog/fire_volume.h：
  帧内 x 最快 → y → z，通道交错；JPVOL2 头 40B（magic8 + 8×uint32），JPVOL1 头 32B。
  R=density ∈[0,1]，G=temperature ∈[0,1]。

渲染：沿 z 轴前到后 emission-absorption 积分（每像素一条视线）：
  σ = density·SIGMA；a = 1−exp(−σ·ds)；L += T·a·(黑体色)·EMIT；T *= (1−a)
  输出 RGB=L（火色/灰）、A=1−T（不透明度）。

用法：
  python3 make_flipbook.py fire  <in.jpovvol> <out.png> [tiles]
  python3 make_flipbook.py smoke <in.jpovvol> <out.png> [tiles]
"""

import struct
import sys
import zlib

import numpy as np


# ---------------- .jpovvol 读取 ----------------

def load_jpovvol(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    magic = blob[:8]
    if magic[:6] == b"JPVOL2":
        ver, nx, ny, nz, nframes, channels, dtype, _res = struct.unpack("<8I", blob[8:40])
        data = blob[40:]
    elif magic[:6] == b"JPVOL1":
        ver, nx, ny, nz, channels, dtype = struct.unpack("<6I", blob[8:32])
        nframes = 1
        data = blob[32:]
    else:
        raise ValueError("not a JPVOL file: %r" % magic[:8])
    np_dtype = np.uint8 if dtype == 1 else np.float32
    arr = np.frombuffer(data, dtype=np_dtype)
    # 帧内 x 最快 → y → z，通道交错 → reshape (frames, nz, ny, nx, channels)
    arr = arr.reshape(nframes, nz, ny, nx, channels)
    if dtype == 1:
        arr = arr.astype(np.float32) / 255.0
    return arr  # (frames, nz, ny, nx, channels)


# ---------------- 颜色 ----------------

def _hash01(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return h / 4294967296.0


def _value_noise(x, y, seed):
    ix = int(np.floor(x))
    iy = int(np.floor(y))
    fx = x - ix
    fy = y - iy
    ux = fx * fx * (3.0 - 2.0 * fx)
    uy = fy * fy * (3.0 - 2.0 * fy)
    a = _hash01(ix, iy, seed)
    b = _hash01(ix + 1, iy, seed)
    c = _hash01(ix, iy + 1, seed)
    d = _hash01(ix + 1, iy + 1, seed)
    return (a + (b - a) * ux) * (1.0 - uy) + (c + (d - c) * ux) * uy


def _fbm(x, y, seed, octaves=3):
    v = 0.0
    amp = 0.5
    fx, fy = x, y
    for i in range(octaves):
        v += amp * _value_noise(fx, fy, seed + i * 17)
        fx *= 2.03
        fy *= 2.11
        amp *= 0.5
    return v / 0.875


def render_puff(tiles, cell, kind="fire"):
    """烘 puff flipbook：每格 = 一小团（不规则软轮廓 + 内部噪声细节 + 点火→熄灭包络）。
    kind="fire"：火色；kind="smoke"：白灰（引擎端染黑）。
    ★ 帧间连续性：噪声用**固定种子 + 随帧平滑偏移**，不用逐帧变种子（否则纹理高频震颤）。"""
    nframes = tiles * tiles
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    xs = (np.arange(cell, dtype=np.float32) + 0.5) / cell
    ys = (np.arange(cell, dtype=np.float32) + 0.5) / cell
    U, V = np.meshgrid(xs, ys)
    FY = 1.0 - V
    dx = U - 0.5
    dyv = FY - 0.40
    ang = np.arctan2(dyv, dx)
    stretch = 0.82 if kind == "fire" else 1.0     # 火舌略拉长；烟更圆
    rr = np.sqrt(dx * dx + (dyv * stretch) ** 2)
    for f in range(nframes):
        t = f / float(max(1, nframes - 1))
        if kind == "fire":
            env = min(t / 0.25, (1.0 - t) / 0.35)
        else:
            env = min(t / 0.35, (1.0 - t) / 0.45)
        env = 0.0 if env < 0.0 else (1.0 if env > 1.0 else env)
        env = env * env * (3.0 - 2.0 * env)
        # 平滑的帧漂移（小）：轮廓与内部细节都用它随帧缓慢变 → 无高频震颤。
        fd = f * (0.06 if kind == "fire" else 0.04)
        wob = np.vectorize(lambda a_: _fbm(np.cos(a_) * 1.3 + 0.5,
                                           np.sin(a_) * 1.3 + 0.5 + fd, 7))(ang)
        r0 = 0.30 if kind == "fire" else 0.34
        rad = (r0 + 0.09 * wob) * (0.55 + 0.45 * env)
        edge = np.clip(1.0 - rr / np.maximum(rad, 1e-4), 0.0, 1.0)
        a = edge ** (1.25 if kind == "fire" else 1.0)
        detail = np.vectorize(lambda u_, v_: _fbm(u_ * 3.2, v_ * 3.2 + fd, 3))(U, FY)
        a *= (0.62 + 0.48 * detail)     # 内部细节（降幅/降频，减“细碎”）
        a = np.clip(a * env, 0.0, 1.0)
        R = np.full_like(a, 1.0 if kind == "fire" else 0.90)
        if kind == "fire":
            G = np.clip(0.25 + 0.72 * np.minimum(1.0, a * 1.25), 0.0, 1.0)
            B = a * a * a * 0.7
        else:
            G = np.full_like(a, 0.90)
            B = np.full_like(a, 0.92)
        cellimg = np.zeros((cell, cell, 4), np.float32)
        cellimg[..., 0] = R
        cellimg[..., 1] = G
        cellimg[..., 2] = B
        cellimg[..., 3] = a
        cellimg = box_blur(cellimg, 2, 2)
        row = f // tiles
        col = f % tiles
        sheet[row * cell:(row + 1) * cell, col * cell:(col + 1) * cell, :] = cellimg
    return (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


# 可分离盒模糊（(H,W,C) 图像；软化 flipbook 边缘，减“骰子/三角感”）。
def box_blur(a, r=2, passes=2):
    a = a.astype(np.float32).copy()
    if r <= 0:
        return a
    for _ in range(passes):
        pad = np.pad(a, ((0, 0), (r, r), (0, 0)), mode="edge")
        c = np.cumsum(pad, axis=1)
        a = (c[:, 2 * r:, :] - c[:, :-2 * r, :]) / (2.0 * r)
        pad = np.pad(a, ((r, r), (0, 0), (0, 0)), mode="edge")
        c = np.cumsum(pad, axis=0)
        a = (c[2 * r:, :, :] - c[:-2 * r, :, :]) / (2.0 * r)
    return a


def blackbody(t):
    """t∈[0,1] → 火色 RGB（暗红→红→橙→黄→白）。t 任意形状。"""
    stops_t = np.array([0.0, 0.18, 0.45, 0.72, 1.0], np.float32)
    stops_r = np.array([0.05, 0.85, 1.00, 1.00, 1.00], np.float32)
    stops_g = np.array([0.00, 0.10, 0.45, 0.85, 1.00], np.float32)
    stops_b = np.array([0.00, 0.02, 0.05, 0.30, 0.95], np.float32)
    r = np.interp(t, stops_t, stops_r)
    g = np.interp(t, stops_t, stops_g)
    b = np.interp(t, stops_t, stops_b)
    return np.stack([r, g, b], axis=-1)


# ---------------- 体积渲染 ----------------

def render_flipbook(vol, mode, tiles):
    nframes, nz, ny, nx, channels = vol.shape
    dens = vol[..., 0].copy()
    temp = vol[..., 1] if channels >= 2 else np.zeros_like(dens)

    # 渲染参数（经验值；火与烟不同）。
    if mode == "fire":
        sigma_scale = 34.0
        emit_gain = 3.0
        temp_scale = 1.15
        beta = 1.3          # alpha 对比度（去灰雾）
    else:  # smoke：只按密度做不透明度，颜色留白（引擎端染黑/灰）
        sigma_scale = 22.0
        emit_gain = 0.0
        temp_scale = 0.0
        beta = 1.2

    # 阈值：抹掉微弱密度（否则整格填一层灰雾 → 广告牌露方块）。
    dens[dens < 0.03] = 0.0

    ds = 1.0 / float(nz)
    frames = []
    for fidx in range(nframes):
        d = dens[fidx]      # (nz, ny, nx)
        tp = temp[fidx]
        T = np.ones((ny, nx), np.float32)
        L = np.zeros((ny, nx, 3), np.float32)
        for z in range(nz):
            dz = d[z]
            a = 1.0 - np.exp(-dz * sigma_scale * ds)
            if emit_gain > 0.0:
                col = blackbody(np.clip(tp[z] * temp_scale, 0.0, 1.0))
                L += T[..., None] * a[..., None] * col * emit_gain
            else:
                L += T[..., None] * a[..., None] * np.array([1.0, 1.0, 1.0], np.float32)
            T = T * (1.0 - a)
        alpha = np.clip(1.0 - T, 0.0, 1.0)
        alpha = np.clip(alpha * beta, 0.0, 1.0)
        img = np.zeros((ny, nx, 4), np.float32)
        img[..., :3] = np.clip(L, 0.0, 1.0)
        img[..., 3] = alpha
        if mode == "fire":
            # 生命包络：点火(t=0 无) → 峰值 → 熄灭(t=1 无)。
            # 配合引擎端 SheetTimeMode=kLifetime：每粒随自身寿命「烧起来→灭掉」。
            t = fidx / float(max(1, nframes - 1))
            env = min(t / 0.28, (1.0 - t) / 0.38)
            env = 0.0 if env < 0.0 else (1.0 if env > 1.0 else env)
            env = env * env * (3.0 - 2.0 * env)   # smoothstep
            img[..., 3] *= env
            img[..., :3] *= (0.45 + 0.55 * env)   # 熄灭时整体变暗
        img = box_blur(img, 3, 3)   # 软化边缘（减方块感）
        # 图像行 0 = 体积 y 高端（火苗尖朝上）→ 上下翻转一次。
        frames.append(img[::-1, :, :])

    # 用全序列的 alpha bbox 裁剪（去掉体积域周围的空白），再放进统一 cell。
    any_a = np.zeros((ny, nx), np.float32)
    for img in frames:
        any_a = np.maximum(any_a, img[..., 3])
    ys, xs = np.nonzero(any_a > 0.02)
    if len(ys) == 0:
        y0, y1, x0, x1 = 0, ny, 0, nx
    else:
        y0, y1 = int(ys.min()), int(ys.max()) + 1
        x0, x1 = int(xs.min()), int(xs.max()) + 1
    if mode == "fire":
        # 「小火团」：裁一个**正方形**小块（边长 = 火宽的较小者），竖直放在火焰最亮处。
        # 每格 = 一小团火（一段火焰），靠大量小粒子叠加成火 —— 而不是每粒一根整火柱。
        w = x1 - x0
        h = y1 - y0
        side = max(16, min(w, h))
        cx = (x0 + x1) // 2
        tot = float(any_a.sum())
        if tot > 0.0:
            yy = np.arange(ny, dtype=np.float32)[:, None]
            yc = int((any_a * yy).sum() / tot)
        else:
            yc = (y0 + y1) // 2
        x0 = max(0, cx - side // 2)
        x1 = min(nx, x0 + side)
        x0 = max(0, x1 - side)
        y0 = max(0, yc - side // 2)
        y1 = min(ny, y0 + side)
        y0 = max(0, y1 - side)
    ch = y1 - y0
    cw = x1 - x0
    print("crop x[%d:%d] y[%d:%d] side=%d -> cell %dx%d" % (x0, x1, y0, y1, side if mode == 'fire' else -1, cw, ch))
    cell = int(max(ch, cw) * 1.30) + 2   # cell 含 ~30% 透明内边距

    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for i, img in enumerate(frames):
        if i >= tiles * tiles:
            break
        crop = img[y0:y1, x0:x1, :]
        row = i // tiles
        col = i % tiles
        oy = cell - ch - 1                 # 底部对齐（火根在下）
        ox = (cell - cw) // 2              # 水平居中
        sheet[row * cell + oy:row * cell + oy + ch,
              col * cell + ox:col * cell + ox + cw, :] = crop

    rgba = (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    return rgba


# ---------------- PNG 写出（滤波 0，zlib）----------------

def write_png(path, rgba):
    h, w, _ = rgba.shape
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += rgba[y].tobytes()

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    mode = sys.argv[1]
    in_path = sys.argv[2]
    out_path = sys.argv[3]
    tiles = int(sys.argv[4]) if len(sys.argv) > 4 else 8

    if mode in ("puff", "smokepuff"):
        kind = "smoke" if mode == "smokepuff" else "fire"
        rgba = render_puff(tiles, 128, kind)
        write_png(out_path, rgba)
        print("wrote %s (%s, %dx%d)" % (out_path, kind, rgba.shape[1], rgba.shape[0]))
        return 0

    vol = load_jpovvol(in_path)
    print("volume: frames=%d nz=%d ny=%d nx=%d ch=%d" % vol.shape)
    dens = vol[..., 0]
    print("density: max=%.3f mean=%.4f  nonzero>%.02f: %.2f%%" % (
        dens.max(), dens.mean(), 0.02, 100.0 * (dens > 0.02).mean()))
    if vol.shape[-1] >= 2:
        print("temperature: max=%.3f mean=%.4f" % (vol[..., 1].max(), vol[..., 1].mean()))

    rgba = render_flipbook(vol, mode, tiles)
    write_png(out_path, rgba)
    print("wrote %s  (%dx%d)" % (out_path, rgba.shape[1], rgba.shape[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
