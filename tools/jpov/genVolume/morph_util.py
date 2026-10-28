#!/usr/bin/env python3
"""morph_util.py — 通过验收的两种 2D 形变（图像域「矢量形变」）工具。

只暴露两个形变算法（其余实验方法留在 morph_bench.py）：

    morph_optical_flow(img_a, img_b, t, **kw)   # FLOW：稠密光流
    morph_profile(img_a, img_b, t)              # PROFILE：中心线+宽度

另有三个辅助：
    morph_frames(...)      # 一次生成 N 帧
    make_flipbook_sheet(...)  # 拼 8×4 atlas（含 GL 行序处理）
    recommend_frames(...)  # 按寿命/fps 推荐帧数

------------------------------ 输入 / 输出约定 ------------------------------
输入：
    img_a, img_b : np.ndarray, shape (H, W), dtype float32（或可转 float32）
                   **单通道**灰度/alpha，值域 [0, 1]。
                   两张图**必须同尺寸**、同一构图（同一画布坐标）。
    t            : float，0.0 ~ 1.0。
                   t=0 返回 img_a **原样**，t=1 返回 img_b 原样（逐像素一致）。
输出：
    np.ndarray, shape (H, W), dtype float32，值域 [0, 1]（已 clip）。

------------------------------ 帧率 / 帧数 ------------------------------
粒子 flipbook 的经验法则（与 JPOV 的 SheetTimeMode::kLifetime 对齐）：
    * 一个粒子寿命里播放 **1 个完整形变**（不是循环）。
    * 推荐 N = 24 ~ 32 帧；30 fps 时 32 帧 ≈ 1.07 s，正好覆盖常见火苗寿命 0.7~1.1 s。
    * 帧时间戳用 t_i = i / (N - 1)（i=0..N-1），保证**末帧就是目标形状**。
      如果要循环播放（首尾同形），改用 t_i = i / N。
    * 贴图播放 fps 与寿命的关系：fps = N / lifetime_seconds。
    * 需要「慢放观察」时不要改 t 的分布，直接降播放 fps 即可。

------------------------------ 用法示例 ------------------------------
    from morph_util import morph_frames, make_flipbook_sheet
    frames = morph_frames(alpha_f1, alpha_f5, n_frames=32, method="flow")
    sheet = make_flipbook_sheet(frames, tiles_x=8)   # -> (1024, 2048, 4) uint8
    Image.fromarray(sheet).save("f1_to_f5_8x4.png")

依赖：numpy、opencv-python（cv2）。PROFILE 只需要 numpy + cv2.remap。
"""

from __future__ import annotations

from typing import List, Optional

import cv2
import numpy as np

__all__ = [
    "morph_optical_flow",
    "morph_profile",
    "morph_frames",
    "make_flipbook_sheet",
    "recommend_frames",
]


# --------------------------------------------------------------------------- #
# 内部工具
# --------------------------------------------------------------------------- #
def _check_pair(img_a: np.ndarray, img_b: np.ndarray) -> tuple:
    # uint8 输入按 0..255 处理，自动归一化；float 输入按 [0,1]
    if isinstance(img_a, np.ndarray) and img_a.dtype == np.uint8:
        img_a = img_a.astype(np.float32) / 255.0
    if isinstance(img_b, np.ndarray) and img_b.dtype == np.uint8:
        img_b = img_b.astype(np.float32) / 255.0
    a = np.clip(np.asarray(img_a, dtype=np.float32), 0.0, 1.0)
    b = np.clip(np.asarray(img_b, dtype=np.float32), 0.0, 1.0)
    if a.ndim != 2 or b.ndim != 2:
        raise ValueError("img_a/img_b 必须是单通道 (H, W)")
    if a.shape != b.shape:
        raise ValueError(f"两张图尺寸必须一致：{a.shape} vs {b.shape}")
    return a, b


def _remap(img: np.ndarray, map_x: np.ndarray, map_y: np.ndarray) -> np.ndarray:
    """双线性反向采样；画布外视为 0（透明）。"""
    return cv2.remap(img, map_x.astype(np.float32), map_y.astype(np.float32),
                     interpolation=cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=0.0)


