#!/usr/bin/env python3
"""篝火 A/B 指标：把两张「孤立火」渲染（参考 vs 我们）做**同口径**量化 + 并排图。

用途：P0 基准台的度量工具。双方各自是一张「黑底孤立火」图（参考 = EmberGen
官方渲染帧；我们 = JPOV `--ab-capture` 输出），先按同一亮度阈值取前景，再算：

  1. 形状：前景包围盒高宽比 h/w、前景面积占比 %
  2. 颜色：前景均值 RGB、近白(>230)占比
  3. 竖向亮度剖面：前景归一化后分 N 带，逐带前景平均亮度（band 0 = 底部）

用法::

    python3 ab_metrics.py --gt <ref.png> --ours <ours.png> \\
        [--out <montage.png>] [--report <report.txt>] [--thresh 12]

退出码：0 = 正常；2 = 双方前景至少一侧为空。
"""

import argparse
import dataclasses
import sys
from typing import List, Tuple

import numpy as np
from PIL import Image, ImageDraw

# 前景判定的亮度阈值（0..255）；双方必须同值。
DEFAULT_THRESH = 12.0
# 竖向剖面的分带数。
PROFILE_BANDS = 10
# 近白像素的单通道阈值。
NEAR_WHITE = 230.0
# 裁前景时四周留白（像素），让火烧的形态在并排图里可读。
CROP_MARGIN = 40
# 并排图统一缩放后的高度（像素）。
MONTAGE_HEIGHT = 560


@dataclasses.dataclass
class Metrics:
    """单张「孤立火」图的指标。foreground 为空时 empty=True，其余字段无意义。"""

    empty: bool
    size: Tuple[int, int] = (0, 0)                 # (W, H)，像素
    bbox: Tuple[int, int, int, int] = (0, 0, 0, 0)  # (x0, y0, x1, y1)，含端点
    aspect: float = 0.0                            # 前景包围盒高/宽
    area_pct: float = 0.0                          # 前景像素占全图 %
    mean_rgb: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    near_white_pct: float = 0.0                    # 前景里三通道均 >230 的占比 %
    profile: List[float] = dataclasses.field(default_factory=list)  # 底→顶，长度 PROFILE_BANDS


def load_rgb(path: str) -> np.ndarray:
    """读图为 float32 RGB 数组（H, W, 3），值域 [0,255]。"""
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)


def foreground_mask(rgb: np.ndarray, thresh: float) -> np.ndarray:
    """亮度 > thresh 的像素视为前景（火/烟）。"""
    return rgb.mean(axis=2) > thresh


def crop_to_mask(
    rgb: np.ndarray, mask: np.ndarray, margin: int
) -> Tuple[np.ndarray, np.ndarray]:
    """把图裁到前景包围盒 + margin；前景为空时原样返回。"""
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return rgb, mask
    y0 = max(0, ys.min() - margin)
    y1 = min(rgb.shape[0], ys.max() + 1 + margin)
    x0 = max(0, xs.min() - margin)
    x1 = min(rgb.shape[1], xs.max() + 1 + margin)
    return rgb[y0:y1, x0:x1], mask[y0:y1, x0:x1]


def vertical_profile(rgb: np.ndarray, mask: np.ndarray, bands: int) -> List[float]:
    """前景归一化后的竖向平均亮度剖面；band 0 = 底部（火根）。

    仅在**前景包围盒**内分带（忽略盒外的黑），每带取该带前景像素的平均亮度；
    空带记 0。返回长度 = bands 的列表，值域 [0,255]。
    """
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return [0.0] * bands
    top, bottom = ys.min(), ys.max()
    span = max(1, bottom - top + 1)
    lum = rgb.mean(axis=2)
    out: List[float] = []
    for b in range(bands):
        # band 0 在底部 → 从 bottom 往上数。
        lo = max(top, bottom - int((b + 1) * span / bands) + 1)
        hi = min(bottom + 1, bottom - int(b * span / bands))
        sub_mask = mask[lo:hi, :]
        if sub_mask.sum() == 0:
            out.append(0.0)
        else:
            out.append(float(lum[lo:hi, :][sub_mask].mean()))
    return out


