#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
程序化生成"砖铺地面"的无缝 tileable 纹理 + 法线贴图 (brick_floor.png + brick_normal.png)。

设计目标 (对齐 Danis 需求):
  - 砖铺地面, 大小可调, 无缝平铺 (tileable/周期性)
  - 程序化生成, 不依赖外部下载 (medieval kit 的砖纹理下载一直截断, 故改自产)
  - 法线贴图从砖的凹凸高度场自产
  - 全局 MR: 调用方直接给 metallic/roughness 常值, 本贴图只出 baseColor + normal

图案:
  - 标准错缝砖墙 (running bond): 每行砖宽 w, 高 h, 相邻行错开 w/2
  - 水泥凹缝 (mortar) 在下沉, 砖面微凸 → 生成高度场做法线
  - 每块砖加轻微噪声/颜色变化增加真实感

输出: 512x512 (2x2 平铺显示时看不出接缝)
"""
import numpy as np
from PIL import Image

SIZE = 512
BRICK_W = 64   # 砖宽(px)
BRICK_H = 32   # 砖高(px)
MORTAR = 4     # 灰缝宽(px)
SEED = 123

def brick_heightfield(w, h):
    """生成砖铺高度场 (0=灰缝最低, 1=砖面最高), tileable。"""
    H = np.zeros((h, w), dtype=np.float32)
    rows = h // BRICK_H
    cols = w // BRICK_W
    rng = np.random.default_rng(SEED)
    for r in range(rows):
        y0 = r * BRICK_H
        off = (BRICK_W // 2) if (r % 2 == 1) else 0  # 错缝
        for c in range(cols + 1):
            x0 = c * BRICK_W - off
            # 砖所在矩形 (考虑循环)
            xa, xb = x0 + MORTAR//2, x0 + BRICK_W - MORTAR//2
            ya, yb = y0 + MORTAR//2, y0 + BRICK_H - MORTAR//2
            # 用 mask 填充该砖区域的砖面高度 (1.0 微凸)
            for y in range(ya, min(yb, h)):
                for x in range(xa, min(xb, w)):
                    yy, xx = y % h, ((x % w) + w) % w
                    H[yy, xx] = 1.0
    return H

def make_brick_color(H):
    """从高度场生成砖色 baseColor (红色砖 + 灰缝 + 微噪声)。"""
    h, w = H.shape
    rng = np.random.default_rng(SEED)
    base = np.zeros((h, w, 3), dtype=np.float32)
    brick_col = np.array([0.72, 0.35, 0.28])   # 红砖
    mortar_col = np.array([0.55, 0.52, 0.50])   # 灰泥
    # 每块砖的随机色调变化
    per_brick = np.ones((h, w, 1), dtype=np.float32)
    for y in range(h):
        for x in range(w):
            is_mortar = H[y, x] < 0.5
            if is_mortar:
                base[y, x] = mortar_col
            else:
                # 轻微砖间色差
                base[y, x] = brick_col * (0.9 + 0.2 * rng.random())
    # 细颗粒噪声
    noise = (rng.random((h, w, 1)).astype(np.float32) - 0.5) * 0.04
    base = np.clip(base + noise, 0, 1)
    return base

def make_normal(H, strength=15.0):
    """从高度场生成法线贴图。"""
    h, w = H.shape
    gx = np.zeros_like(H); gy = np.zeros_like(H)
    gx[:, 1:] = (H[:, 1:] - H[:, :-1])
    gx[:, 0] = gx[:, -1]
    gy[1:, :] = (H[1:, :] - H[:-1, :])
    gy[0, :] = gy[-1, :]
    nx = -gx * strength
    ny = -gy * strength
    nz = np.ones_like(H)
    norm = np.sqrt(nx*nx + ny*ny + nz*nz)
    norm[norm < 1e-8] = 1
    N = np.stack([nx/norm, ny/norm, nz/norm], axis=-1)
    return ((N*0.5+0.5)*255).astype(np.uint8)

H = brick_heightfield(SIZE, SIZE)
col = make_brick_color(H)
nrm = make_normal(H)
Image.fromarray((col*255).astype(np.uint8)).save("brick_floor.png")
Image.fromarray(nrm).save("brick_normal.png")
print("生成 brick_floor.png + brick_normal.png:", SIZE, "x", SIZE)

# 验证无缝性
a = (col*255).astype(np.uint8).astype(float)
h,w,_=a.shape
print("上下边缘差:", abs(a[0] - a[-1]).mean().round(2), "| 左右边缘差:", abs(a[:,0]-a[:,-1]).mean().round(2))
