#!/usr/bin/env python3
"""MediaPipe Pose Landmarker -> pose_track.json (JPOV mocap 的「眼睛」).

两种输入（"连环画"模式是给 AI 图生图工作流用的：10 张 still @5fps = 2s）:
  pose_extract.py --video in.webm --out track.json [--fps 30] [--num-poses 2]
  pose_extract.py --images IMG_DIR --fps 5 --out track.json [--pattern '*.png']

关键区别：--images 走 MediaPipe 的 IMAGE 模式（逐张独立检测，无跨帧 tracker），
因为 AI 生成的连环画帧与帧之间本来就不连续，VIDEO 模式的 tracker 反而会把手牌粘错人。

输出契约见 docs/jpov_video_mocap_design.md §2：
  { "version":1, "fps":..., "width":..., "height":...,
    "frames":[ {"t":0.0333,
                "landmarks":[{"x":u,"y":v,"z":depth,"visibility":p},...33],   # 像素空间 (x*W, y*H)
                "world":[{"x":..,"y":..,"z":..},...33],                       # 米, 髋原点, y 向下
                "presence":p} ] }

MediaPipe 的 33 点 world landmarks 已经提供 y 向下的近似米制量；本脚本原样落盘，
任何「翻 y / 缩放到身高」都在 C++ 侧（mocap_retarget）做 —— 提取器保持哑。
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np


def die(msg):
    print(f"[pose_extract] FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def close_quietly(landmarker):
    """显式释放 landmarker，避开解释器退出阶段的已知噪声。

    背景（mediapipe 1.2.0 + Python 3.12）：不显式 close 时，`PoseLandmarker.__del__`
    在解释器 shutdown 之后才跑，此时 serial dispatcher 的回调已被拆掉，会往 stderr 打
    `TypeError: 'NoneType' object is not callable` 的 traceback —— 退出码仍是 0、
    结果文件也正常，纯属吓人的噪声。close() 是幂等的（内部 `if not self._handle: return`），
    进程还活着时先关一次，__del__ 里的第二次就变成空操作。

    只吞这一种异常：其它异常照常抛，避免把真正的释放失败一起藏掉。
    """
    try:
        landmarker.close()
    except TypeError as exc:
        if "NoneType" not in str(exc):
            raise


def probe(video):
    out = subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate",
        "-of", "json", video])
    s = json.loads(out)["streams"][0]
    num, den = (s.get("avg_frame_rate") or "0/1").split("/")
    fps = float(num) / float(den) if float(den) else 0.0
    return int(s["width"]), int(s["height"]), (fps if fps > 0 else 30.0)


def read_frames(video, fps, max_frames):
    """ffmpeg 解码成 rgb24 原始流，按 fps 重采样。返回 (w,h,frame_bytes)."""
    w, h, _ = probe(video)
    # 统一成偶数尺寸，避免 yuv 转换报错
    w -= w % 2
    h -= h % 2
    cmd = ["ffmpeg", "-v", "error", "-i", video,
           "-vf", f"fps={fps},scale={w}:{h}", "-pix_fmt", "rgb24",
           "-f", "rawvideo", "-"]
    if max_frames:
        cmd = cmd[:-1] + ["-frames:v", str(max_frames), "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h * 3
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        yield w, h, np.frombuffer(buf, np.uint8).reshape(h, w, 3)
    p.stdout.close()
    p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", default="")
    ap.add_argument("--images", default="", help="图片目录（连环画模式，走 IMAGE running mode）")
    ap.add_argument("--pattern", default="*.png")
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default=os.environ.get("POSE_MODEL", "/tmp/pose_full.task"))
    ap.add_argument("--fps", type=float, default=0.0, help="0 = 保持源帧率")
    ap.add_argument("--num-poses", type=int, default=2)
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--overlay-every", type=int, default=0,
                    help="每隔 N 帧落一张 2D 关键点叠加图，供 image 工具验收")
    ap.add_argument("--overlay-dir", default="")
    args = ap.parse_args()
    if not args.video and not args.images:
        die("必须给 --video 或 --images 之一")

    import mediapipe as mp
    from mediapipe.tasks.python import vision, BaseOptions

    if args.images:
        return run_images(args, mp, vision, BaseOptions)

    src_w, src_h, src_fps = probe(args.video)
    fps = args.fps if args.fps > 0 else src_fps
    print(f"[pose_extract] {args.video} {src_w}x{src_h} src_fps={src_fps:.3f} -> {fps}")

    opts = vision.PoseLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=args.model),
        running_mode=vision.RunningMode.VIDEO,
        num_poses=max(1, args.num_poses),
        min_pose_detection_confidence=0.35,
        min_pose_presence_confidence=0.35,
        min_tracking_confidence=0.35,
        output_segmentation_masks=False)
    landmarker = vision.PoseLandmarker.create_from_options(opts)

    import cv2  # 只用于叠加图, 缺失时降级

    frames = []
    prev_center = None
    idx = 0
    for w, h, rgb in read_frames(args.video, fps, args.max_frames):
        mp_img = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        t_ms = int(round(idx * 1000.0 / fps))
        res = landmarker.detect_for_video(mp_img, t_ms)

        rec = {"t": idx / fps, "landmarks": [], "world": [], "presence": 0.0}
        if res.pose_landmarks:
            # 多人时按「可见度加权人数」+「与上一帧中心距离」选一个。
            best, best_score = None, -1e9
            for pi, lms in enumerate(res.pose_landmarks):
                vis = [l.visibility for l in lms]
                score = sum(vis) / len(vis)
                cx = sum(l.x for l in lms) / len(lms) * w
                cy = sum(l.y for l in lms) / len(lms) * h
                if prev_center is not None:
                    d = math.hypot(cx - prev_center[0], cy - prev_center[1]) / max(w, h)
                    score -= 2.0 * d          # 时序一致性（简单轨迹粘连）
                if score > best_score:
                    best, best_score = (pi, cx, cy), score
            pi, cx, cy = best
            prev_center = (cx, cy)
            lms = res.pose_landmarks[pi]
            world = res.pose_world_landmarks[pi] if res.pose_world_landmarks else None
            rec["landmarks"] = [{"x": l.x * w, "y": l.y * h, "z": l.z, "visibility": l.visibility}
                                for l in lms]
            if world:
                rec["world"] = [{"x": l.x, "y": l.y, "z": l.z} for l in world]
            rec["presence"] = sum(l.visibility for l in lms) / len(lms)

            if args.overlay_every and args.overlay_dir and idx % args.overlay_every == 0:
                if cv2 is not None:
                    vis_img = rgb[:, :, ::-1].copy()
                    for l in lms:
                        cv2.circle(vis_img, (int(l.x * w), int(l.y * h)), 4, (0, 255, 0), -1)
                    os.makedirs(args.overlay_dir, exist_ok=True)
                    cv2.imwrite(os.path.join(args.overlay_dir, f"ov_{idx:05d}.png"), vis_img)
        frames.append(rec)
        idx += 1
        if idx % 25 == 0:
            print(f"  [{idx}] presence={rec['presence']:.2f}", flush=True)

    meta = {"version": 1, "fps": fps, "width": w, "height": h,
            "frame_count": len(frames), "source": os.path.basename(args.video),
            "extractor": "mediapipe.PoseLandmarker/full",
            "num_poses": args.num_poses}
    with open(args.out, "w") as f:
        json.dump({**meta, "frames": frames}, f, separators=(",", ":"))
    det = sum(1 for fr in frames if fr["landmarks"])
    print(f"[pose_extract] wrote {args.out}: {len(frames)} frames, "
          f"{det} with pose ({100.0*det/max(1,len(frames)):.1f}%)")
    close_quietly(landmarker)


def _pick_and_serialize(res, w, h, prev_center):
    """从一次检测结果里选一个（最可信）人，转成落盘结构。返回 (rec_fields, center)."""
    if not res.pose_landmarks:
        return {"landmarks": [], "world": [], "presence": 0.0}, prev_center
    best, best_score = None, -1e9
    for pi, lms in enumerate(res.pose_landmarks):
        score = sum(l.visibility for l in lms) / len(lms)
        cx = sum(l.x for l in lms) / len(lms) * w
        cy = sum(l.y for l in lms) / len(lms) * h
        if prev_center is not None:
            d = math.hypot(cx - prev_center[0], cy - prev_center[1]) / max(w, h)
            score -= 2.0 * d
        if score > best_score:
            best, best_score = (pi, cx, cy), score
    if best is None:
        return {"landmarks": [], "world": [], "presence": 0.0}, prev_center
    pi, cx, cy = best
    lms = res.pose_landmarks[pi]
    world = res.pose_world_landmarks[pi] if res.pose_world_landmarks else None
    rec = {
        "landmarks": [{"x": l.x * w, "y": l.y * h, "z": l.z, "visibility": l.visibility}
                      for l in lms],
        "world": ([{"x": l.x, "y": l.y, "z": l.z} for l in world] if world else []),
        "presence": sum(l.visibility for l in lms) / len(lms),
    }
    return rec, (cx, cy)


def run_images(args, mp, vision, BaseOptions):
    """连环画模式：目录里按文件名排序的一串 still，每张独立检测。"""
    import glob
    files = sorted(glob.glob(os.path.join(args.images, args.pattern)))
    if not files:
        die(f"{args.images} 下没有匹配 {args.pattern} 的图片")
    fps = args.fps if args.fps > 0 else 5.0
    print(f"[pose_extract] images: {len(files)} 张 @ {fps} fps")

    opts = vision.PoseLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=args.model),
        running_mode=vision.RunningMode.IMAGE,
        num_poses=max(1, args.num_poses),
        min_pose_detection_confidence=0.35,
        min_pose_presence_confidence=0.35,
        output_segmentation_masks=False)
    landmarker = vision.PoseLandmarker.create_from_options(opts)

    import cv2
    frames, prev_center = [], None
    for idx, fp in enumerate(files):
        bgr = cv2.imread(fp, cv2.IMREAD_COLOR)
        if bgr is None:
            die(f"读不了图片 {fp}")
        h, w = bgr.shape[:2]
        rgb = bgr[:, :, ::-1].copy()
        res = landmarker.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb))
        rec, prev_center = _pick_and_serialize(res, w, h, prev_center)
        rec["t"] = idx / fps
        rec["image"] = os.path.basename(fp)
        frames.append(rec)
        if args.overlay_every and args.overlay_dir and rec["landmarks"] and idx % args.overlay_every == 0:
            if cv2 is not None:
                vis_img = bgr.copy()
                for l in rec["landmarks"]:
                    cv2.circle(vis_img, (int(l["x"]), int(l["y"])), 5, (0, 255, 0), -1)
                os.makedirs(args.overlay_dir, exist_ok=True)
                cv2.imwrite(os.path.join(args.overlay_dir, f"ov_{idx:05d}.png"), vis_img)
        print(f"  [{idx}] {os.path.basename(fp)} presence={rec['presence']:.2f}")

    # 连环画各帧尺寸可能不同 —— 落盘记录首帧尺寸，2D 坐标是各帧自身的像素空间。
    meta = {"version": 1, "fps": fps, "width": w, "height": h,
            "frame_count": len(frames), "source": os.path.basename(args.images.rstrip('/')),
            "extractor": "mediapipe.PoseLandmarker/full", "mode": "images",
            "num_poses": args.num_poses}
    with open(args.out, "w") as f:
        json.dump({**meta, "frames": frames}, f, separators=(",", ":"))
    det = sum(1 for fr in frames if fr["landmarks"])
    print(f"[pose_extract] wrote {args.out}: {len(frames)} 帧, {det} 帧检出")
    close_quietly(landmarker)


if __name__ == "__main__":
    main()
