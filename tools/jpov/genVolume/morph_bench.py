#!/usr/bin/env python3
"""morph_bench.py — F1→F5 的四种图像内插（矢量形变）对比。

不接 JPOV，只比较「差值本身」。四种方法（都保证 t=0 是 F1、t=1 是 F5）：

  1. BN   — Beier–Neely 1992 feature-line field morphing
            自动从 alpha 的逐行质心提取「脊线」作为特征线对；
            两张图各自 warp 到中间形状，再 cross-dissolve。
  2. TPS  — Thin-Plate Spline landmark warp（Wolberg 的经典做法）
            取轮廓点（按弧长等距重采样 + 归一化弧长配对）做 TPS 反向映射。
  3. FLOW — 稠密光流（Farneback）warp + dissolve
            不做显式特征，靠 dense correspondence。
  4. SDF  — signed distance field / level-set 插值
            两个形状的 SDF 线性插值，再取零等值面 → 轮廓自然过渡。

用法：
  python3 morph_bench.py --a mask_1.png --b mask_5.png --out /tmp/morph_bench
"""

import argparse
import os

import cv2
import numpy as np
from scipy.interpolate import LinearNDInterpolator
from scipy.spatial import cKDTree
from scipy.interpolate import RBFInterpolator
from PIL import Image, ImageDraw

N_FRAMES = 7          # t = 0, 1/6, ..., 1
CELL = 256


# ---------------------------------------------------------------- 基础工具
def load_alpha(path, S=CELL):
    im = Image.open(path).convert("RGBA")
    a = np.array(im, dtype=np.float32)[..., 3] / 255.0
    if a.max() < 1e-3:
        a = np.array(im.convert("L"), dtype=np.float32) / 255.0
    return cv2.resize(a, (S, S), interpolation=cv2.INTER_AREA)


def spine_points(alpha, n=13):
    """逐行质心 → 从底到顶的脊线点 (x,y)。"""
    ys = np.where(alpha.max(axis=1) > 0.05)[0]
    if len(ys) == 0:
        return np.zeros((n, 2), np.float32)
    y0, y1 = ys.min(), ys.max()
    out = []
    for y in np.linspace(y1, y0, n):          # 底 → 顶
        yi = int(round(y))
        row = alpha[yi]
        s = row.sum()
        if s < 1e-4:
            out.append([CELL * 0.5, y])
            continue
        xc = float((row * np.arange(CELL)).sum() / s)
        out.append([xc, y])
    return np.array(out, np.float32)


def contour_points(alpha, n=72):
    mask = (alpha > 0.5).astype(np.uint8)
    cnts, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    if not cnts:
        return np.zeros((n, 2), np.float32)
    c = max(cnts, key=cv2.contourArea).reshape(-1, 2).astype(np.float32)
    # 统一方向（按 signed area 取逆时针），再从最低点起，保证两张图同向配对
    area = 0.5 * float(np.sum(c[:, 0] * np.roll(c[:, 1], -1) -
                              np.roll(c[:, 0], -1) * c[:, 1]))
    if area < 0:
        c = c[::-1]
    start = int(np.argmax(c[:, 1]))
    c = np.roll(c, -start, axis=0)
    # 按弧长等距重采样
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(c, axis=0), axis=1))]
    if d[-1] < 1e-6:
        return np.repeat(c[:1], n, axis=0)
    u = np.linspace(0, d[-1], n, endpoint=False)
    x = np.interp(u, d, c[:, 0])
    y = np.interp(u, d, c[:, 1])
    return np.stack([x, y], axis=1).astype(np.float32)


def remap(img, map_x, map_y):
    return cv2.remap(img, map_x.astype(np.float32), map_y.astype(np.float32),
                     interpolation=cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=0)