def measure(rgb: np.ndarray, thresh: float) -> Metrics:
    """对单张图算全部指标。图应为「黑底孤立火」。"""
    mask = foreground_mask(rgb, thresh)
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return Metrics(empty=True, size=(int(rgb.shape[1]), int(rgb.shape[0])))
    w = xs.max() - xs.min() + 1
    h = ys.max() - ys.min() + 1
    fg = rgb[mask]
    near_white = (
        (fg[:, 0] > NEAR_WHITE) & (fg[:, 1] > NEAR_WHITE) & (fg[:, 2] > NEAR_WHITE)
    ).mean() * 100.0
    return Metrics(
        empty=False,
        size=(int(rgb.shape[1]), int(rgb.shape[0])),
        bbox=(int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())),
        aspect=h / float(w),
        area_pct=float(mask.mean() * 100.0),
        mean_rgb=(float(fg[:, 0].mean()), float(fg[:, 1].mean()), float(fg[:, 2].mean())),
        near_white_pct=float(near_white),
        profile=vertical_profile(rgb, mask, PROFILE_BANDS),
    )


def format_metrics(name: str, m: Metrics) -> str:
    """把指标格式化成多行文本。"""
    if m.empty:
        return f"[{name}] 前景为空（阈值过滤后无可见像素）"
    r, g, b = m.mean_rgb
    prof = " ".join(f"{v:5.1f}" for v in m.profile)
    return (
        f"[{name}] size={m.size} bbox={m.bbox}\n"
        f"    h/w={m.aspect:.2f}  area={m.area_pct:.2f}%\n"
        f"    meanRGB=({r:.1f},{g:.1f},{b:.1f})  nearWhite={m.near_white_pct:.1f}%\n"
        f"    profile(bottom->top)={prof}"
    )


def make_montage(
    gt_rgb: np.ndarray,
    gt_mask: np.ndarray,
    ours_rgb: np.ndarray,
    ours_mask: np.ndarray,
    out_path: str,
) -> None:
    """把两侧前景裁图等比缩到同高，并排 + 标题，写出 PNG。"""
    gt_c, _ = crop_to_mask(gt_rgb, gt_mask, CROP_MARGIN)
    ours_c, _ = crop_to_mask(ours_rgb, ours_mask, CROP_MARGIN)

    def to_image(arr: np.ndarray) -> Image.Image:
        im = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
        scale = MONTAGE_HEIGHT / im.height
        return im.resize((max(1, int(im.width * scale)), MONTAGE_HEIGHT))

    gt_im = to_image(gt_c)
    ours_im = to_image(ours_c)
    gap = 40
    width = gt_im.width + ours_im.width + gap
    canvas = Image.new("RGB", (width, MONTAGE_HEIGHT + 40), (18, 18, 22))
    canvas.paste(gt_im, (0, 40))
    canvas.paste(ours_im, (gt_im.width + gap, 40))
    draw = ImageDraw.Draw(canvas)
    draw.text((8, 14), "GROUND TRUTH  EmberGen official render", fill=(255, 210, 150))
    draw.text((gt_im.width + gap + 8, 14), "OURS  JPOV", fill=(150, 210, 255))
    canvas.save(out_path)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Campfire A/B metrics + montage")
    ap.add_argument("--gt", required=True, help="参考图（EmberGen 官方渲染帧）")
    ap.add_argument("--ours", required=True, help="我们的图（JPOV ab-capture 输出）")
    ap.add_argument("--out", default="", help="并排图输出路径（省略则不写）")
    ap.add_argument("--report", default="", help="报告文本输出路径（省略则只打印）")
    ap.add_argument("--thresh", type=float, default=DEFAULT_THRESH, help="前景亮度阈值")
    args = ap.parse_args(argv)

    gt_rgb = load_rgb(args.gt)
    ours_rgb = load_rgb(args.ours)
    gt_mask = foreground_mask(gt_rgb, args.thresh)
    ours_mask = foreground_mask(ours_rgb, args.thresh)

    m_gt = measure(gt_rgb, args.thresh)
    m_ours = measure(ours_rgb, args.thresh)

    text = "\n".join(
        [
            "== Campfire A/B ==",
            f"thresh={args.thresh}  gt={args.gt}  ours={args.ours}",
            format_metrics("GT", m_gt),
            format_metrics("OURS", m_ours),
        ]
    )
    print(text)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if args.out:
        make_montage(gt_rgb, gt_mask, ours_rgb, ours_mask, args.out)
        print(f"montage: {args.out}")

    return 2 if (m_gt.empty or m_ours.empty) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
