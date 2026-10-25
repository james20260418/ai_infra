# JPOV 深度 / 法线工具集

> 位置：`tools/jpov/tools/depth_tool/`（MoGe 深度）+ `tools/jpov/tools/texture_tool/`（Sobel 法线）
>
> 给「一张平面纹理 / 一张照片」生成 **法线、高度、深度** 的离线工具集。
> 两个工具互不依赖：Sobel 是纯 CPU 毫秒级，MoGe 要 GPU + 1.5GB 权重。

---

## 0. 先用哪张表判断

| 你手上有什么 | 想要什么 | 用哪个 | 现实预期 |
|---|---|---|---|
| 平铺材质贴图（无光照的 albedo） | 可平铺的法线/高度 | **`jpov_texture_normal` + `integrate_normal.py`** | 锐利、可平铺，但会"把亮度当几何" |
| 一张真实照片（有光影） | 场景/物体的整体深度 | **`moge_depth.py`**（MoGe-3） | 大形很准，**微起伏基本没有** |
| 平铺贴图 | 物理正确的浮雕 | 都不行 → 需要多光照照片做光度立体 | 见 §2 |
| 已有法线图 | 高度场 | `integrate_normal.py` | 忠实于该法线 |

**一句话**：单图 AI 深度模型**不适合做材质微浮雕**，这一点有实测数据支撑（§2.1）。

---

## 1. 三个工具

| 工具 | 语言/设备 | 输入 | 输出 | 依赖 |
|---|---|---|---|---|
| `texture_tool/jpov_texture_normal` | C++ / CPU | 任意图（按灰度高度场处理） | 法线 PNG（GL 切线空间） | stb（仓库自带） |
| `depth_tool/moge_depth.py` | Python / CUDA | RGB 图 | 深度 npy/16-bit/灰度、法线、掩码、meta | torch + MoGe + 模型权重 |
| `depth_tool/integrate_normal.py` | Python / CPU | 法线 PNG 或 npy | 高度 npy/16-bit/灰度 + 往返误差 | numpy + opencv |

三者可以串起来：**纹理 → Sobel 法线 → 积分高度**；也可以互相校验：**Sobel 高度 ↔ MoGe 深度**。

---

## 2. 实测结论（2026-10-23，RTX 5060 Ti）

### 2.1 单图深度模型对材质微浮雕无效（有真值）

真值素材：Poly Haven `bark_brown_01`（**CC0**），法线与位移图自洽度
`corr(法线积分高度, disp) = 0.949`，即这套真值可信。

| 输入 | 平均角误差 | >30° 像素 | 与真值 nx 相关 |
|---|---|---|---|
| **平坦法线（什么都不做）** | 23.90° | 32.1% | 0 |
| MoGe-3 · 无光照 albedo | 23.64° | 31.1% | 0.229 |
| MoGe-3 · 重新打光（单灯） | 23.79° | 31.5% | 0.204 |
| MoGe-3 · 打光 + AO | 23.49° | 30.7% | 0.263 |
| **光度立体（6 张不同方向光）** | **5.62°** | **1.2%** | ≈1.0 |

**结论**：给它光影线索也救不回来（23.90 → 23.44，只降 0.5°）。瓶颈不是"没光照"，
而是**单图深度模型估计的是场景布局，输出是低频场**，表达不了 mm 级浮雕。
真正需要准确浮雕时，走多光照（光度立体）。

### 2.2 真实照片：MoGe 抓大形，Sobel 抓细节，两者几乎正交

素材：`test_brick.png`（4284×5712 砖墙照片），推理尺寸 1536×2048。

| 指标 | MoGe-3 | JPOV Sobel + 积分 |
|---|---|---|
| 浮雕幅度（去平面后） | 0.0091 m ≈ **9 mm**（占 2.13 m 的 0.4%） | 0.232（任意单位） |
| 边缘锐度 p99(\|∇\|)/std | 0.6 | **10.6（17.7×）** |
| 与亮度残差相关 | −0.36 | **+0.96** |
| 与对方浮雕的相关 | **−0.007（几乎正交）** | — |
| 平铺 | 接缝会裂 | wrap 采样，天然可平铺 |
| 耗时 | 4.9 s（+18.6 s 加载） | 端到端 5.4 s @24.5MP |

**读法**：MoGe 不被照片明暗带偏（几何上更"干净"），但太平；Sobel 锐利，但基本是在抄亮度。
两者相关性接近 0，说明它们提供的是**互补**信息，不是同一个量的两种估计。

### 2.3 Sobel 的各种配置（同一张砖墙）