# ---------------------------------------------------------------- 1. Beier-Neely
def bn_warp(src, src_lines, dst_lines, a=0.001, b=1.6, p=0.5):
    """把 src 沿 src_lines → dst_lines 的映射 warp 到目标坐标系。"""
    h, w = src.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dsum_x = np.zeros((h, w), np.float32)
    dsum_y = np.zeros((h, w), np.float32)
    wsum = np.zeros((h, w), np.float32)
    for (ps, qs), (pd, qd) in zip(src_lines, dst_lines):
        qp = qd - pd
        L2 = float(qp[0] ** 2 + qp[1] ** 2)
        if L2 < 1e-6:
            continue
        L = np.sqrt(L2)
        u = ((xx - pd[0]) * qp[0] + (yy - pd[1]) * qp[1]) / L2
        v = ((xx - pd[0]) * (-qp[1]) + (yy - pd[1]) * qp[0]) / L
        # 目标像素到该线段的距离
        uc = np.clip(u, 0.0, 1.0)
        proj_x = pd[0] + uc * qp[0]
        proj_y = pd[1] + uc * qp[1]
        dist = np.hypot(xx - proj_x, yy - proj_y)
        weight = (L ** p / (a + dist)) ** b
        # 源线段坐标
        qsp = qs - ps
        Ls = max(float(np.hypot(qsp[0], qsp[1])), 1e-6)
        sx = ps[0] + u * qsp[0] + v * (-qsp[1]) / Ls
        sy = ps[1] + u * qsp[1] + v * (qsp[0]) / Ls
        dsum_x += weight * sx
        dsum_y += weight * sy
        wsum += weight
    wsum[wsum < 1e-8] = 1e-8
    return remap(src, dsum_x / wsum, dsum_y / wsum)


def morph_bn(A, B, t):
    pa, pb = spine_points(A), spine_points(B)
    lines_a = [(pa[i], pa[i + 1]) for i in range(len(pa) - 1)]
    lines_b = [(pb[i], pb[i + 1]) for i in range(len(pb) - 1)]
    lines_t = [((1 - t) * np.array(la[0]) + t * np.array(lb[0]),
                (1 - t) * np.array(la[1]) + t * np.array(lb[1]))
               for la, lb in zip(lines_a, lines_b)]
    wa = bn_warp(A, lines_a, lines_t)
    wb = bn_warp(B, lines_b, lines_t)
    return (1 - t) * wa + t * wb


# ---------------------------------------------------------------- 2. TPS
def tps_fit(dst_pts, src_pts, lam=1e-3):
    """求 TPS 映射：dst_pts → src_pts（用于反向采样）。"""
    n = len(dst_pts)
    d = np.linalg.norm(dst_pts[:, None, :] - dst_pts[None, :, :], axis=2)
    K = d * d * np.log(d + 1e-6)
    K += np.eye(n) * lam
    P = np.c_[np.ones(n), dst_pts]
    L = np.zeros((n + 3, n + 3), np.float32)
    L[:n, :n] = K
    L[:n, n:] = P
    L[n:, :n] = P.T
    rhs = np.zeros((n + 3, 2), np.float32)
    rhs[:n] = src_pts
    sol = np.linalg.solve(L, rhs)
    return sol[:n], sol[n:]


def tps_eval(pts, ctrl_pts, coef_w, coef_a):
    """pts: 要评估的点（可很多）；ctrl_pts: 拟合时的控制点。"""
    d = np.linalg.norm(pts[:, None, :] - ctrl_pts[None, :, :], axis=2)
    U = d * d * np.log(d + 1e-6)
    return U @ coef_w + np.c_[np.ones(len(pts)), pts] @ coef_a


def morph_tps(A, B, t, npts=48):
    ca = contour_points(A, npts)
    cb = match_contours(ca, contour_points(B, npts))
    # 加质心，稳住内部
    ga = np.array([[np.mean(ca[:, 0]), np.mean(ca[:, 1])]], np.float32)
    gb = np.array([[np.mean(cb[:, 0]), np.mean(cb[:, 1])]], np.float32)
    sa = spine_points(A, 9)
    sb = spine_points(B, 9)
    pa = np.vstack([ca, ga, sa])
    pb = np.vstack([cb, gb, sb])
    pt = (1 - t) * pa + t * pb

    # 目标网格 → 源 A / 源 B
    yy, xx = np.mgrid[0:A.shape[0], 0:A.shape[1]].astype(np.float32)
    grid = np.stack([xx.ravel(), yy.ravel()], axis=1)

    wa = np.zeros_like(A)
    wb = np.zeros_like(B)
    for src_img, p_src, out in ((A, pa, wa), (B, pb, wb)):
        cw, ca_ = tps_fit(pt, p_src, lam=200.0)
        mapped = tps_eval(grid, pt, cw, ca_)
        out[:] = remap(src_img, mapped[:, 0].reshape(A.shape),
                       mapped[:, 1].reshape(A.shape))
    return (1 - t) * wa + t * wb


