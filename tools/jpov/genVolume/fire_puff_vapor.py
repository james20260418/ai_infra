#!/usr/bin/env python3
"""fire_puff_vapor.py — 「透明状火焰雾」火团 flipbook（对齐 Danis 的描述）。

目标不是火球、也不是火丝，而是**高速摄像机下火团形成初期的透明状雾**：
  * 湍流上升的**薄雾/薄片**，纹理像蕾丝/烟一样撕裂；
  * **90%+ 的像素是透光的**（alpha 很低），只有极少数「炽热点」是亮的；
  * 颜色：暗处偏红/暗橙，热点才发黄白；整体远看是「一团发光的雾」而不是「一块火」。

做法：可循环 3D value-noise 湍流（向上平流 + 双频混合）→ ridge 变换出**细丝/薄片**结构
→ 椭球包络收成一团 → 渲染时用**很小的消光系数**，让透射率保持很高。

用法：
  python3 fire_puff_vapor.py --out /tmp/vapor.png [--cell 256] [--frames 32] \
      [--grid 96] [--sigma 3.0] [--gain 1.6] [--ridge 3.0] [--hot 1.0]
"""

import argparse

import numpy as np

from fire_puff_sim import _lattice, fbm
from make_flipbook import blackbody, box_blur, write_png


def build_fields(args):
    n = args.grid
    F = args.frames
    g = (np.arange(n, dtype=np.float32) + 0.5) / n
    zz = g[:, None, None]
    yy = g[None, :, None]
    xx = g[None, None, :]

    lat_a = _lattice(11, 20)
    lat_b = _lattice(22, 20)
    lat_c = _lattice(33, 20)

    dens = np.zeros((F, n, n, n), np.float32)
    temp = np.zeros_like(dens)

    # 包络：一个小团（不是球，是压扁一点、底部更宽）
    ex = (xx - 0.5) / 0.34
    ey = (yy - 0.5) / 0.34
    ez = (zz - 0.42) / 0.46
    env = np.exp(-(ex * ex + ey * ey + ez * ez) * 1.8)
    env *= np.clip(1.15 - zz * 0.9, 0.0, 1.0)      # 越往上越稀

    for f in range(F):
        t = f / float(F)
        # 向上平流（mod 1 → 严格无缝循环）；y = 高度方向
        gz = ((g - args.speed * t) % 1.0) * args.freq
        gy = g * args.freq
        gx = g * args.freq

        # 三个尺度：大团包络 / 垂直拉丝的薄片 / 高频蕾丝细节
        n_lo = fbm(lat_a, gx * 2.2, gy * 1.6, gz * 2.2, octaves=3)
        n_mid = fbm(lat_b, gx * 7.0, gy * 2.6, gz * 7.0, octaves=3)
        n_hi = fbm(lat_c, gx * 15.0, gy * 7.0, gz * 15.0, octaves=2)

        wisp = np.clip(1.0 - np.abs(2.0 * n_mid - 1.0), 0.0, 1.0) ** 2.2
        lace = np.clip(1.0 - np.abs(2.0 * n_hi - 1.0), 0.0, 1.0) ** 1.6

        # 不规则剪影：用低频噪声切出一个“团”的轮廓（不是光滑椭球）
        gate = np.clip((n_lo - 0.34) / 0.26, 0.0, 1.0)
        d = wisp * (0.35 + 1.05 * lace) * gate * env
        d = np.clip((d - 0.06) / 0.94, 0.0, 1.0)

        dens[f] = d
        # 温度只在「最密的细丝」上高：smoothstep(d) → 少量白热点，其余暗红
        hot = np.clip((d - 0.30) / 0.45, 0.0, 1.0) ** 0.9
        temp[f] = np.clip(hot * args.hot * (1.0 - 0.45 * zz), 0.0, 1.0)

    return dens, temp


