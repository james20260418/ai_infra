#!/usr/bin/env python3
"""jpvol.py — .jpovvol 体积格式的**单一来源**（读/写 + 公共数组工具）。

本模块把「.jpovvol 格式定义」和几个被多个转换脚本复用的纯函数集中到一处，
避免同一逻辑散在多份脚本里各写一遍（历史上正是这种复制粘贴，让一个
`copyToArray` 轴序 bug 同时存在于两个转换器里）。

格式（little-endian）：
  JPVOL1（ver=1，单帧，f32）：
    char magic[8]="JPVOL1\\0\\0"; uint32 ver, nx, ny, nz, channels, dtype(=0)
    数据：nx*ny*nz*channels 个 float32
  JPVOL2（ver=2，帧序列）：
    char magic[8]="JPVOL2\\0\\0"; uint32 ver, nx, ny, nz, nframes, channels, dtype, reserved
    数据：nframes*nx*ny*nz*channels
  dtype：0=f32、1=u8、2=f16(half)。
  体素序（两种都一样）：**每帧内 x 最快 → y → z**，通道交错；JPVOL2 帧间顺序排列（帧最慢）。
  ⇒ 与「每帧一层 2D 数组（宽=nx、高=ny*nz）」的 GPU 上传布局一致。
  通道语义：R=density(火), G=temperature/flames，均归一化 [0,1]；
  **可选第 3 通道 B = 烟密度**（3 通道文件用于「火/温度/烟」三场分离，见 vdb_to_jpovvol.py）。
  引擎读取器：tools/jpov/src/volume_manager.cc（GL）+ src/fire_fog/fire_volume.h（GL-free）。

numpy 数组约定：本模块一律用 **[F][z][y][x]**（x 最快）表示体积。
⚠️ pyopenvdb 的 `copyToArray` 用的是 **array[x][y][z]**（首轴 = x）；两者不同，转换时须 transpose。
"""
import struct

import numpy as np

MAGIC1 = b"JPVOL1\x00\x00"
MAGIC2 = b"JPVOL2\x00\x00"


def write_jpovvol(path, dens, temp, smoke=None, dtype="u8", pre_quantized=False):
    """写 JPVOL2 帧序列。dens/temp(/smoke): [F][z][y][x]，∈[0,1]。

    传 smoke 则写 **3 通道**（R=火密度, G=温度, B=烟密度）；否则写 2 通道。
    dtype="u8" → uint8（code 1）；"f16" → float16（code 2）；"f32" → float32（code 0）。
    pre_quantized=True 时输入已是目标存储 dtype → 不再量化（省一次全量副本）。
    返回 (nx, ny, nz, nframes, 总字节数)。
    """
    F, Z, Y, X = dens.shape
    chans = [dens, temp] if smoke is None else [dens, temp, smoke]
    for c in chans:
        if c.shape != dens.shape:
            raise ValueError("dens/temp/smoke 形状必须一致")
    if dtype == "u8":
        if pre_quantized:
            np_dt, dtype_code, scale = np.uint8, 1, None
        else:
            np_dt, dtype_code, scale = np.uint8, 1, 255.0
    elif dtype == "f16":
        np_dt, dtype_code, scale = np.float16, 2, None
    elif dtype == "f32":
        if pre_quantized:
            raise ValueError("pre_quantized 只能用于 dtype='u8'/'f16'")
        np_dt, dtype_code, scale = np.float32, 0, None
    else:
        raise ValueError("dtype 必须是 'u8'/'f16'/'f32'，收到 %r" % (dtype,))

    # half：2 字节/通道，动态范围 ±65504、精度远优于 u8。
    def frame_payload(i):
        if scale is None:   # 输入已是目标 dtype（pre_quantized）或 f32
            fr = np.stack([np.asarray(c[i]) for c in chans], axis=-1)
            return fr.astype(np_dt, copy=False).tobytes()
        fr = np.clip(np.stack([np.asarray(c[i]) for c in chans], axis=-1), 0.0, 1.0)
        return (fr * scale + 0.5).astype(np.uint8).tobytes()

    # **逐帧写**（不拼全量 [F,z,y,x,C] 大数组）——ds=1 f16 下全量可达 GB 级，容器会 OOM。
    with open(path, "wb") as f:
        f.write(MAGIC2)
        f.write(struct.pack("<6I", 2, X, Y, Z, F, len(chans)))
        f.write(struct.pack("<2I", dtype_code, 0))
        total = 40
        for i in range(F):
            b = frame_payload(i)
            f.write(b)
            total += len(b)
    return X, Y, Z, F, total


