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
        beta = 1.8          # alpha 对比度（去灰雾）
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
    ch = y1 - y0
    cw = x1 - x0
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