def _row_profile(alpha: np.ndarray):
    """逐行统计 (x_center, half_width, top, bottom)。

    用于 PROFILE：把形状看成「竖直广义柱」——每行一个中心 + 一个半宽。
    """
    h, w = alpha.shape
    xs = np.arange(w, dtype=np.float32)
    xc = np.full(h, w * 0.5, np.float32)
    hw = np.zeros(h, np.float32)
    occ = np.zeros(h, bool)
    for y in range(h):
        row = alpha[y]
        s = row.sum()
        if s < 1e-3:
            continue
        occ[y] = True
        c = float((row * xs).sum() / s)
        var = float((row * (xs - c) ** 2).sum() / s)
        xc[y] = c
        hw[y] = max(2.0 * np.sqrt(max(var, 0.0)), 1.0)
    ys = np.where(occ)[0]
    if len(ys) == 0:
        return xc, hw, (0, h - 1)
    return xc, hw, (int(ys.min()), int(ys.max()))


# --------------------------------------------------------------------------- #
# 算法 1：FLOW（Farneback 稠密光流）
# --------------------------------------------------------------------------- #
def morph_optical_flow(img_a: np.ndarray, img_b: np.ndarray, t: float, *,
                       blur_sigma: float = 2.0,
                       levels: int = 4,
                       win: int = 31,
                       iters: int = 5,
                       poly_n: int = 7,
                       poly_sigma: float = 1.5) -> np.ndarray:
    """光流形变：A/B 各自沿稠密光流 warp 到中间时刻，再按 t 融合。

    原理：
        对 A→B、B→A 各算一次 Farneback 光流；
        A 的采样坐标 = x - t * flow_AB(x)（反向映射），B 对称；
        结果 = (1-t)·warpA + t·warpB。
    优点：不需要任何手工特征点，形状差异大也能拿到平滑的对应场。
    缺点：当两形状有遮挡/拓扑变化时，中段可能出现轻微双影（可接受）。

    参数：
        img_a, img_b : (H,W) float32 [0,1] 单通道。
        t            : 0..1；0/1 直接返回原图。
        blur_sigma   : 算光流前的高斯模糊半径（像素）。输入是软遮罩时 1.5~3 合适；
                       太大（>4）会丢掉细结构，太小（<1）光流会抖。
        levels       : 金字塔层数，默认 4。形变位移大时加层数。
        win          : 搜索窗口（像素），默认 31；越大越稳、越慢。
        iters        : 每层迭代次数，默认 5。
        poly_n       : 多项式展开邻域，默认 7（常用 5/7）。
        poly_sigma   : 多项式高斯 σ，默认 1.5。

    返回：
        (H,W) float32 [0,1]。
    """
    a, b = _check_pair(img_a, img_b)
    t = float(np.clip(t, 0.0, 1.0))
    if t <= 0.0:
        return a.copy()
    if t >= 1.0:
        return b.copy()

    a8 = cv2.GaussianBlur((np.clip(a, 0, 1) * 255).astype(np.uint8), (0, 0), blur_sigma)
    b8 = cv2.GaussianBlur((np.clip(b, 0, 1) * 255).astype(np.uint8), (0, 0), blur_sigma)
    params = dict(pyr_scale=0.5, levels=levels, winsize=win, iterations=iters,
                  poly_n=poly_n, poly_sigma=poly_sigma, flags=0)
    flow_ab = cv2.calcOpticalFlowFarneback(a8, b8, None, **params)
    flow_ba = cv2.calcOpticalFlowFarneback(b8, a8, None, **params)

    h, w = a.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    warp_a = _remap(a, xx - t * flow_ab[..., 0], yy - t * flow_ab[..., 1])
    warp_b = _remap(b, xx - (1.0 - t) * flow_ba[..., 0],
                    yy - (1.0 - t) * flow_ba[..., 1])
    return np.clip((1.0 - t) * warp_a + t * warp_b, 0.0, 1.0)


