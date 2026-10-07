#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
处理土地（dirt）地板纹理，使其满足 tileable / 周期性 + 自产法线贴图。

输入:   dirt_diff_1k.jpg        (Poly Haven forest_ground_04 diffuse, CC0)
输出:
  - dirt_diff_tileable.png     (可无缝平铺的 baseColor, 512x512)
  - dirt_normal.png            (从灰度推导的法线贴图, 512x512, OpenGL 约定)

约定 (对齐 JPOV 采样):
  - JPOV: stbi_load 不翻转上传, V=0=图片顶部, 与 glTF 左上角 UV 一致.
    因此本脚本输出的 png 直接按"V=0=顶部"的自然方向保存即可,
    loader 直接透传 UV, 不额外翻转.
  - 法线贴图: RGB 中 G 通道在 OpenGL 传统约定下 Y 向下(与 glTF 一致: Y-up 法线,
    但 JPOV 用 Z-up + TBN)。此处生成的是标准 "OpenGL 风格" 切线空间法线
    (R=+X右, G=+Y上, B=+Z), shader 里经 TBN 变换。
    注意: 与 T_Brick_Normal (medieval kit) 的约定是否一致需在渲染中验证。

步�骤:
  1. 读入 diffuse, 转灰度作为高度场 (或直接用 luminance)
  2. 用"偏移加权平均(offset stitch)"把图像边缘做成无缝 (tileable)
  3. 从高度场用 central difference 生成法线贴图
  4. 写出 tileable baseColor + normal
"""
import numpy as np
from PIL import Image, ImageFilter

SRC   = "dirt_diff_1k.jpg"
BASE  = "dirt_diff_tileable.png"
NORM  = "dirt_normal.png"
OUT_W = 512
OUT_H = 512

# --- 1. 读入 diffuse 并缩放 ---
im = Image.open(SRC).convert("RGB")
im = im.resize((OUT_W, OUT_H), Image.LANCZOS)
arr = np.asarray(im).astype(np.float32) / 255.0  # [0,1]

def make_seamless(a, blend=16):
    """
    通过"偏移缝合法(offset stitch)"使图像在 0<->1 边界无缝 (tileable)。

    原理: 先做 ±(h/2, w/2) 的循环移位副本, 再在四条边缘带内与原图
    crossfade, 让对向边缘的图案互相平滑过渡, 消除接缝。
    对带大块岩石/碎片的纹理效果远优于简单的窄带边缘混合。
    """
    h, w, c = a.shape
    rolled = np.roll(a, (h // 2, w // 2), axis=(0, 1))
    out = a.copy()
    for i in range(blend):
        t = (i + 1) / (blend + 1)  # 0..1 渐入
        out[i, :, :]          = (1 - t) * a[i, :, :]          + t * rolled[i, :, :]
        out[h - 1 - i, :, :]  = (1 - t) * a[h - 1 - i, :, :]  + t * rolled[h - 1 - i, :, :]
        out[:, i, :]          = (1 - t) * a[:, i, :]          + t * rolled[:, i, :]
        out[:, w - 1 - i, :]  = (1 - t) * a[:, w - 1 - i, :]  + t * rolled[:, w - 1 - i, :]
    return out

tiled = make_seamless(arr, blend=48)
tiled = np.clip(tiled, 0.0, 1.0)

# --- 2. 生成法线贴图 (从灰度高度场) ---
gray = np.asarray(Image.fromarray((tiled * 255).astype(np.uint8)).convert("L")).astype(np.float32) / 255.0

# 手动 central-difference 梯度 (Sobel 3x3), 无需 scipy
h, w = gray.shape
gx = np.zeros_like(gray)
gy = np.zeros_like(gray)
# gx: 水平梯度 (x 方向差分)
gx[1:-1,1:-1] = (gray[1:-1,2:]   - gray[1:-1,:-2]) / 2.0
# gy: 垂直梯度 (y 方向差分, 图片顶部为 y=0, 向下增大)
gy[1:-1,1:-1] = (gray[2:,1:-1]   - gray[:-2,1:-1]) / 2.0

strength = 15.0   # 法线扰动强度
nx = -gx * strength
ny = -gy * strength
nz = np.ones_like(gray)
norm = np.sqrt(nx*nx + ny*ny + nz*nz)
norm[norm < 1e-8] = 1.0
N = np.stack([nx/norm, ny/norm, nz/norm], axis=-1)
normal_8 = ((N * 0.5 + 0.5) * 255).astype(np.uint8)

# --- 3. 写出 ---
Image.fromarray((tiled * 255).astype(np.uint8)).save(BASE)
Image.fromarray(normal_8).save(NORM)
print("写出:", BASE, "和", NORM, f"({OUT_W}x{OUT_H})")
print("法线贴图 B 通道均值(应≈1):", normal_8[...,2].mean()/255.0)