# ---------------------------------------------------------------- 3. Optical flow
def morph_flow(A, B, t):
    a8 = cv2.GaussianBlur((np.clip(A, 0, 1) * 255).astype(np.uint8), (0, 0), 2.0)
    b8 = cv2.GaussianBlur((np.clip(B, 0, 1) * 255).astype(np.uint8), (0, 0), 2.0)
    ab = cv2.calcOpticalFlowFarneback(a8, b8, None, 0.5, 4, 31, 5, 7, 1.5, 0)
    ba = cv2.calcOpticalFlowFarneback(b8, a8, None, 0.5, 4, 31, 5, 7, 1.5, 0)
    h, w = A.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # A 向 B 走 t：采样 A(x - t*flow)
    wa = remap(A, xx - t * ab[..., 0], yy - t * ab[..., 1])
    wb = remap(B, xx - (1 - t) * ba[..., 0], yy - (1 - t) * ba[..., 1])
    return (1 - t) * wa + t * wb


# ---------------------------------------------------------------- 4. SDF
def _stats(alpha):
    ys, xs = np.nonzero(alpha > 0.3)
    if len(xs) == 0:
        return np.array([alpha.shape[1] * 0.5, alpha.shape[0] * 0.5]), 1.0
    c = np.array([xs.mean(), ys.mean()], np.float32)
    r = max(np.sqrt(len(xs) / np.pi), 1.0)     # 等效半径
    return c, r


def _sdf(mask):
    m = (mask > 0.15).astype(np.uint8)
    # 填内部洞（软笔触在 0.5 阈值下会留麻点/空洞）
    ff = m.copy()
    hh, ww = ff.shape
    fmask = np.zeros((hh + 2, ww + 2), np.uint8)
    cv2.floodFill(ff, fmask, (0, 0), 1)
    m = m | (1 - ff)
    din = cv2.distanceTransform(m, cv2.DIST_L2, 5)
    dout = cv2.distanceTransform(1 - m, cv2.DIST_L2, 5)
    return dout - din          # >0 外部，<0 内部


