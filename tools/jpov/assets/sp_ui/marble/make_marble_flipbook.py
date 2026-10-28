#!/usr/bin/env python3
"""make_marble_flipbook.py — 把 N 张大理石关键帧内插成一张**周期** flipbook 图集。

用法：
    python3 make_marble_flipbook.py --out plate_marble_flipbook_4x4.png \\
        --frames 16 --tiles 4 --cell 256 --ease smoothstep marble_sync_*.png

输入必须是**已经做过周期性（seamless）处理**的等尺寸关键帧（见 README.md 第 2 步）。
输出是一张 `tiles × tiles` 网格图集，**帧 0 在左上角**、行主序 —— 与引擎
`CellUV()` 的约定一致（`vrow = tiles_y-1-row` 翻转后，帧 0 落在图片左上）。

算法（两段，顺序不可交换）：
  1. **关键帧内插**：把 N 张关键帧当成环上的 N 个控制点，用**周期 Catmull-Rom**
     逐像素插值；段内相位 t 先过一条 **profile 曲线**（`--ease`）再喂给插值。
     这与火焰链（`genVolume/make_flipbook.py`、`Gradient`/`Curve` 那套）同构：
     profile 只决定「帧在两个关键姿态之间怎么走」，不改变插值本身。
  2. **周期性**：Catmull-Rom 是**逐像素仿射**的，而仿射组合保持可平铺性
     ⇒ 只要输入每张都平铺无缝，输出的每一帧也平铺无缝；控制点首尾相连
     ⇒ 帧序列本身周期（末帧回到首帧连续，无跳变）。
     所以**必须先 seamless 再内插**，反了就前功尽弃。

为什么用 Catmull-Rom 而不是线性：线性只过控制点、一阶不连续，帧序列在关键帧处
会出现「速度突变」，慢速播放时看得见一顿；Catmull-Rom 过点且切线连续，观感是
连续流动的大理石水纹。
"""

import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image


def apply_gain(frames, median_lum, gain, target):
    """围绕中位亮度做线性增益：out = target + (v - median) * gain，截到 [0,1]。

    为什么不用 levels 拉伸：源图 50% 的像素挤在 ±0.005 的白平台里，任何
    [lo,hi]→[0,1] 的拉伸都会把平台以上的细节（也就是水纹本身）整段剪掉，
    只剩几条深色石纹。围绕中位数放大则**保留全部结构**，只把对比度抬上来。

    target = 白平台落到哪里：取 0.9 而不是 0.5，是为了让「tint 就是板材亮度」
    这个直觉成立（tint=0.42 的板子就是 ~0.38），否则整套主题颜色得反过来重新标。

    所有帧用同一组 (median, gain, target)：逐帧自适应会在 flipbook 里变成闪电式闪烁。
    仿射映射保持可平铺性与周期闭环。
    """
    if gain <= 0.0:
        sys.exit(f"错误：gain 必须 > 0（收到 {gain}）")
    if not (0.0 < target <= 1.0):
        sys.exit(f"错误：gain 目标亮度必须 ∈ (0,1]（收到 {target}）")
    out = []
    for f in frames:
        out.append(np.clip(target + (f - median_lum) * gain, 0.0, 1.0))
    return out


def apply_levels(frames, lo, hi):
    """对全部关键帧做同一套亮度拉伸（levels）。

    为什么必须要这一步：生图产出的“白大理石”实际上 95% 的像素挤在 0.96~0.98，
    作为 UI 底材乘上 tint 后纹理直接归零（见 README 的量化说明）。把
    [lo, hi] 映射到 [0, 1] 后纹路才立得起来。

    关键：**所有关键帧用同一组 lo/hi**。逐帧自适应会变成“每帧自己变亮暗”，
    在 flipbook 里就是闪电式闪烁。仿射映射也保持可平铺性与周期闭环。
    """
    if hi <= lo:
        sys.exit(f"错误：levels 上界({hi})必须大于下界({lo})")
    out = []
    for f in frames:
        out.append(np.clip((f - lo) / (hi - lo), 0.0, 1.0))
    return out


def load_keyframes(paths, white_balance):
    """读入关键帧为 float32 [0,1] 的 HxWx3 数组。

    white_balance=True 时对每张做灰世界白平衡（各通道增益归到全体均值），
    抵消“某张关键帧天生偏暖/偏冷”—— 否则内插经过那张时整个帧序列会跟着泛色。

    Pre-condition: 所有图尺寸一致；至少 3 张（Catmull-Rom 需要 4 个邻点去重后>=3）。
    """
    if len(paths) < 3:
        sys.exit(f"错误：至少需要 3 张关键帧（当前 {len(paths)} 张）")

    frames = []
    size = None
    for p in paths:
        im = Image.open(p).convert("RGB")
        if size is None:
            size = im.size
        elif im.size != size:
            sys.exit(f"错误：关键帧尺寸不一致 {p}={im.size} 期望 {size}")
        frames.append(np.asarray(im, dtype=np.float32) / 255.0)

    if white_balance:
        means = np.stack([f.reshape(-1, 3).mean(axis=0) for f in frames])  # Nx3
        target = means.mean(axis=0)
        for i, f in enumerate(frames):
            gain = target / np.maximum(means[i], 1e-6)
            frames[i] = np.clip(f * gain.reshape(1, 1, 3), 0.0, 1.0)
            print(f"  白平衡 key[{i}]: 原均值 {means[i].round(4).tolist()} "
                  f"→ 增益 {gain.round(3).tolist()}")
    return frames, size