# --------------------------------------------------------------------------- #
# 算法 2：PROFILE（中心线 + 宽度）
# --------------------------------------------------------------------------- #
def morph_profile(img_a: np.ndarray, img_b: np.ndarray, t: float) -> np.ndarray:
    """轴+宽度形变：逐行插值「中心线位置」和「半宽」，再做水平仿射采样。

    原理（真·矢量形变，可解释性最好）：
        对每个形状统计每行的 x_center(y) 与 half_width(y)；
        中间帧：
            xc_t(y) = lerp(xc_a, xc_b)
            w_t(y)  = lerp(w_a,  w_b)
            上下边界 y_top/y_bottom 也分别 lerp；
        反向采样 A：
            src_x = xc_a + (x - xc_t) · (w_a / w_t)
            src_y = 该行在 A 中对应的高度；
        B 同理，最后 (1-t)/t 融合。
    特点：
        * 端点严格等于原图（t=0/1 直接返回）；
        * 不会折、不会撕裂；但只适合**大致竖直**的形状（火焰、烟柱、树）。
        * 如果形状有分叉或多连通块，逐行统计会把它们并成一条「主柱」，
          所以分叉明显的素材请用 FLOW。

    参数：
        img_a, img_b : (H,W) float32 [0,1] 单通道。
        t            : 0..1；0/1 直接返回原图。

    返回：
        (H,W) float32 [0,1]。
    """
    a, b = _check_pair(img_a, img_b)
    t = float(np.clip(t, 0.0, 1.0))
    if t <= 0.0:
        return a.copy()
    if t >= 1.0:
        return b.copy()

    xc_a, hw_a, (top_a, bot_a) = _row_profile(a)
    xc_b, hw_b, (top_b, bot_b) = _row_profile(b)
    h, w = a.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)

    top_t = (1 - t) * top_a + t * top_b
    bot_t = (1 - t) * bot_a + t * bot_b
    # 归一化高度：0=顶，1=底；按中间帧的上下边界算
    u = np.clip((yy - top_t) / max(bot_t - top_t, 1e-3), 0.0, 1.0)

    def prof_at(xc, hw, top, bot):
        y = top + u * (bot - top)
        yi = np.clip(y, 0, h - 1)
        y0 = np.floor(yi).astype(np.int32)
        y1 = np.minimum(y0 + 1, h - 1)
        f = yi - y0
        return (xc[y0] * (1 - f) + xc[y1] * f,
                np.maximum(hw[y0] * (1 - f) + hw[y1] * f, 1.0),
                y)

    c_a, w_a, y_a = prof_at(xc_a, hw_a, top_a, bot_a)
    c_b, w_b, y_b = prof_at(xc_b, hw_b, top_b, bot_b)
    c_t = (1 - t) * c_a + t * c_b
    w_t = (1 - t) * w_a + t * w_b

    def warp(img, c, ww, y):
        src_x = c + (xx - c_t) * (ww / w_t)
        return _remap(img, src_x, y)

    wa = warp(a, c_a, w_a, y_a)
    wb = warp(b, c_b, w_b, y_b)
    return np.clip((1 - t) * wa + t * wb, 0.0, 1.0)


# --------------------------------------------------------------------------- #
# 辅助：批量出帧 / 拼 atlas / 帧数建议
# --------------------------------------------------------------------------- #
def morph_frames(img_a: np.ndarray, img_b: np.ndarray, n_frames: int = 32,
                 method: str = "flow", **kwargs) -> List[np.ndarray]:
    """生成 n_frames 帧形变序列。

    参数：
        img_a, img_b : (H,W) float32 [0,1]。
        n_frames     : 帧数，推荐 24~32（见文件顶部的帧率说明）。
        method       : "flow" 或 "profile"。
        **kwargs     : 透传给对应算法（例如 flow 的 blur_sigma/win）。

    帧时间分布（重要）：
        t_i = i / (n_frames - 1)  → 首帧 = A，末帧 = B（单向形变，推荐）。
        若要循环（首=尾），请自己用 i / n_frames 调这个函数或后处理。
    """
    a, b = _check_pair(img_a, img_b)
    if n_frames < 2:
        raise ValueError("n_frames 必须 >= 2")
    fn = {"flow": morph_optical_flow, "profile": morph_profile}.get(method.lower())
    if fn is None:
        raise ValueError(f"未知 method={method!r}，可选 'flow' / 'profile'")
    out = []
    for i in range(n_frames):
        t = i / float(n_frames - 1)
        out.append(fn(a, b, t, **kwargs))
    return out