def morph_sdf(A, B, t, band=1.2):
    """SDF 形变（修复版）：

    1) 两张 SDF 先按质心/等效半径对齐，再线性插值（去掉平移/尺寸造成的错位）；
    2) lerp 后在 t≈0.5 会出现细颈断裂 → 对二值结果做**形态学闭运算**把颈接上；
    3) 用高斯羽化边缘，避免之前「硬二值块」的观感；
    4) 端点附近混合回原始图，保证 t→0/1 连续。
    """
    h, w = A.shape
    cA, rA = _stats(A)
    cB, rB = _stats(B)
    sA0, sB0 = _sdf(A), _sdf(B)

    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    ct = (1 - t) * cA + t * cB
    rt = (1 - t) * rA + t * rB
    ka, kb = rA / max(rt, 1e-3), rB / max(rt, 1e-3)

    xa = cA[0] + (xx - ct[0]) * ka
    ya = cA[1] + (yy - ct[1]) * ka
    xb = cB[0] + (xx - ct[0]) * kb
    yb = cB[1] + (yy - ct[1]) * kb

    sa = cv2.remap(sA0, xa, ya, cv2.INTER_LINEAR,
                   borderMode=cv2.BORDER_CONSTANT, borderValue=1000.0)
    sb = cv2.remap(sB0, xb, yb, cv2.INTER_LINEAR,
                   borderMode=cv2.BORDER_CONSTANT, borderValue=1000.0)
    st = (1 - t) * sa + t * sb

    soft = np.clip(0.5 - st / (2.0 * band), 0.0, 1.0)
    binm = (soft > 0.5).astype(np.uint8)
    if binm.any():
        # 闭运算把中间的细颈接上（端点处不改变形状）
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
        binm = cv2.morphologyEx(binm, cv2.MORPH_CLOSE, k)
    # 去掉漂浮碎块：只保留面积 >= 最大块 15% 的连通域
    if binm.any():
        ncomp, lab, stats, _ = cv2.connectedComponentsWithStats(binm)
        if ncomp > 1:
            areas = stats[1:, 4]
            amax = int(areas.max()) if len(areas) else 0
            keep = np.zeros_like(binm)
            for i in range(1, ncomp):
                if stats[i, 4] >= max(30, int(0.15 * amax)):
                    keep[lab == i] = 1
            binm = keep
    alpha = cv2.GaussianBlur(binm.astype(np.float32), (0, 0), 1.0)
    # 端点连续：t=0/1 时退回原始遮罩
    w_end = min(1.0, max(0.0, 1.0 - 4.0 * t * (1.0 - t)))
    orig = (1 - t) * A + t * B
    alpha = (1 - w_end) * alpha + w_end * orig
    return np.clip(alpha, 0.0, 1.0)

# ---------------------------------------------------------------- 5. 轴+宽度（profile warp）
def row_profile(alpha):
    """每行的 (x_center, half_width, occupied)。"""
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


def morph_profile(A, B, t):
    """按行把 A/B 的「中心线 + 宽度」插值后反向采样；端点精确。"""
    xcA, hwA, (tA, bA) = row_profile(A)
    xcB, hwB, (tB, bB) = row_profile(B)
    h, w = A.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # 中间形状的上下边界
    top_t = (1 - t) * tA + t * tB
    bot_t = (1 - t) * bA + t * bB
    u = np.clip((yy - top_t) / max(bot_t - top_t, 1e-3), 0.0, 1.0)

    def sample_profile(xc, hw, top, bot):
        y = top + u * (bot - top)
        yi = np.clip(y, 0, h - 1)
        y0 = np.floor(yi).astype(np.int32)
        y1 = np.minimum(y0 + 1, h - 1)
        f = yi - y0
        c = xc[y0] * (1 - f) + xc[y1] * f
        ww = hw[y0] * (1 - f) + hw[y1] * f
        return c, np.maximum(ww, 1.0), y

    cA, wA_, yA = sample_profile(xcA, hwA, tA, bA)
    cB, wB_, yB = sample_profile(xcB, hwB, tB, bB)
    c_t = (1 - t) * cA + t * cB
    w_t = (1 - t) * wA_ + t * wB_

    def warp(img, c, ww, y):
        sx = c + (xx - c_t) * (ww / w_t)
        return remap(img, sx, y)

    wa = warp(A, cA, wA_, yA)
    wb = warp(B, cB, wB_, yB)
    return (1 - t) * wa + t * wb



# ---------------------------------------------------------------- 6. MESH（三角网格 / 分片仿射）

def match_contours(ca, cb):
    """DTW 把两条轮廓按形状对齐（解决「从最低点按弧长配对」在大形变下错位）。

    返回重采样到 len(ca) 的 cb 对应点。
    """
    n, m = len(ca), len(cb)
    D = ((ca[:, None, :] - cb[None, :, :]) ** 2).sum(axis=2)
    C = np.full((n + 1, m + 1), np.inf, np.float32)
    C[0, 0] = 0
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            C[i, j] = D[i - 1, j - 1] + min(C[i - 1, j - 1], C[i - 1, j], C[i, j - 1])
    # 回溯
    i, j = n, m
    path = []
    while i > 0 and j > 0:
        path.append((i - 1, j - 1))
        step = np.argmin([C[i - 1, j - 1], C[i - 1, j], C[i, j - 1]])
        if step == 0:
            i, j = i - 1, j - 1
        elif step == 1:
            i -= 1
        else:
            j -= 1
    path.reverse()
    # 按 ca 的索引重采样 cb
    pi = np.array([p[0] for p in path], np.float32)
    pj = np.array([p[1] for p in path], np.float32)
    out = np.zeros_like(ca)
    for k in range(2):
        out[:, k] = np.interp(np.arange(n), pi, cb[pj.astype(np.int32), k])
    return out


