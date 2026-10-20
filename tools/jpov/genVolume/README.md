# genVolume — JPOV 体积资产生成工具

> 位置：`tools/jpov/genVolume/`
>
> **离线体积资产生成**：把仿真/VDB 数据烘成引擎可读的 `.jpovvol`，或渲成广告牌粒子用的
> flipbook PNG。**重活全在离线**；JPOV 运行期只读 `.jpovvol` / PNG。
>
> 一句话：**Mantaflow / EmberGen / VDB 是上游数据源，`.jpovvol` 是产出的运行时缓存。**
>
> 历史：本目录 2026-10-16 由 `fire_hero/tools` + `tools/jpov/tools/fire` 合并而来（Danis：「不要散着」）。

---

## 1. 目录内容

```
tools/jpov/genVolume/
├── README.md                # 本文档
├── jpvol.py                 # 【格式单一来源】.jpovvol 读/写 + 公共数组工具（downsample/window/loop）
│
├── # —— 转换器（纯 Python，numpy[/pyopenvdb]，可单测）——
├── npzseq_to_jpovvol.py     # Mantaflow npz 序列 → JPVOL2（含无缝循环）
├── vdb_to_jpovvol.py        # 【通用】OpenVDB（单帧/序列）→ JPVOL2，任意 grid→通道映射
├── fire_family_bake.py      # 生命周期三段（点火/循环/熄灭）烘焙 → JPVOL2 + sidecar
├── make_flipbook.py         # .jpovvol → flipbook PNG（粒子火/烟纹理）
│
├── # —— 仿真场景（需 Mantaflow 运行时；见 env/）——
├── fire_scene.py            # 火（间歇版；旋钮走环境变量）
├── fire_scene_stable.py     # 火（稳定版 ★）
├── smoke_scene.py           # 黑烟
├── fire_family_scene.py     # 四大家族参数化场景（jet/area/pool/transient）
│
├── test_genvolume.py        # 单测（stdlib unittest）
└── env/                     # 依赖/容器统一管理（Mantaflow + Blender + pyopenvdb）
```

---

## 2. `.jpovvol` 格式

定义与读写**只在 `jpvol.py`**（别再各脚本复制一份——历史上正因复制粘贴，一个轴序 bug 同时存在两个转换器里）。

- **JPVOL1**：`magic "JPVOL1\0\0"`, `ver=1`, `nx,ny,nz`, `channels`, `dtype(=0 f32)`，随后 f32 数据（单帧）。
- **JPVOL2**：`magic "JPVOL2\0\0"`, `ver=2`, `nx,ny,nz`, `nframes`, `channels(=2)`, `dtype(0 f32/1 u8)`, `reserved`（40B 头）。
- **体素序**：帧内 **x 最快 → y → z**，通道交错（R=density, G=temperature）；JPVOL2 帧间顺序排列（帧最慢）。
  ⇒ 与「每帧一层 2D 数组（宽=nx、高=ny*nz）」的 GPU 上传布局一致。
- 通道归一化 `[0,1]`。引擎读取器：`tools/jpov/src/volume_manager.cc`（GL）+ `src/fire_fog/fire_volume.h`（GL-free）。

> ⚠️ **pyopenvdb 陷阱**：`grid.copyToArray(a, ijk)` 的 numpy 约定是 **`a[i][j][k] = voxel(i,j,k)`（首轴 = x，
> 形状须 `(nx,ny,nz)`）**，**不是** `[z][y][x]`。按 `(nz,ny,nx)` 分配会 **x/z 对调 + 在 `nz>nx` 时静默截断 z**。
> `vdb_to_jpovvol.py` 已修，并被 `test_genvolume.py` 锁住。

---

## 3. 依赖

**外部程序（Mantaflow / Blender）由 `env/` 的 Docker 镜像统一管理**，不在宿主机手装：

- 依赖清单：`env/dependency.txt`
- 镜像定义 / 用法：`env/Dockerfile` · `env/README.md`
- 构建：`docker build -t jpov-genvolume tools/jpov/genVolume/env`

最小 Python 依赖：`numpy`（转换器）；`pyopenvdb`（仅 VDB 转换器 + VDB 单测）；
`Pillow`（色谱 LUT `make_palette.py` + flipbook `make_flipbook.py`）；`blender`（仅植被工具，见下）。

---

## 4. 工作流

### 4A. Mantaflow → 体积 → flipbook（自产路线）

