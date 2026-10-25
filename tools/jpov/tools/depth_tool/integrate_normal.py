#!/usr/bin/env python3
"""JPOV depth tool — 把切线空间法线图积分成高度场（Frankot-Chellappa）。

为什么需要它：JPOV 的 `jpov_texture_normal` 只产法线，不产高度。要做视差/位移/
真实几何，或者把法线与别的深度结果对照，都需要一个可积的高度场。

算法：在频域解 ∇h = (p, q) 的最小二乘解（Frankot & Chellappa, 1988）。
相比逐行累加，频域解不会沿积分路径累积误差，也不需要选积分路径。

输出：
    height.npy         float32 高度（任意单位，与法线斜率成正比）
    height_16bit.png   按 p1~p99 归一化的 16-bit
    height_gray.png    8-bit 预览（高=白）
    meta.json          参数 + 往返误差

自检（默认开启）：把高度重新求导成法线，与输入法线比对角误差。
这个往返误差是判断"符号/轴向搞反了"最直接的手段——搞反时误差会从几度跳到几十度。

用法:
    python integrate_normal.py NORMAL.png --out-dir OUT [--format gl|dx] [--detrend]
    python integrate_normal.py --selftest
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="法线图 → 高度场（Frankot-Chellappa 频域积分）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("normal", type=Path, nargs="?", help="输入法线图（png 8-bit RGB 或 npy HxWx3）")
    parser.add_argument("--out-dir", type=Path, help="输出目录")
    parser.add_argument("--format", choices=("gl", "dx"), default="gl",
                        help="输入法线约定：gl=+Y 朝上，dx=-Y 朝下（DirectX）")
    parser.add_argument("--detrend", action="store_true",
                        help="去除最佳拟合平面（平铺贴图/大幅倾斜时用）")
    parser.add_argument("--no-check", action="store_true", help="跳过往返误差检查")
    parser.add_argument("--selftest", action="store_true", help="跑合成数据自检后退出")
    return parser.parse_args(argv)


def load_normal(path: Path, convention: str) -> np.ndarray:
    """读法线并转成单位向量。约定统一到 GL（+Y 朝上）。"""
    if path.suffix.lower() == ".npy":
        data = np.load(path).astype(np.float32)
        if data.ndim != 3 or data.shape[2] != 3:
            raise SystemExit(f"error: npy 需要 HxWx3，实际 {data.shape}")
        normal = data.copy()
        # npy 约定为 [-1,1]；若是 [0,1] 则自动换算
        if normal.min() >= 0.0 and normal.max() <= 1.0:
            normal = normal * 2.0 - 1.0
    else:
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if image is None:
            raise SystemExit(f"error: 读不到法线图 '{path}'")
        normal = cv2.cvtColor(image, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0 * 2.0 - 1.0

    if convention == "dx":
        normal[..., 1] *= -1.0
    length = np.linalg.norm(normal, axis=-1, keepdims=True)
    return normal / np.maximum(length, 1e-9)


def integrate(normal: np.ndarray) -> np.ndarray:
    """Frankot-Chellappa：频域解 ∇h = (p, q)，p=∂h/∂x，q=∂h/∂行(向下)。"""
    nx, ny, nz = normal[..., 0], normal[..., 1], normal[..., 2]
    # 防止 nz→0 时斜率爆炸；保留符号，避免把背向面翻成正向
    nz = np.where(np.abs(nz) < 1e-4, np.where(nz < 0, -1e-4, 1e-4), nz)
    p = -nx / nz
    q = -ny / nz
    rows, cols = p.shape
    wx = np.fft.fftfreq(cols)[None, :] * 2.0 * np.pi
    wy = np.fft.fftfreq(rows)[:, None] * 2.0 * np.pi
    denom = wx ** 2 + wy ** 2
    denom[0, 0] = 1.0
    height_hat = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / denom
    height_hat[0, 0] = 0.0
    return np.real(np.fft.ifft2(height_hat)).astype(np.float32)


def normals_from_height(height: np.ndarray) -> np.ndarray:
    """用中心差分（周期边界）从高度重建法线，供往返检查。"""
    p = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    q = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    normal = np.stack([-p, -q, np.ones_like(p)], axis=-1)
    length = np.linalg.norm(normal, axis=-1, keepdims=True)
    return normal / np.maximum(length, 1e-9)


def mean_angle_deg(a: np.ndarray, b: np.ndarray) -> float:
    cos = np.clip((a * b).sum(-1), -1.0, 1.0)
    return float(np.degrees(np.arccos(cos)).mean())


def detrend_plane(height: np.ndarray) -> np.ndarray:
    rows, cols = height.shape
    ys, xs = np.mgrid[0:rows, 0:cols]
    design = np.c_[xs.ravel(), ys.ravel(), np.ones(rows * cols)]
    coeffs, *_ = np.linalg.lstsq(design, height.ravel().astype(np.float64), rcond=None)
    return (height.ravel() - design @ coeffs).reshape(rows, cols).astype(np.float32)


def write_outputs(height: np.ndarray, out_dir: Path, meta: dict) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    np.save(out_dir / "height.npy", height)
    lo, hi = np.percentile(height, 1), np.percentile(height, 99)
    norm = np.clip((height - lo) / max(hi - lo, 1e-9), 0.0, 1.0)
    cv2.imwrite(str(out_dir / "height_16bit.png"), (norm * 65535).astype(np.uint16))
    cv2.imwrite(str(out_dir / "height_gray.png"), (norm * 255).astype(np.uint8))
    meta.update({"height_p1": float(lo), "height_p99": float(hi)})
    with open(out_dir / "meta.json", "w", encoding="utf-8") as handle:
        json.dump(meta, handle, indent=2, ensure_ascii=False)


def selftest() -> int:
    """合成已知高度 → 算法线 → 积分回来。正例必须重合，反例必须失败。"""
    # 频率必须能整除画面尺寸：Frankot-Chellappa 假设周期边界，
    # 非整周期会在边界产生跳变，正例的相关系数就掉到 0.95 左右
    size = 256
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    truth = 0.6 * np.sin(2 * np.pi * xx / 64.0) + 0.4 * np.cos(2 * np.pi * yy / 32.0)

    p = 0.6 * (2 * np.pi / 64.0) * np.cos(2 * np.pi * xx / 64.0)
    q = -0.4 * (2 * np.pi / 32.0) * np.sin(2 * np.pi * yy / 32.0)
    normal = np.stack([-p, -q, np.ones_like(p)], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)

    # 正例：应找回真值（允许常数偏移，故先减均值再比）
    recovered = integrate(normal)
    a = (recovered - recovered.mean()).ravel()
    b = (truth - truth.mean()).ravel()
    corr = float((a @ b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12))

    # 反例：y 分量符号取反（等价于轴向搞反）必须明显失败
    flipped = normal.copy()
    flipped[..., 1] *= -1.0
    flipped /= np.linalg.norm(flipped, axis=-1, keepdims=True)
    bad = integrate(flipped)
    a_bad = (bad - bad.mean()).ravel()
    corr_bad = float((a_bad @ b) / (np.linalg.norm(a_bad) * np.linalg.norm(b) + 1e-12))

    roundtrip = mean_angle_deg(normals_from_height(recovered), normal)

    print(f"selftest: corr(good)={corr:.4f} (需 > 0.99)")
    print(f"selftest: corr(flipped)={corr_bad:.4f} (需 < 0.90)")
    print(f"selftest: roundtrip mean angle={roundtrip:.3f} deg")

    ok = corr > 0.99 and corr_bad < 0.90 and roundtrip < 1.0
    print("selftest: PASS" if ok else "selftest: FAIL")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.selftest:
        return selftest()
    if args.normal is None or args.out_dir is None:
        print("error: 需要 NORMAL 和 --out-dir（或用 --selftest）", file=sys.stderr)
        return 2
    if not args.normal.is_file():
        print(f"error: 输入不存在 '{args.normal}'", file=sys.stderr)
        return 2

    normal = load_normal(args.normal, args.format)
    height = integrate(normal)

    meta = {
        "tool": "jpov_depth_tool/integrate_normal",
        "input": str(args.normal),
        "format": args.format,
        "detrend": bool(args.detrend),
        "size": [int(height.shape[1]), int(height.shape[0])],
    }
    if args.detrend:
        height = detrend_plane(height)

    if not args.no_check:
        error = mean_angle_deg(normals_from_height(height), normal)
        meta["roundtrip_mean_angle_deg"] = round(error, 3)
        print(f"roundtrip mean angle = {error:.3f} deg  (>10 度通常意味着符号/轴向搞错了)")

    write_outputs(height, args.out_dir, meta)
    print(f"saved {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