def _interior_points(alpha, rows=5, cols=3):
    """在形状内部放规则点（沿每行中心 ±宽度比例），让三角网更密。"""
    xc, hw, (top, bot) = row_profile(alpha)
    pts = []
    for u in np.linspace(0.15, 0.85, rows):
        y = int(round(top + u * (bot - top)))
        for k in (-0.45, 0.0, 0.45):
            pts.append([xc[y] + k * hw[y], float(y)])
    return np.array(pts, np.float32)


def morph_mesh(A, B, t, npts=160):
    """三角网（分片仿射）形变：轮廓 + 脊线 + 内部点；凸包外直接置 0。"""
    ca = contour_points(A, npts)
    cb = match_contours(ca, contour_points(B, npts))
    pa_all = np.vstack([ca, spine_points(A, 13), _interior_points(A, rows=8, cols=5)])
    pb_all = np.vstack([cb, spine_points(B, 13), _interior_points(B, rows=8, cols=5)])
    # 只在 A 的 landmark 上去重（保持 pa/pb 一一对应）
    keep = []
    for i, p0 in enumerate(pa_all):
        if all((p0[0] - pa_all[k][0]) ** 2 + (p0[1] - pa_all[k][1]) ** 2 >= 9.0
               for k in keep):
            keep.append(i)
    pa = pa_all[keep]
    pb = pb_all[keep]
    pt = (1 - t) * pa + t * pb

    h, w = A.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    grid = np.stack([xx.ravel(), yy.ravel()], axis=1)

    # 有效域 = Delaunay 三角网覆盖区（用 NaN 探测），边缘羽化；
    # 不要用 convexHull 裁剪——那会留下直边多边形。
    valid = np.zeros((h, w), np.float32)

    out = np.zeros((h, w), np.float32)
    for src_img, p_src, wgt in ((A, pa, 1 - t), (B, pb, t)):
        if wgt <= 0:
            continue
        # 三角网定对应，位移场用平滑 RBF 插值（分片仿射会在三角形边界留折痕）
        disp = (p_src - pt).astype(np.float64)
        rbf = RBFInterpolator(pt.astype(np.float64), disp,
                              kernel="thin_plate_spline", smoothing=2e4)
        d = rbf(grid.astype(np.float64)).astype(np.float32)
        mx = (grid[:, 0] + d[:, 0]).reshape(h, w)
        my = (grid[:, 1] + d[:, 1]).reshape(h, w)
        # 三角网域（NaN = 域外）
        probe = LinearNDInterpolator(pt, p_src[:, 0])(grid).reshape(h, w)
        inside = np.isfinite(probe).astype(np.float32)
        inside = cv2.GaussianBlur(inside, (0, 0), 2.0)
        warp = remap(src_img, mx, my) * inside
        out += wgt * warp
        valid = np.maximum(valid, inside)
    return out  # 不再额外乘凸包掩膜

# ---------------------------------------------------------------- 输出
METHODS = [
    ("BN (Beier-Neely)", morph_bn),
    ("TPS (thin-plate spline)", morph_tps),
    ("FLOW (Farneback)", morph_flow),
    ("SDF (level set)", morph_sdf),
    ("PROFILE (axis+width)", morph_profile),
    ("MESH (triangulated)", morph_mesh),
]


RAMP = [(0.00, (0.16, 0.010, 0.000)), (0.18, (0.55, 0.055, 0.004)),
        (0.36, (0.92, 0.200, 0.010)), (0.55, (1.00, 0.430, 0.035)),
        (0.72, (1.00, 0.660, 0.150)), (0.86, (1.00, 0.870, 0.450)),
        (1.00, (1.00, 1.000, 0.980))]