```bash
# 0) 进 genVolume 并激活环境（env/ 里装的 manta 需在其 waveletNoiseTile.bin 所在目录运行）
cd tools/jpov/genVolume

# 1) 烘仿真：每帧导出 density/heat/flame 到 .npz
RES=96 FRAMES=96 OUT=/tmp/firestable_out manta fire_scene_stable.py
#   火：也可 fire_scene.py（间歇）；烟：smoke_scene.py；家族：fire_family_scene.py
#   旋钮（环境变量）：NOISE_SCALE / VORT_GLOBAL / VORT_FLAME / BUOY_DENS / BUOY_HEAT / SRC_W

# 2) npz 序列 → .jpovvol（取稳态段 + 尾部交叉淡化做无缝循环 + u8）
python3 npzseq_to_jpovvol.py /tmp/firestable_out /tmp/fire_stable.jpovvol \
        --skip 24 --frames 64 --blend 8 --dtype u8
#   ⚠️ 出炉后**一定看一眼**：如果 npz 里只有一根细柱（dens>0.1 的体素 <1%），
#     入库后 viewer 就会渲染成"发光棒"。诊断：读 jpovvol 打 y/z 剖面（见 §2b）。

# 2b) 已有大 .jpovvol → 小体积可入库样例（volume fire viewer 的样例 = 这条）
#     读 → 按密度 bbox 裁剪 → 盒平均降采样 → 取窗口 → 首尾交叉淡化
#     → 边界淡出 → 归一化 → u8（3 通道原样保留：火/温度/烟）
python3 resample_jpovvol.py /james_pm/fire_hero/assets/prototypes/campfire.jpovvol \
        ../../../data/fire_vdb/campfire.jpovvol \
        --crop 0.02 --scale 2 --frames 32 --blend 6 --window 0.10 --dtype u8

# 3) .jpovvol → flipbook PNG（8×8=64 帧；格内留透明内边距、按 bbox 裁剪）
python3 make_flipbook.py fire  /tmp/fire_stable.jpovvol /tmp/fire_flipbook.png 8
python3 make_flipbook.py smoke /tmp/smoke.jpovvol       /tmp/smoke_flipbook.png 8
```

### 4B. OpenVDB（EmberGen 等）→ 体积

```bash
# 通用 VDB → jpovvol：按 --channel 指定「grid→通道」映射（顺序 = R,G,B）。
# 通道只负责“把场装进通道”，物理语义（消光/温度/散射）由渲染侧材质决定。
python3 vdb_to_jpovvol.py '/path/fire_%04d.vdb' out.jpovvol \
        --channel flames --channel flames --channel density:0.35 \
        --start 60 --end 140 --ds 2 --unit 0.01 --z-up
#   --channel GRID[:SCALE[:NORM]]：可重复；SCALE 乘性权重；NORM 归一化口径
#       max / nzmax(默认) / p:X / nzp:X  —— 默认按**非零体素**统计，避免稀疏数组上取分位致通道饱和。
#   --z-up：VDB 为 Z-up（火沿 +z）时转成引擎的 Y-up。默认不转。
#   --rot {none,x90,x-90,y90,y-90,z90,z-90}：额外整体旋转（在降采样之后施加）。
# 单帧：直接给单个 .vdb 文件路径（忽略 --start/--end）→ 写成 nframes=1 的 JPVOL2。
```

### 4C. 四大家族生命周期（点火 / 循环 / 熄灭）

```bash
FAMILY=pool OUT=/tmp/fam_pool manta fire_family_scene.py     # 一步烘 main(点火+持续)+ext(熄灭)
python3 fire_family_bake.py --sim /tmp/fam_pool --out pool.jpovvol \
        --scale 2 --window 0.18 --loop half-xfade             # 拼三段 + 环闭合 + 写 sidecar
```

---

## 5. 已知坑（踩过，务必复用）

1. **pyopenvdb 轴序**：见 §2 警告框（本目录的历史 bug 根因）。
2. **EmberGen = Z-up**：VDB 的 transform 只是几何、**不表达"哪边是上"** → 不加 `--z-up` 火会"躺着"。
3. **`NoiseField.valScale=0`** 会把密度源也归零（整场空）→ 火/烟都要**非零** valScale。
4. **`manta` 必须在其 `waveletNoiseTile.bin` 所在目录运行**（env/ 装好后由脚本保证）。
5. **flipbook 每格需透明内边距**（`make_flipbook.py` 的 `cell` 放大系数），否则广告牌露硬边。
6. 稳定燃烧：`noise.timeAnim=0`（源不随时间忽大忽小）+ 降涡度（`VORT_GLOBAL 0.1→0.02`、`VORT_FLAME 0.5→0.05`）。
   效果：密度总质量相对抖动 70.9% → 7.7%。

---

## 6. 单测

```bash
python3 -m unittest discover -s tools/jpov/genVolume -p 'test_*.py' -v
```

- 纯 stdlib `unittest`；夹具**运行时合成**（不往仓库塞大资产），跑一次 ~0.05s。
- `pyopenvdb` **仅** `VdbReadTest` 需要（VDB 轴序测试）；其余在任何有 numpy 的环境都能跑。
- 进 CI：见仓库 `.github/workflows/ci.yml` 的 `genvolume` job。

---

## 7. 来源 / 授权

- 由本目录脚本烘出的 `.jpovvol` / flipbook PNG 均为**自产衍生资产**。
- Mantaflow：**Apache-2.0**（`github.com/tum-pbs/mantaflow`，0.13）。
- OpenVDB：**Apache-2.0**（ASWF）。EmberGen 免费 VDB：JangaFX **CC0**。
- CC0 真火 flipbook（对比用）：BenHickling「Animated Fire」（OpenGameArt，CC0）。
- Blender：**GPL 独立应用**，仅**离线出资产**用，**不进产品/运行时**（见 `env/README.md`）。
