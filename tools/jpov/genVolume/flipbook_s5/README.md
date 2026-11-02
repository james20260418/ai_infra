# F5 → S5 → flipbook：把 5 张关键帧做成可循环的粒子序列帧

> 位置：`tools/jpov/genVolume/flipbook_s5/` ｜ 定稿：2026-11-02（水花验收通过）
> 适用：火 / 烟 / 水花 / 爆炸等**任何"几张关键形状 → 连续形变"**的广告牌粒子素材。
> 已落地案例：`assets/fire/mp/*_12x6.png`（火）、`assets/smoke/*_12x6.png`（烟）、
> `assets/water/water_s5_flow_12x6.png`（水花，见 `docs/jpov_particle_water_splash.md`）。

---

## 0. 一句话

**5 张关键帧 → 相邻帧中点 S1..S5 → 6 条两段链 × 12 帧 → 12×6 图集。**
每颗粒子随机选一条链，在寿命内从链首走到链尾 —— 5 张图就能变成 72 种不重样的形变过程。

```
F1..F5 ──morph(0.5)──► S1..S5 ──链(S_i→S_j→S_k)──► 12 帧/块 ×6 ──► 12×6 atlas
```

---

## 1. 为什么要绕一层 S

直接 `F1→F2` 补间只有 4 段，每颗粒子看到的是"两张原图之间的硬过渡"；
**先把中点算出来当节点**，再从中点出发走两段，好处：

1. 链内形变速率均匀（每段长度相近），不会"前 3 帧几乎不动、最后 1 帧突跳"；
2. 随机选块时不会总是撞到同一段（6 条链 × 12 帧分散了相位）；
3. 原图只当下限/上限用，**中间态是形变出来的**，不会出现"原图硬切"。

链的取法（`CHAINS`）：

```
S1→S2→S3   S2→S3→S4   S3→S4→S5
S4→S5→S1   S5→S1→S2   S1→S3→S5   ← 最后一条故意跨一段，增加方差
```

每块 12 帧 = 第一段 6 帧（t = 0, 1/6, …, 5/6）+ 第二段 6 帧（t = 1/6, …, 1）。
**中点只出现一次**（第一段末帧 ≠ 第二段首帧，否则同一帧重复）。

---

## 2. 形变算子：FLOW（唯一验收通过的）

`morph_flow.py` = Farneback 稠密光流：A→B、B→A 各算一次，各自 warp 到中间时刻再按 t 融合。

- **FLOW**：细丝、飞沫、分叉都能保住 → 水花/爆炸/多分叉素材必须用它。
- **PROFILE**（逐行中心线+半宽，本工具不提供）：只适合**竖直单体柱**（火舌、烟柱）。
  遇到 C 形/星形/斜带会把分叉并成一根"主柱"，出来偏糊、偏"充气"。
  > 火焰/烟当年用的是 PROFILE；水花实测 PROFILE 明显更差（见 `jpov_particle_water_splash.md` §4）。

参数（`morph_flow.PARAMS`）：`pyr_scale=0.5, levels=4, winsize=31, iterations=5,
poly_n=7, poly_sigma=1.5`，算光流前 `GaussianBlur σ=2`。**改了要重新验收出图。**

---

## 3. 素材准备（`extract_keyframes.py`）

输入通常是 Danis 给的**横排一张**（黑底白花）。脚本负责：

1. 切格：按列亮度低谷自动找切割线（也可 `--splits` 手工给）；
2. 取 alpha：`alpha = smoothstep((亮度 - 背景) / 0.85)` —— 背景是黑的，亮度就是形状；
3. 归一化：每格 bbox → 缩放到 `CELL × CELL`，**最长边占 80%**，居中；
4. 去水印：`--cut-bottom N` / `--cut-top N` 先切掉一条边。

> ⚠️ **每格必须归一到同一画布坐标**。形变是逐像素光流，两格若比例/位置不一致，
> 光流会把"整体缩放"当成形状变化，出来会晃。

---

## 4. 图集约定（和引擎的契约，错了就是整张图乱帧）

| 项 | 值 | 原因 |
|---|---|---|
| 列 × 行 | **12 × 6**（72 帧） | 6 条链 × 12 帧；单格 256px ⇒ 3072×1536 |
| 单格 | 256×256 | 再大浪费显存；再小近景糊（4K 屏可上 384/512） |
| **行序** | `row_img = tilesY - 1 - row_frame` | JPOV `CellUV` 用 `vrow = tilesY-1-row`，PNG 第 1 行 = v=0 ⇒ **frame 0 在最后一行** |
| RGB | 全白 (255,255,255) | 形状全在 alpha；颜色交给渲染端梯度/温度映射 |
| 空槽 | 透明 | 12×6 整除，一般没有空槽 |

引擎侧对应：`SheetTimeMode::kPairMorph` + `pair_len = 12` + `start_frame_random = true`
（每颗粒子随机选一块，`age/life` 在块内 0→1）。

---

## 5. 用法

```bash
cd tools/jpov/genVolume/flipbook_s5

# 1) 拆关键帧（黑底横排图 → F1..F5）
python3 extract_keyframes.py --src /james_pm/danis/water_splash.png --out /tmp/f --cut-bottom 160

# 2) 出图集
python3 build_s5_atlas.py --frames /tmp/f --out water_s5_flow_12x6.png

# 3) 拷进 assets（构建脚本会把 assets/water/*.png 拷进 output/textures/water/）
cp water_s5_flow_12x6.png ../../../assets/water/
```

依赖：`numpy`、`opencv-python`、`Pillow`。

---

## 6. 粒子侧的配套经验（都是踩出来的）

| 问题 | 做法 |
|---|---|
| 图集缩小后闪烁/像盐粒 | 纹理注册开 **mipmap**（`TextureOptions{.mipmap=true}`）；cell 越小越必须 |
| 俯瞰穿帮（粒子变竖片） | 对齐用 **`kView`**，不要 `kVertical`（vertical 在 70° 俯视时 quad 侧对相机） |
| 加法叠起来糊成一坨白玉 | 颜色压暗（×0.3~0.4）+ 降不透明度；或切覆盖集对比 |
| 顶点不散 | 用「核心粒子死亡点 `SpawnAt`」而不是第二套发射器：死亡点天然在抛物线顶点 |
| 柱体串珠 | 核心速度区间收窄（间距均匀）+ 单粒子尺寸 ≥ 间距 |
| 寿命 < `2v/g` | 粒子在最高点就死，画面上永远看不到回落段 |
| 一颗粒子看到整张图 | `flipbook=true` 但 `tiles` 没设 / `sheet_time_mode` 不是 kPairMorph |

---

## 7. 验收清单

- [ ] 图集 12×6、单格正方形、**frame 0 在最后一行**（拿 `--frames` 的 F1 对比第 6 行第 1 格）
- [ ] 肉眼过一遍 6 行预览：链内无撕裂、无重影、无"原图硬切"
- [ ] 在 viewer 里两种混合（加法/覆盖）各看一遍，俯视 70° 再看一遍
- [ ] 与上一版出图做 **md5 对比**（只改代码不改观感时必须逐字节一致）