def to_rgb(mask):
    v = (np.clip(mask, 0, 1) * 255).astype(np.uint8)
    return np.dstack([v, v, v])


def to_fire(mask, y_norm):
    """按 alpha 上色 + 底部偏白热（只为看效果，不参与形状判断）。"""
    out = np.zeros(mask.shape + (3,), np.float32)
    a = np.clip(mask, 0, 1)
    for k in range(len(RAMP) - 1):
        t0, c0 = RAMP[k]
        t1, c1 = RAMP[k + 1]
        m = (a >= t0) & (a <= t1)
        w = (a[m] - t0) / max(t1 - t0, 1e-6)
        for ch in range(3):
            out[..., ch][m] = c0[ch] + (c1[ch] - c0[ch]) * w
    out[a > RAMP[-1][0]] = RAMP[-1][1]
    hot = np.clip(1.2 - 1.1 * y_norm, 0, 1)[..., None] * (a[..., None] ** 1.4)
    out = np.clip(out + hot * 0.55, 0, 1)
    return (out * 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="tools/jpov/assets/fire/mp/mask_1.png")
    ap.add_argument("--b", default="tools/jpov/assets/fire/mp/mask_5.png")
    ap.add_argument("--out", default="/tmp/morph_bench")
    ap.add_argument("--cell", type=int, default=CELL)
    ap.add_argument("--color", action="store_true", help="额外输出彩色版")
    ap.add_argument("--only", default="", help="只跑这些方法（逗号分隔，如 FLOW,PROFILE,SDF,MESH）")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    A = load_alpha(args.a, args.cell)
    B = load_alpha(args.b, args.cell)

    ts = np.linspace(0.0, 1.0, N_FRAMES)
    only = {x.strip().upper() for x in args.only.split(",") if x.strip()}
    methods = [m for m in METHODS if not only or m[0].split()[0].upper() in only]
    rows = []
    for name, fn in methods:
        cells = []
        for t in ts:
            img = A if t == 0.0 else (B if t == 1.0 else fn(A, B, float(t)))
            cells.append(to_rgb(img))
        # 存单条
        strip = np.concatenate(cells, axis=1)
        fn_name = name.split()[0].lower()
        Image.fromarray(strip).save(os.path.join(args.out, f"strip_{fn_name}.png"))
        rows.append((name, strip))
        print("done", name)

    # 汇总图：每行一个方法，左侧留标签区
    S = args.cell
    lab_w = 210
    W = lab_w + S * N_FRAMES
    H = S * len(rows)
    sheet = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(sheet)
    for i, (name, strip) in enumerate(rows):
        sheet.paste(Image.fromarray(strip), (lab_w, i * S))
        d.text((10, i * S + S // 2 - 6), name, fill=(255, 255, 255))
    out = os.path.join(args.out, "morph_bench.png")
    sheet.save(out)
    print("wrote", out, sheet.size)

    if args.color:
        keep = [m[0].split()[0] for m in methods]
        color_rows = []
        yy = np.linspace(0, 1, args.cell)[:, None]
        for name, fn in methods:
            if name.split()[0] not in keep:
                continue
            cells = []
            for t in ts:
                img = A if t == 0.0 else (B if t == 1.0 else fn(A, B, float(t)))
                cells.append(to_fire(img, np.broadcast_to(yy, img.shape)))
            color_rows.append((name, np.concatenate(cells, axis=1)))
        H2 = args.cell * len(color_rows)
        sheet2 = Image.new("RGB", (W, H2), (0, 0, 0))
        d2 = ImageDraw.Draw(sheet2)
        for i, (name, strip) in enumerate(color_rows):
            sheet2.paste(Image.fromarray(strip), (lab_w, i * args.cell))
            d2.text((10, i * args.cell + args.cell // 2 - 6), name, fill=(255, 255, 255))
        out2 = os.path.join(args.out, "morph_bench_color.png")
        sheet2.save(out2)
        print("wrote", out2, sheet2.size)


if __name__ == "__main__":
    main()