| 变体 | leak（越低越不吃光照） | sharp | 平坦区/边缘区梯度比 |
|---|---|---|---|
| S1 Sobel scale2 | 0.990 | 7.1 | 0.176 |
| S2 预模糊 σ3 + scale4 | 0.896 | 3.3 | 0.307 |
| S3 Scharr | 0.990 | 7.2 | 0.176 |
| S4 中心差分 | 0.999 | 7.2 | 0.177 |
| S5 CLAHE + Sobel | 0.965 | 7.0 | 0.236 |
| C1 局部对比归一 + Sobel | 0.876 | 8.3 | 0.268 |
| C2 多尺度 Retinex + Sobel | 0.912 | **10.8** | **0.129** |
| C3 DoG 带通 + Sobel | **0.848** | 6.4 | 0.243 |

**结论**：换算子（Sobel / Scharr / 中心差分）结果几乎完全一致，**没有调优空间**。
差别来自预处理。所有变体共有的"双边缘"（每条砖缝变成一沟一坎）是**单图亮度法的原理性缺陷**，
调参去不掉。

### 2.4 融合（低=MoGe，高=Sobel）

| 融合方式 | 亮度泄漏 | 锐度 |
|---|---|---|
| A 固定高通叠加 | 0.888 | 7.9 |
| B 拉普拉斯金字塔 | 0.749 | 8.4 |
| C 边缘掩膜（只在强边取 Sobel 细节） | **0.573** | 6.9 |

C 把亮度泄漏从 0.956 压到 0.573，同时保留 11 倍于 MoGe 的锐度。**融合脚本尚未入库**，
需要时按本节结论实现。

---

## 3. 安装

### 3.1 Sobel 工具（C++，无需安装）

```bash
# 在仓库根目录
bazel build //tools/jpov/tools/texture_tool:jpov_texture_normal
./bazel-bin/tools/jpov/tools/texture_tool/jpov_texture_normal --help
```

也可以直接用 g++ 编（单文件 + 仓库内 stb）：

```bash
g++ -O2 -I . tools/jpov/tools/texture_tool/normal_map.cc -o /tmp/jpov_texture_normal
```

### 3.2 MoGe（Python，需要 GPU）

```bash
cd tools/jpov/tools/depth_tool
chmod +x install_moge_env.sh

# 重要：--env-dir 指到挂载盘，否则 venv(≈6GB)+权重(1.5GB) 会撑大镜像层
./install_moge_env.sh --env-dir /james_pm/gpu_envs/moge
```

脚本做的事：建 venv（`--copies`）→ 装钉死版本的 torch（默认 `2.14.1+cu130`）→
装钉死 commit 的 MoGe → 跑一次 CUDA 自检。缓存全部落在 env 目录内
（`PIP_CACHE_DIR` / `HF_HOME` / `TMPDIR`）。

- **50 系（sm_120）必须用 cu128/cu130 的 torch wheel**，老的 cu121 会报
  `no kernel image is available`。
- **HuggingFace 在很多容器里直连不通**。脚本会自动探测，不通则改用
  `HF_ENDPOINT=https://hf-mirror.com`。
- venv 内是绝对路径，**env 目录不要移动/改名**，否则重建。

| 资源 | 占用 |
|---|---|
| venv（含 CUDA 运行库） | ≈ 6 GB |
| 权重 `Ruicheng/moge-3-vitl` | 1.48 GB（`moge-3-vitg` 为 5.0 GB） |
| 峰值显存 | ViT-L 约 3~4 GB |
| 峰值内存 | 权重 1.5 GB（mmap 后主要是 page cache） |

---

## 4. 用法

> 下面命令都从 `tools/jpov/tools/depth_tool/` 起算，路径尽量用相对路径。

### 4.1 照片 → 深度

```bash
cd tools/jpov/tools/depth_tool
. /james_pm/gpu_envs/moge/venv/bin/activate

# 2048 长边、resolution_level 9、2 步细化
python moge_depth.py ../../../../test_brick.png --out-dir ../../../out/depth_brick
```

大图想更快就 `--max-side 1024`；想要更细就把 `--reslv` 提到 9（默认已是 9）并加大 `--max-side`。

### 4.2 纹理 → 法线 → 高度

```bash
# 1) Sobel 法线（scale 越大浮雕越强）
/tmp/jpov_texture_normal ../../../../assets/bark.png ../../../out/bark_normal.png 3

# 2) 积分成高度（平铺贴图建议加 --detrend 去掉整体倾斜）
python integrate_normal.py ../../../out/bark_normal.png --out-dir ../../../out/bark_height --detrend
```

`integrate_normal.py` 默认会打一行 **往返误差**（把高度重新求导成法线再比对）：

```
roundtrip mean angle = 0.42 deg  (>10 度通常意味着符号/轴向搞错了)
```