def write_jpovvol1(path, dens, temp):
    """写 JPVOL1 单帧 f32。dens/temp: [z][y][x]，∈[0,1]。返回 (nx,ny,nz,字节数)。"""
    Z, Y, X = dens.shape
    assert temp.shape == dens.shape, "dens/temp 形状必须一致"
    data = np.ascontiguousarray(np.stack([dens, temp], axis=-1).astype(np.float32))
    with open(path, "wb") as f:
        f.write(MAGIC1)
        f.write(struct.pack("<6I", 1, X, Y, Z, 2, 0))
        f.write(data.tobytes())
    return X, Y, Z, 32 + data.nbytes


def read_jpovvol(path):
    """读 .jpovvol → (frames, nz, ny, nx, channels) 的 float32 数组（∈[0,1]）。"""
    with open(path, "rb") as fh:
        blob = fh.read()
    magic = blob[:8]
    if magic[:6] == b"JPVOL2":
        _ver, nx, ny, nz, nframes, channels, dtype, _res = struct.unpack("<8I", blob[8:40])
        data = blob[40:]
    elif magic[:6] == b"JPVOL1":
        _ver, nx, ny, nz, channels, dtype = struct.unpack("<6I", blob[8:32])
        nframes = 1
        data = blob[32:]
    else:
        raise ValueError("not a JPVOL file: magic=%r" % (magic,))
    np_dtype = np.uint8 if dtype == 1 else (np.float16 if dtype == 2 else np.float32)
    arr = np.frombuffer(data, dtype=np_dtype).reshape(nframes, nz, ny, nx, channels)
    if dtype == 1:
        arr = arr.astype(np.float32) / 255.0
    elif dtype == 2:
        arr = arr.astype(np.float32)   # half → float32
    return arr


def downsample(a, s):
    """对末尾三轴 (z,y,x) 做 s×s×s 盒平均。a: [..., z, y, x]。s<=1 原样返回。"""
    if s <= 1:
        return a
    *lead, nz, ny, nx = a.shape
    a = a[..., :nz // s * s, :ny // s * s, :nx // s * s]
    a = a.reshape(*lead, nz // s, s, ny // s, s, nx // s, s)
    red = tuple(range(len(lead) + 1, a.ndim, 2))   # 's' 轴的位置
    return a.mean(axis=red)


def apply_window(dens, temp, f):
    """边界淡出：把内容在**外侧 f 比例**内平滑降到 0，避免「火贴到体素盒壁被切」的硬盒边
    （x/z 两端 + y 顶端；**底面不淡**，火坐地）。f<=0 时不改。需 dens/temp 为 [F][z][y][x]。"""
    if f <= 0.0:
        return dens, temp

    def smooth(k):
        t = np.linspace(0.0, 1.0, k, dtype=np.float32)
        return t * t * (3.0 - 2.0 * t)   # smoothstep: 0→1

    def ramp_both(n):
        r = np.ones(n, dtype=np.float32)
        k = max(1, int(round(n * f)))
        s = smooth(k)
        r[:k] = s
        r[n - k:] = s[::-1]
        return r

    F, Z, Y, X = dens.shape
    wx = ramp_both(X)[None, None, None, :]
    wz = ramp_both(Z)[None, :, None, None]
    wy = np.ones(Y, dtype=np.float32)          # 仅顶端淡出
    ky = max(1, int(round(Y * f)))
    wy[Y - ky:] = 1.0 - smooth(ky)
    wy = wy[None, None, :, None]
    w = wx * wz * wy
    return dens * w, temp * w


def loop_half_xfade(a, offset_frac=0.5):
    """offset + cross-dissolve 环闭合（业界 "Make Loop" 思路）：a=[F,...] → 无缝循环 [F,...]。

    L[t] = (1-c[t])·a[t] + c[t]·a[(t+off)%F]，c[t] = 0.5 + 0.5·cos(2πt/F)
    （c[0]=c[F]=1 ⇒ 接缝处只显示"内部"帧 a[off]）。代价：轻微重影（对混沌火可接受）。
    """
    F = a.shape[0]
    off = int(round(F * offset_frac)) % F
    t = np.arange(F, dtype=np.float32)
    c = 0.5 + 0.5 * np.cos(2.0 * np.pi * t / F)
    c = c.reshape((F,) + (1,) * (a.ndim - 1))
    shifted = np.roll(a, -off, axis=0)
    return (1.0 - c) * a + c * shifted