def ease_phase(t, mode):
    """段内相位 t ∈ [0,1] 的 profile 曲线。

    t=0 / t=1 必须精确映射到 0 / 1 —— 否则帧序列不再经过关键帧，观感会「跑偏」。
    """
    if mode == "linear":
        return t
    if mode == "smoothstep":
        return t * t * (3.0 - 2.0 * t)
    if mode == "smootherstep":
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
    sys.exit(f"错误：未知 ease 模式 {mode}（可选 linear / smoothstep / smootherstep）")


def catmull_rom(p0, p1, p2, p3, t):
    """逐像素三次 Catmull-Rom（复数同理）。t ∈ [0,1] 落在 p1→p2 段。"""
    t2 = t * t
    t3 = t2 * t
    return 0.5 * (
        (2.0 * p1)
        + (-p0 + p2) * t
        + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2
        + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3
    )


def build_frames(keys, frames, ease):
    """在环上均匀插值出 `frames` 帧。返回 HxWx3 float32 列表。"""
    n = len(keys)
    out = []
    for f in range(frames):
        # u ∈ [0,1) 沿整圈走；k_u ∈ [0,n) 落在「关键帧 k → k+1」段内。
        u = float(f) / float(frames)
        k_u = u * float(n)
        i = int(np.floor(k_u))
        t = ease_phase(k_u - float(i), ease)
        # 周期取邻点：模 n 环绕，保证首尾连续（这就是「周期内插」）。
        p0 = keys[(i - 1) % n]
        p1 = keys[i % n]
        p2 = keys[(i + 1) % n]
        p3 = keys[(i + 2) % n]
        out.append(catmull_rom(p0, p1, p2, p3, t))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("keys", nargs="+", help="已 seamless 化的关键帧 PNG（按顺序）")
    ap.add_argument("--out", required=True, help="输出图集 PNG 路径")
    ap.add_argument("--frames", type=int, default=16, help="输出总帧数（默认 16）")
    ap.add_argument("--tiles", type=int, default=4, help="图集列数=行数（默认 4）")
    ap.add_argument("--cell", type=int, default=256, help="每格像素边长（默认 256）")
    ap.add_argument("--ease", default="smoothstep",
                    help="段内 profile 曲线：linear / smoothstep / smootherstep")
    ap.add_argument("--no-white-balance", action="store_true",
                    help="关闭「灰世界白平衡」（默认开：把各关键帧均值归一，避免经过偏色帧时整条序列泛色）")
    # 亮度对比：（默认）围绕中位亮度放大 gain 倍；也可用 levels 拉伸或 off。
    # 详见 apply_gain 的注释：levels 会剪掉白平台上的水纹细节。
    ap.add_argument("--contrast", default="gain:6",
                    help="对比度模式：gain:<k>（默认 6）/ levels:lo,hi / off")
    ap.add_argument("--gain-target", type=float, default=0.90,
                    help="gain 模式下白平台落到的亮度（默认 0.90）")
    ap.add_argument("--levels", default="auto",
                    help="旧参数（仅在 --contrast 以 levels 开头时使用）；保留兼容")
    args = ap.parse_args()

    paths = []
    for k in args.keys:
        expanded = sorted(glob.glob(k))
        paths.extend(expanded if expanded else [k])
    if args.tiles * args.tiles != args.frames:
        sys.exit(f"错误：frames({args.frames}) 必须等于 tiles²({args.tiles}×{args.tiles})")

    keys, size = load_keyframes(paths, white_balance=not args.no_white_balance)

    if args.contrast != "off":
        lum_all = np.concatenate(
            [(0.2126 * f[..., 0] + 0.7152 * f[..., 1] + 0.0722 * f[..., 2]).ravel()
             for f in keys])
        if args.contrast.startswith("gain:"):
            gain = float(args.contrast.split(":", 1)[1])
            median = float(np.median(lum_all))
            print(f"对比度 gain: median={median:.4f} gain={gain:.1f} "
                  f"target={args.gain_target:.2f}")
            keys = apply_gain(keys, median, gain, args.gain_target)
        elif args.contrast.startswith("levels:"):
            parts = [float(x) for x in args.contrast.split(":", 1)[1].split(",")]
            if len(parts) != 2:
                sys.exit(f"错误：--contrast levels:lo,hi 需要两个值，收到 {args.contrast}")
            print(f"对比度 levels: [{parts[0]:.4f}, {parts[1]:.4f}] → [0, 1]")
            keys = apply_levels(keys, parts[0], parts[1])
        else:
            sys.exit(f"错误：未知 --contrast 模式 {args.contrast}")
    print(f"关键帧 {len(keys)} 张，源尺寸 {size[0]}x{size[1]}，"
          f"输出 {args.frames} 帧 / {args.tiles}x{args.tiles} / 每格 {args.cell}px / ease={args.ease}")

    imgs = build_frames(keys, args.frames, args.ease)

    sheet = Image.new("RGB", (args.tiles * args.cell, args.tiles * args.cell))
    for f, arr in enumerate(imgs):
        cell = Image.fromarray(
            np.clip(arr * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8), "RGB"
        ).resize((args.cell, args.cell), Image.LANCZOS)
        col = f % args.tiles
        row = f // args.tiles          # 行主序：帧 0 在左上
        sheet.paste(cell, (col * args.cell, row * args.cell))

    sheet.save(args.out)
    print(f"已写出 {args.out}（{sheet.size[0]}x{sheet.size[1]}）")


if __name__ == "__main__":
    main()
