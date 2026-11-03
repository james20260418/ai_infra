#!/usr/bin/env python3
"""morph_flow.py — FLOW（Farneback 稠密光流）两帧形变。

这是 JPOV「F5 → S5 → flipbook」产线的形变算子。**FLOW 是唯一验收通过的算法**：

  * 火焰/烟当年也试过 PROFILE（逐行中心线+半宽），它在「竖直柱」形状上更稳，
    但遇到 C 形/星形/斜带（水花就是）会把分叉并成一根"主柱"，出来偏糊偏"充气"。
  * 所以：**竖直单体形状可考虑 PROFILE；其它一律 FLOW**。本工具只保留 FLOW。

接口（与产线其它脚本一致）：

    morph(img_a, img_b, t) -> np.ndarray   # (H,W) float32 [0,1]，t=0/1 逐像素等于入参

输入约定：**单通道 alpha 形状**（白形黑底取亮度，或 RGBA 取 alpha），
两张图必须同尺寸、同一画布坐标（先把每格归一到同尺寸画布再调这里）。
"""

from __future__ import annotations

import cv2
import numpy as np

__all__ = ["morph", "PARAMS"]

# Farneback 参数：JPOV 火焰/水花都用这一组（改了要重新验收出图）。
PARAMS = dict(pyr_scale=0.5, levels=4, winsize=31, iterations=5,
              poly_n=7, poly_sigma=1.5)


def _as_float(img: np.ndarray) -> np.ndarray:
    a = np.asarray(img)
    if a.dtype == np.uint8:
        a = a.astype(np.float32) / 255.0
    return np.clip(a.astype(np.float32), 0.0, 1.0)


def _remap(img: np.ndarray, map_x: np.ndarray, map_y: np.ndarray) -> np.ndarray:
    """双线性反向采样；画布外视为透明（0）。"""
    return cv2.remap(img, map_x.astype(np.float32), map_y.astype(np.float32),
                     interpolation=cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=0.0)


def morph(img_a: np.ndarray, img_b: np.ndarray, t: float,
          blur_sigma: float = 2.0) -> np.ndarray:
    """A→B 在 t∈[0,1] 处的中间形状。

    做法：对 A→B、B→A 各算一次 Farneback 光流，各自沿光流反向 warp 到中间时刻，
    再按 t 线性融合。两端 t=0/1 直接返回入参（逐像素相等，保证图集首尾就是原图）。

    blur_sigma：算光流前的高斯模糊（软遮罩 1.5~3 合适；太大丢细节、太小光流会抖）。
    """
    a, b = _as_float(img_a), _as_float(img_b)
    if a.shape != b.shape:
        raise ValueError(f"两张图尺寸必须一致：{a.shape} vs {b.shape}")
    if a.ndim != 2:
        raise ValueError("morph 只吃单通道 (H, W)")
    t = float(np.clip(t, 0.0, 1.0))
    if t <= 0.0:
        return a.copy()
    if t >= 1.0:
        return b.copy()

    a8 = cv2.GaussianBlur((a * 255).astype(np.uint8), (0, 0), blur_sigma)
    b8 = cv2.GaussianBlur((b * 255).astype(np.uint8), (0, 0), blur_sigma)
    flow_ab = cv2.calcOpticalFlowFarneback(a8, b8, None, flags=0, **PARAMS)
    flow_ba = cv2.calcOpticalFlowFarneback(b8, a8, None, flags=0, **PARAMS)

    h, w = a.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    warp_a = _remap(a, xx - t * flow_ab[..., 0], yy - t * flow_ab[..., 1])
    warp_b = _remap(b, xx - (1.0 - t) * flow_ba[..., 0],
                    yy - (1.0 - t) * flow_ba[..., 1])
    return np.clip((1.0 - t) * warp_a + t * warp_b, 0.0, 1.0)
