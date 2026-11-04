#!/usr/bin/env python3
"""验收渲染：把 poses.json 的 Mixamo23 姿势叠回原视频帧（2D 相似变换对齐）+ 纯骨架前视图。

用途：算法对比的**唯一裁判**。三种算法跑同一段视频，出一模一样的对比图，
人眼（和 AI 看图）只看「骨节是不是压在关节上、四肢朝向对不对」。

用法：
  render_check.py --track track.json --poses poses.json --video in.mp4 \
                  --frames 0,40,80 --out out_dir/
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mixamo23 as mk
from mocap_fit import SEG, point, to_px

EDGES = [(mk.PAR[j], j) for j in range(1, mk.NB)]

# 骨名 -> 该骨的**末端关节**（画线用；FK 的关节位置就是头的父级关节 + 骨长）
TAIL_BONE = {}


def color_for(j):
    n = mk.NAMES[j]
    if n.startswith("Left"):
        return (255, 60, 60)
    if n.startswith("Right"):
        return (60, 120, 255)
    return (40, 220, 120)


def fit_similarity(src, dst):
    z = src[:, 0] + 1j * src[:, 1]
    w = dst[:, 0] + 1j * dst[:, 1]
    zc = z - z.mean()
    wc = w - w.mean()
    denom = (zc * zc.conj()).sum()
    if abs(denom) < 1e-9:
        return 1 + 0j, w.mean()
    a = (wc * zc.conj()).sum() / denom
    return a, w.mean() - a * z.mean()


PAIRS = [(7, 11), (11, 12), (8, 13), (12, 14), (9, 15), (13, 16),
         (15, 23), (19, 24), (16, 25), (20, 26), (17, 27), (21, 28),
         (18, 31), (22, 32)]


def draw(draw, pts, w=5):
    for a, b in EDGES:
        draw.line([tuple(pts[a]), tuple(pts[b])], fill=color_for(b), width=w)
    for j in range(mk.NB):
        r = max(2, w // 2)
        draw.ellipse([pts[j][0] - r, pts[j][1] - r, pts[j][0] + r, pts[j][1] + r],
                     fill=(255, 255, 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track", required=True)
    ap.add_argument("--poses", required=True)
    ap.add_argument("--video", required=True)
    ap.add_argument("--frames", default="0")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    track = json.load(open(args.track))
    poses_doc = json.load(open(args.poses))
    poses = [[np.array(q) for q in f["quat"]] for f in poses_doc["frames"]]
    scale = poses_doc["scale"]
    fps = poses_doc["fps"]
    os.makedirs(args.out, exist_ok=True)

    for fi in [int(x) for x in args.frames.split(",")]:
        pos, _ = mk.fk(poses[fi], scale)
        lm = to_px(track["frames"][fi])
        src = np.array([pos[b][:2] for b, _ in PAIRS])
        dst = np.array([point(lm, l)[:2] for _, l in PAIRS])
        a, bq = fit_similarity(src, dst)
        z = pos[:, 0] + 1j * pos[:, 1]
        w = a * z + bq
        proj = np.stack([w.real, -w.imag], 1)   # 回到图像坐标（y 向下）

        tmp = f"/tmp/_rc_{fi}.png"
        subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{fi / fps:.4f}", "-i", args.video,
                        "-frames:v", "1", tmp, "-y"], check=True)
        img = Image.open(tmp).convert("RGB")
        W, H = img.size
        lw = max(2, W // 220)

        ov = img.copy()
        draw(ImageDraw.Draw(ov), proj, lw)

        sk = Image.new("RGB", (W, H), (18, 18, 22))
        draw(ImageDraw.Draw(sk), proj, lw)

        sheet = Image.new("RGB", (W * 2, H))
        sheet.paste(img, (0, 0))
        sheet.paste(sk, (W, 0))
        out = os.path.join(args.out, f"cmp_{fi:04d}.jpg")
        sheet.resize((W, H // 2)).save(out, quality=90)
        print("wrote", out)


if __name__ == "__main__":
    main()