def make_flipbook_sheet(frames: List[np.ndarray], tiles_x: int = 8,
                        flip_rows: bool = True) -> np.ndarray:
    """把帧拼成 atlas（默认 8 列，行数自动），返回 uint8 RGBA。

    行序（重要）：
        JPOV 的 CellUV 用 `vrow = tilesY-1-row`，而 GL 上传时 PNG 第一行对应 v=0，
        所以 **viewer 的 frame 0 采样的是 PNG 的最后一行**。
        flip_rows=True（默认）会按这个约定把 frame 0 写在最后一行。
        如果只是导出视频/序列帧，传 flip_rows=False。

    参数：
        frames   : 同尺寸 (H,W) float32 [0,1] 的列表，长度 <= tiles_x * tiles_y。
        tiles_x  : 列数，默认 8；行数 = ceil(len(frames)/tiles_x)。
        flip_rows: 是否按 GL/viewer 行序翻转（见上）。

    返回：
        (tiles_y*H, tiles_x*W, 4) uint8，RGB=白，A=形变结果。
        空槽位为全透明。
    """
    if not frames:
        raise ValueError("frames 不能为空")
    h, w = frames[0].shape
    tiles_y = int(np.ceil(len(frames) / float(tiles_x)))
    sheet = np.zeros((tiles_y * h, tiles_x * w, 4), np.uint8)
    for i, f in enumerate(frames):
        if f.shape != (h, w):
            raise ValueError("所有帧尺寸必须一致")
        r, c = divmod(i, tiles_x)
        rr = (tiles_y - 1 - r) if flip_rows else r
        v = (np.clip(f, 0, 1) * 255 + 0.5).astype(np.uint8)
        y0, x0 = rr * h, c * w
        sheet[y0:y0 + h, x0:x0 + w, 0:3] = 255
        sheet[y0:y0 + h, x0:x0 + w, 3] = v
    return sheet


def recommend_frames(lifetime_seconds: float, fps: float = 30.0,
                     min_frames: int = 16, max_frames: int = 32) -> int:
    """按「一个寿命播放一遍」给推荐帧数。

    例：lifetime=0.9 s, fps=30 → 27 帧（落在 16~32 内）。
    返回值为 clamp(round(lifetime*fps), min_frames, max_frames)。
    """
    n = int(round(float(lifetime_seconds) * float(fps)))
    return int(np.clip(n, min_frames, max_frames))


# --------------------------------------------------------------------------- #
# 命令行 demo
# --------------------------------------------------------------------------- #
if __name__ == "__main__":
    import argparse
    import os
    from PIL import Image

    ap = argparse.ArgumentParser(description="F1→F5 形变 demo")
    ap.add_argument("--a", default="tools/jpov/assets/fire/mp/mask_1.png")
    ap.add_argument("--b", default="tools/jpov/assets/fire/mp/mask_5.png")
    ap.add_argument("--method", default="flow", choices=["flow", "profile"])
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--out", default="/tmp/morph_demo.png")
    args = ap.parse_args()

    def load(p):
        im = Image.open(p).convert("RGBA")
        al = np.array(im, dtype=np.float32)[..., 3] / 255.0
        if al.max() < 1e-3:
            al = np.array(im.convert("L"), dtype=np.float32) / 255.0
        return al

    frames = morph_frames(load(args.a), load(args.b),
                          n_frames=args.frames, method=args.method)
    sheet = make_flipbook_sheet(frames, tiles_x=8)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    Image.fromarray(sheet).save(args.out)
    print(f"wrote {args.out} {sheet.shape} ({len(frames)} frames)")
