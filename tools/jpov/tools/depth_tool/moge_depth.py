#!/usr/bin/env python3
"""JPOV depth tool — 用 MoGe-3 从单张图估计深度 / 点图 / 法线。

输入一张 RGB 图，输出：
    depth.npy            原始 float32 深度（米，MoGe-3 为相对/近度量尺度）
    depth_16bit.png      按 p1~p99 归一化到 0..65535 的 16-bit 深度
    depth_gray.png       8-bit 灰度可视化（默认近=白；--far-white 反过来）
    normal_gl.png        切线空间法线（OpenGL 约定，+Y 朝上）
    mask.png             有效区域掩码（模型有输出时才有）
    meta.json            模型 / 参数 / 深度统计，便于复现与排查

关于内存：MoGe-3 ViT-L 权重 1.48 GB。默认用 mmap + assign 加载，
权重留在 page cache 而不是匿名内存——容器 cgroup 上限较小时（例如 6 GB）
这是唯一能跑起来的方式，直接 from_pretrained 会被 OOM killer 杀掉。

用法:
    python moge_depth.py INPUT.png --out-dir OUT [--max-side 2048] [--reslv 9]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

import cv2
import numpy as np

# 必须在 import torch 之前设置，决定权重缓存位置
os.environ.setdefault("HF_HOME", str(Path.home() / ".cache" / "huggingface"))

import torch  # noqa: E402  (依赖上面的环境变量)
from huggingface_hub import hf_hub_download  # noqa: E402
from moge.model.v3 import MoGeModel  # noqa: E402

DEFAULT_MODEL = "Ruicheng/moge-3-vitl"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="MoGe-3 单图深度/法线估计（JPOV depth tool）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("input", type=Path, help="输入图片（png/jpg）")
    parser.add_argument("--out-dir", type=Path, required=True, help="输出目录（不存在会自动创建）")
    parser.add_argument("--model", default=DEFAULT_MODEL, help="HuggingFace 模型仓库名")
    parser.add_argument("--max-side", type=int, default=2048,
                        help="推理前把长边缩到不超过该值（0 表示不缩放）。"
                             "越大越吃显存，2048 在 16GB 卡上很宽裕")
    parser.add_argument("--reslv", type=int, default=9, choices=range(0, 10), metavar="[0-9]",
                        help="MoGe 的 resolution_level，越大细节越多越慢")
    parser.add_argument("--refine", type=int, default=2,
                        help="MoGe-3 稀疏细化步数，0 关闭")
    parser.add_argument("--device", default="cuda", help="cuda / cuda:0 / cpu")
    parser.add_argument("--fp16", action="store_true", help="半精度推理（更快，个别算子可能不稳）")
    parser.add_argument("--far-white", action="store_true",
                        help="深度可视化改成远=白（默认近=白）")
    parser.add_argument("--no-normal", action="store_true", help="跳过法线输出")
    return parser.parse_args(argv)


def load_model(repo: str, device: str) -> MoGeModel:
    """加载 MoGe-3，用 mmap 避免把 1.5GB 权重全塞进匿名内存。"""
    ckpt_path = hf_hub_download(repo, "model.pt")
    try:
        checkpoint = torch.load(ckpt_path, map_location="cpu", weights_only=True, mmap=True)
    except (RuntimeError, TypeError) as exc:
        # 非 zip 格式的 checkpoint 不支持 mmap，退回普通加载
        print(f"[warn] mmap 加载失败（{exc}），改用常规加载", file=sys.stderr)
        checkpoint = torch.load(ckpt_path, map_location="cpu", weights_only=True)
    model = MoGeModel(**checkpoint["model_config"])
    model.load_state_dict(checkpoint["model"], assign=True)
    del checkpoint
    return model.to(device).eval()


def read_image(path: Path, max_side: int) -> tuple[np.ndarray, tuple[int, int], float]:
    """读 RGB 并可选缩放。返回 (rgb float32 HxWx3 [0,1], 原尺寸, 缩放系数)。"""
    bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if bgr is None:
        raise SystemExit(f"error: 读不到图片 '{path}'")
    src_h, src_w = bgr.shape[:2]
    scale = 1.0
    if max_side > 0 and max(src_h, src_w) > max_side:
        scale = max_side / float(max(src_h, src_w))
        new_w = max(1, int(round(src_w * scale)))
        new_h = max(1, int(round(src_h * scale)))
        bgr = cv2.resize(bgr, (new_w, new_h), interpolation=cv2.INTER_AREA)
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    return rgb, (src_w, src_h), scale


def write_gray(depth: np.ndarray, mask: np.ndarray, path: Path,
               far_white: bool) -> tuple[float, float, np.ndarray]:
    """按 p1~p99 归一化写 8-bit 灰度图。返回 (lo, hi, 0..1 的归一化数组)。"""
    valid = depth[mask]
    lo = float(np.percentile(valid, 1))
    hi = float(np.percentile(valid, 99))
    norm = np.clip((depth - lo) / max(hi - lo, 1e-9), 0.0, 1.0)
    norm[~mask] = 0.0
    display = 1.0 - norm if not far_white else norm
    cv2.imwrite(str(path), (display * 255).astype(np.uint8))
    return lo, hi, norm


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.input.is_file():
        print(f"error: 输入不存在 '{args.input}'", file=sys.stderr)
        return 2
    if args.device.startswith("cuda") and not torch.cuda.is_available():
        print("error: 指定了 --device cuda 但 torch.cuda 不可用；改用 --device cpu 或先装 CUDA 版 torch",
              file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    rgb, src_size, scale = read_image(args.input, args.max_side)
    height, width = rgb.shape[:2]
    print(f"input  {src_size[0]}x{src_size[1]} -> infer {width}x{height} (scale {scale:.3f})")

    started = time.time()
    model = load_model(args.model, args.device)
    print(f"model  {args.model} loaded in {time.time() - started:.1f}s")

    tensor = torch.tensor(rgb, device=args.device).permute(2, 0, 1)
    if args.fp16:
        tensor = tensor.half()
    with torch.no_grad():
        started = time.time()
        out = model.infer(tensor, resolution_level=args.reslv, refine_steps=args.refine,
                          use_fp16=args.fp16)
        if args.device.startswith("cuda"):
            torch.cuda.synchronize()
        infer_ms = (time.time() - started) * 1000.0
    print(f"infer  {infer_ms:.0f} ms")

    depth = out["depth"].detach().float().cpu().numpy()
    mask = out["mask"].detach().cpu().numpy().astype(bool) if "mask" in out else np.ones(depth.shape, bool)
    if not mask.any():
        # 模型极端情况下可能给出全 False 掩码；此时按全图有效处理，避免空数组百分位崩溃
        print("[warn] 模型返回空掩码，按全图有效处理", file=sys.stderr)
        mask = np.ones(depth.shape, bool)
    if args.no_normal:
        normal_cam = None
    else:
        normal_cam = out["normal"].detach().float().cpu().numpy()
    del out
    if args.device.startswith("cuda"):
        torch.cuda.empty_cache()

    np.save(args.out_dir / "depth.npy", depth)
    lo, hi, norm = write_gray(depth, mask, args.out_dir / "depth_gray.png", args.far_white)

    cv2.imwrite(str(args.out_dir / "depth_16bit.png"), (norm * 65535).astype(np.uint16))
    cv2.imwrite(str(args.out_dir / "mask.png"), (mask * 255).astype(np.uint8))

    if normal_cam is not None:
        # MoGe 输出 OpenCV 相机系 (x 右, y 下, z 前)；切线空间 OpenGL 约定是 (x 右, y 上, z 出)
        normal_gl = np.stack([normal_cam[..., 0], -normal_cam[..., 1], -normal_cam[..., 2]], axis=-1)
        normal_gl /= np.maximum(np.linalg.norm(normal_gl, axis=-1, keepdims=True), 1e-9)
        cv2.imwrite(str(args.out_dir / "normal_gl.png"),
                    cv2.cvtColor(((normal_gl * 0.5 + 0.5) * 255).astype(np.uint8), cv2.COLOR_RGB2BGR))

    valid = depth[mask]
    meta = {
        "tool": "jpov_depth_tool/moge_depth",
        "model": args.model,
        "input": str(args.input),
        "source_size": list(src_size),
        "infer_size": [width, height],
        "scale": round(scale, 4),
        "reslv": args.reslv,
        "refine": args.refine,
        "fp16": bool(args.fp16),
        "device": args.device,
        "infer_ms": round(infer_ms),
        "torch": torch.__version__,
        "depth_stats": {
            "min": float(valid.min()),
            "p1": lo,
            "median": float(np.median(valid)),
            "p99": hi,
            "max": float(valid.max()),
        },
        "notes": "depth 单位为米；p1/p99 来自有效区域，depth_gray 用该范围归一化",
    }
    with open(args.out_dir / "meta.json", "w", encoding="utf-8") as handle:
        json.dump(meta, handle, indent=2, ensure_ascii=False)

    print(f"depth  min={valid.min():.4f} median={np.median(valid):.4f} max={valid.max():.4f}")
    print(f"saved  {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
