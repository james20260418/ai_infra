#!/usr/bin/env python3
"""生成「整键 flipbook」按钮底板（steel），4x4 / 16 帧 / 每格 380x96。

用途：`sp_ui` 的 `SpButtonSkin::Style::kFlipbook` —— 按钮**整块**就是这张图集
（不画主题板材/边框），hover / pressed 只在这张图上乘 tint。

风格：拉丝碳灰金属 + 一条周期性扫过的高光带 + 边缘呼吸的冷蓝余晖。
**不用水纹/涟漪/位移场** —— 这里是机械感，不是流体。

格子长宽比 ≈ 3.95:1（380x96），对应 demo 里 173x44 的按钮 —— 图集格子与按钮
长宽比不一致时，倒角会被非等比拉伸。

无缝约定（见 `interface/flipbook.h`）：
  * 行主序、帧 0 在图片左上角；
  * 帧 N 的图案 = 帧 0 的**前一拍**（相位走满整圈），所以 `t = N/fps` 与 `t = 0`
    逐像素相同 —— 播放时不会顿一下；
  * 高光带的位置用 `cos(2πu - phase)` 表达，u 沿对角线、在边长上周期 ⇒ 空间与
    时间同时闭合（不会出现「带子从右边跳回左边」的断层）。

复现：
    cd tools/jpov/assets/sp_ui/button
    python3 make_button_flipbook.py --out plate_button_steel_4x4.png
"""

import argparse
import math

import numpy as np
from PIL import Image

# ---- 调色板（直接当 sRGB 值用）----
STEEL_TOP = np.array([0.46, 0.49, 0.54])     # 上缘亮
STEEL_BOT = np.array([0.15, 0.16, 0.19])     # 下缘暗
SHEEN = np.array([0.88, 0.93, 1.00])         # 扫光（冷白，不做暖橙——冷底暖光会显脏）
RIM = np.array([0.42, 0.62, 0.95])           # 边缘呼吸（冷蓝）


def hash01(n: np.ndarray) -> np.ndarray:
    """确定性哈希噪声（不随帧变化 ⇒ 拉丝是「静态的表面」，不是流动的纹理）。"""
    return np.modf(np.sin(n * 12.9898) * 43758.5453)[0] * 0.5 + 0.5


def brushed_noise(xs: np.ndarray, ys: np.ndarray) -> np.ndarray:
    """横向拉丝：多种尺度的哈希叠加，只在 x 方向变化（y 只做轻微扰动）。

    频率要够**粗**：图集格是 256x128，而实际按钮只有 104x52（缩 2.4 倍），
    太细的丝纹在缩小后会被双线性抹成一片平滑渐变（实测 0.077/0.19 那两档
    在按钮上完全看不见）。所以主能量放在 20~60 纹素周期这一段。

    纯哈希、无随机种子 ⇒ 每帧完全一致（表面本身不动）。
    """
    s = np.zeros_like(xs)
    for k, (fx, fy, amp) in enumerate([(0.008, 1.7, 0.60), (0.021, 3.1, 0.42),
                                      (0.052, 5.3, 0.26), (0.130, 9.7, 0.12)]):
        s += amp * (hash01(xs * fx + ys * fy + k * 17.3) - 0.5)
    s += 0.25 * (hash01(xs * 1.1 + ys * 0.7) - 0.5)   # 细频颗粒
    return s


def make_cell(w: int, h: int, phase: float, band_center: float) -> np.ndarray:
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    tx = xs / (w - 1.0)     # 0..1
    ty = ys / (h - 1.0)

    # ---- 底板：垂直渐变 + 横向拉丝 ----
    grad = (1.0 - ty)[..., None]
    base = STEEL_BOT + (STEEL_TOP - STEEL_BOT) * grad ** 1.4
    base = base * (1.0 + 0.34 * brushed_noise(xs, ys))[..., None]

    # ---- 边框倒角：外 2px 压暗、内 2px 提亮（纯几何，不随时间变）----
    d_edge = np.minimum(np.minimum(xs, w - 1.0 - xs), np.minimum(ys, h - 1.0 - ys))
    inner_shadow = np.clip(1.0 - d_edge / 3.0, 0.0, 1.0)[..., None]
    base = base * (1.0 - 0.50 * inner_shadow)
    bevel = np.exp(-((d_edge - 2.4) ** 2) / (2.0 * 1.1 ** 2))[..., None]
    base = base + bevel * np.array([0.26, 0.28, 0.31])

    # ---- 扫光：一条高斯带沿对角线跑完整圈 ----
    # 带中心从 −0.20 走到 1.20（两端都在画面外）⇒ 循环时「光带在画面外」
    # 换位，看不到跳变；而 cos(phase) 那类写法会在端点堆成角落光团。
    u = tx * 0.72 + ty * 0.28
    band = np.exp(-((u - band_center) ** 2) / (2.0 * 0.13 ** 2))
    base = base + band[..., None] * SHEEN * 0.42

    # ---- 边缘呼吸：整圈冷光随相位明暗（同一圈，时间上无缝）----
    breath = 0.5 + 0.5 * math.cos(phase)
    rim = np.exp(-(d_edge / 6.0) ** 2)[..., None]
    base = base + rim * RIM * (0.05 + 0.16 * breath)

    # ---- 中心微亮 + 轻微暗角，让按钮「鼓」起来 ----
    cx = (tx - 0.5) * 1.9
    cy = (ty - 0.5) * 1.5
    r2 = cx * cx + cy * cy
    base = base * (1.0 - 0.14 * np.clip(r2, 0.0, 1.0))[..., None]
    base = base + (np.exp(-r2 * 2.4) * 0.06)[..., None]

    # ---- 软限幅（filmic）：高光滚降，避免边界/扫光中心爆白 ----
    base = 1.0 - np.exp(-np.clip(base, 0.0, None) * 1.45)
    return np.clip(base, 0.0, 1.0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="plate_button_steel_4x4.png")
    ap.add_argument("--tiles", type=int, default=4, help="每边格数（4 = 4x4 = 16 帧）")
    ap.add_argument("--cell-w", type=int, default=380,
                    help="单格宽（默认 380；与 96 高 ≈ 3.95:1，对应 demo 的 173x44 按钮）")
    ap.add_argument("--cell-h", type=int, default=96)
    args = ap.parse_args()

    n = args.tiles * args.tiles
    atlas = np.zeros((args.cell_h * args.tiles, args.cell_w * args.tiles, 3),
                     dtype=np.float64)
    for f in range(n):
        # 相位走满整圈：第 N 帧 = 第 0 帧的前一拍（不是重复帧）。
        phase = 2.0 * math.pi * f / n
        # 扫光带中心：−0.20 → 1.20（两端都在画面外 ⇒ 循环处看不见跳变）。
        band_center = -0.20 + 1.40 * (f / n)
        cell = make_cell(args.cell_w, args.cell_h, phase, band_center)
        row, col = divmod(f, args.tiles)     # 行主序、帧 0 在左上
        atlas[row * args.cell_h:(row + 1) * args.cell_h,
              col * args.cell_w:(col + 1) * args.cell_w] = cell

    img = Image.fromarray((atlas * 255.0 + 0.5).astype(np.uint8), "RGB")
    img.save(args.out)
    print(f"{args.out}: {img.width}x{img.height}, {args.tiles}x{args.tiles} = {n} 帧, "
          f"cell {args.cell_w}x{args.cell_h}")


if __name__ == "__main__":
    main()