def render(dens, temp, args):
    F, nz, ny, nx = dens.shape
    ds = 1.0 / float(nz)
    frames = []
    stats = []
    for f in range(F):
        d = dens[f]
        tp = temp[f]
        T = np.ones((ny, nx), np.float32)
        L = np.zeros((ny, nx, 3), np.float32)
        Lmax = np.zeros((ny, nx, 3), np.float32)
        for z in range(nz):
            dz = d[z]
            a = 1.0 - np.exp(-dz * args.sigma * ds)
            col = blackbody(np.clip(tp[z] * 1.25, 0.0, 1.0))
            L += T[..., None] * a[..., None] * col * args.gain
            if args.proj == "max":
                Lmax = np.maximum(Lmax, col * np.clip(tp[z] * 1.6, 0.0, 1.0)[..., None])
            T = T * (1.0 - a)
        if args.proj == "max":
            L = L * 0.35 + Lmax * args.gain
        alpha = np.clip(1.0 - T, 0.0, 1.0)
        img = np.zeros((ny, nx, 4), np.float32)
        img[..., :3] = np.clip(L, 0.0, 1.0)
        img[..., 3] = alpha
        if args.blur > 0:
            img = box_blur(img, args.blur, 1)
        frames.append(img[::-1, :, :])
        stats.append(alpha)

    a_all = np.concatenate([s.ravel() for s in stats])
    cov = a_all > 0.02
    print("[vapor] alpha: mean=%.3f  p50=%.3f  p90=%.3f  max=%.3f  "
          "覆盖=%.1f%%  其中 alpha<0.3 占覆盖的 %.1f%%" % (
              float(a_all.mean()), float(np.percentile(a_all, 50)),
              float(np.percentile(a_all, 90)), float(a_all.max()),
              100.0 * float(cov.mean()),
              (100.0 * float((a_all[cov] < 0.3).mean()) if cov.any() else 0.0)))

    # 统一 bbox 裁剪、居中，放进 cell
    any_a = np.max(np.stack(stats), axis=0)
    ys, xs = np.nonzero(any_a > 0.02)
    if len(ys) == 0:
        ys, xs = np.arange(ny), np.arange(nx)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    side = max(y1 - y0, x1 - x0)
    cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
    y0 = max(0, cy - side // 2)
    x0 = max(0, cx - side // 2)

    cell, tiles = args.cell, args.tiles
    sheet = np.zeros((tiles * cell, tiles * cell, 4), np.float32)
    for i, img in enumerate(frames):
        crop = img[y0:y0 + side, x0:x0 + side, :]
        k = max(1, side // cell)
        if k > 1 and side // k == cell:
            small = crop.reshape(cell, k, cell, k, 4).mean(axis=(1, 3))
        else:
            idx_y = np.linspace(0, side - 1, cell).astype(np.int32)
            idx_x = np.linspace(0, side - 1, cell).astype(np.int32)
            small = crop[np.ix_(idx_y, idx_x)]
        r, c = i // tiles, i % tiles
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell, :] = small
    return (np.clip(sheet, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--tiles", type=int, default=8)
    p.add_argument("--cell", type=int, default=256)
    p.add_argument("--grid", type=int, default=96)
    p.add_argument("--frames", type=int, default=32)
    p.add_argument("--sigma", type=float, default=3.0)
    p.add_argument("--gain", type=float, default=1.6)
    p.add_argument("--ridge", type=float, default=3.0)
    p.add_argument("--hot", type=float, default=1.0)
    p.add_argument("--hot-sharp", type=float, default=2.0)
    p.add_argument("--freq", type=float, default=5.0)
    p.add_argument("--speed", type=float, default=1.0)
    p.add_argument("--blur", type=int, default=0)
    p.add_argument("--proj", type=str, default="eray")
    args = p.parse_args()

    dens, temp = build_fields(args)
    rgba = render(dens, temp, args)
    write_png(args.out, rgba)
    print("wrote", args.out, rgba.shape)


if __name__ == "__main__":
    main()