这条是排查"法线约定搞反"最快的信号——搞反时误差会从几度跳到几十度。

### 4.3 自检（无需任何素材）

```bash
python integrate_normal.py --selftest
```

合成一个已知高度场，算法线 → 积分回来，要求 `corr > 0.99`；
同时跑一个"Y 轴取反"的反例，要求 `corr < 0.90`。反例失败说明测试本身失效了。

---

## 5. 输出文件

`moge_depth.py`：

| 文件 | 说明 |
|---|---|
| `depth.npy` | float32，单位米，未归一化 |
| `depth_16bit.png` | 按 p1~p99 归一化到 0..65535 |
| `depth_gray.png` | 8-bit 预览，**默认近=白**（`--far-white` 反转） |
| `normal_gl.png` | 切线空间法线，**OpenGL 约定（+Y 朝上）** |
| `mask.png` | 有效区域（255 = 有效） |
| `meta.json` | 模型、参数、尺寸、深度统计，便于复现 |

`integrate_normal.py`：`height.npy` / `height_16bit.png` / `height_gray.png` / `meta.json`
（含 `roundtrip_mean_angle_deg`）。

---

## 6. 已知限制（必读）

1. **单图深度 ≠ 材质浮雕**。在 `bark_brown_01` 上 MoGe 平均角误差 23.64°，
   只比"全平法线"的 23.90° 好 0.26°；在另一张 `bark001` 贴图上甚至更差
   （25.8° vs 23.5°）。亮度/语义线索都不足以恢复这种尺度的浮雕。
2. **Sobel 会把明暗当几何**。相关系数 0.96 意味着输出基本是亮度的高频复制；
   高光、阴影、脏污都会变成假的起伏。
3. **双边缘**。每条边在积分后变成"沟 + 坎"，所有单图亮度法共有，调参去不掉。
4. **平铺接缝**。Sobel 用 wrap 采样，可平铺；MoGe 没有周期约束，平铺会出现竖缝
   （实测接缝跳变 122 vs 内部 0.49）。要平铺就用 Sobel 那条链路。
5. **内存**。容器 cgroup 限制在 6~8 GB 时，`from_pretrained` 会被 OOM 杀掉；
   本工具用 mmap + `assign=True` 加载正是为此。若 `mmap` 失败会自动退回常规加载
   （此时需要足够内存）。
6. **法线约定**。MoGe 输出 OpenCV 相机系 `(x右, y下, z前)`，本工具转成 GL 切线空间
   `(x右, y上, z出)`。`integrate_normal.py --format dx` 用于 DirectX 法线。
7. **分辨率**。MoGe 推理尺寸由 `--max-side` 决定，输出深度是**该尺寸**，不是原图尺寸。

---

## 7. 许可证

| 组件 | 许可 | 备注 |
|---|---|---|
| MoGe 代码 / 权重 | **MIT** | `microsoft/MoGe` 代码 MIT；HF 权重卡 license 也是 MIT |
| stb | Public Domain | 仓库自带 |
| 本目录脚本 | 随本仓库 | — |

⚠️ 若将来引入 **Depth Anything 3** 做对比：它的 `DA3-LARGE` / `DA3-GIANT`
权重是 **CC BY-NC 4.0（禁商用）**，只有 `DA3-BASE`(0.12B) / `DA3-SMALL`(0.08B)
是 Apache-2.0。**DSINE 是帝国理工自定义协议，明确禁止商用。**

测试素材建议用 **Poly Haven / ambientCG**（都是 CC0），不要提交到仓库：

```bash
curl -O https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/bark_brown_01/bark_brown_01_diff_1k.png
curl -O https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/bark_brown_01/bark_brown_01_nor_gl_1k.png
curl -O https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/bark_brown_01/bark_brown_01_disp_1k.png
```

---

## 8. 复现 §2 的数据

```bash
cd tools/jpov/tools/depth_tool
. /james_pm/gpu_envs/moge/venv/bin/activate

# 2.1：MoGe 对比 CC0 真值（需先把上面三张 bark_brown_01 下到本地）
python moge_depth.py <dir>/bark_brown_01_diff_1k.png --out-dir ../../../out/ph_bark
# 然后用 nor_gl 当真值算角误差：
#   n_ts 由 normal_gl.png 解码；指标见 §2.1 表

# 2.2 / 2.3：Sobel 链路 + 8 个变体
#   预处理与算子组合见 §2.3 表；亮度泄漏 = corr(去趋势高度, 去趋势亮度)
```

历史脚本（`exp_ph_bark.py` / `classic_variants.py` / `fuse.py`）留在实验机的
`/james_pm/gpu_envs/` 下，未入库；本 README 记录的是它们的结论与可复现口径。
