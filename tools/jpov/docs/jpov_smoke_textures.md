# 粒子烟纹理（6 种，用于 A/B）

particle viewer 的烟纹理与火焰纹理**正交**：顶部下拉「烟纹理」单独选择，
发射逻辑共用一套（参考备份 PR #196 的烟）。

## 三种烟（6 个变体）

| # | 下拉名 | 文件 | 网格 | 采样方式 |
|---|---|---|---|---|
| 0 | `web_others 8×8 (原)` | `assets/smoke/web_others_smoke_8x8.png` | 8×8（64 帧） | `kLifetime`：每个 puff 从第 0 帧播到末帧 |
| 1 | `other 关键帧5 (无旋平)` | `assets/smoke/other_key5_pairs_12x6.png` | 12×6 | `kPairMorph`，`pair_len=12` |
| 2 | `火7.1 冒充烟` | `assets/fire/mp/flames_pairs_v71_12x6.png`（复用火焰图集） | 12×6 | `kPairMorph`，`pair_len=12` |
| 3 | `danis_smoke_5 (旋平)` | `assets/smoke/danis_smoke_pairs_12x6.png` | 12×6 | `kPairMorph`，`pair_len=12` |
| 4 | `danis 无旋平` | `assets/smoke/danis_smoke_norot_12x6.png` | 12×6 | `kPairMorph`，`pair_len=12` |
| 5 | `danis S5=原图#5` | `assets/smoke/danis_smoke_s5orig_12x6.png` | 12×6 | `kPairMorph`，`pair_len=12` |

全部为 `AlignMode::kView` + `BlendMode::kAlpha`（over）+ flipbook。

## 发射逻辑（与 PR196 一致）

- `rate = 0`：烟只在**火焰粒子死亡点**按概率 `spawn`（当前 0.55）。
- 锥形小团：`lifetime 2.4~3.4 s`、`speed 0.14~0.30 m/s`、`size_start 0.08~0.14 m`。
- 尺寸随寿命放大：`0.9 → 2.0 → 3.4`（末端约 0.3~0.5 m）。
- 浮力 `gravity.y = +0.30`、阻尼 `0.55`、噪声 `amp 0.5 / freq 1.1`。
- 颜色梯度：灰黑、alpha `0.10 → 0.38 → 0.26 → 0`（淡入/淡出）。

## 图集生成

### 0. `web_others_smoke_8x8.png`（原 64 帧）

源：`/james_pm/danis/web_others_smoke.png`（804×805，8×8 共 64 帧，黑底）。

- 按 8×8 固定网格切格（每格约 100×100）；
- 每格内容缩到 88% 并居中到 100×100 画布，留出采样安全边；
- `alpha = luminance`，RGB 统一为白（掩膜契约）；
- 放大到 256²/cell，拼成 2048×2048，按 JPOV 行序翻转写盘。

### 1. `other_key5_pairs_12x6.png`（从 64 帧提 5 帧，无旋转平滑）

由 Danis 反馈：原 64 帧帧间差值小、有对不齐。只取 5 个关键帧走 F5 套路：

- 关键帧按「覆盖率分位」选：`[3, 19, 34, 47, 59]`（覆盖 0.14 → 0.43，近似等间隔）；
- 每帧沿用 `key_smoke`（亮度软阈值 + 边缘羽化）；
- **不做旋转平均**；
- `S1..S5 = PROFILE(Fi, F(i+1), 0.5)`；
- 6 条两段链 × 12 帧（与火焰 V4 同一套 `CHAINS`）。

### 2. `flames_pairs_v71_12x6.png`

直接复用火焰 V7.1 图集，不额外生成。

### 3/4/5. danis_smoke_5 的三个版本

源：`/james_pm/danis/danis_smoke_F5.png`（2825×621，5 张 500×500 烟图）。

公共步骤：

1. 5 张源图各取中心 400×400（去掉被画幅切掉的边缘），缩到 256²；
2. 亮度按每张 p95 归一 → 软阈值 `(L-0.18)/(0.80-0.18)`；
3. **径向边缘衰减**（r 从 0.50 到 0.98 平滑到 0）——源图烟充满画幅，
   不做这一步会渲染成方形卡片；
4. `S1..S5 = PROFILE(Fi, F(i+1), 0.5)`；
5. 6 条两段链 × 12 帧。

三个版本的区别：

| 版本 | 旋转平均 | S5 |
|---|---|---|
| 3 `danis_smoke_pairs` | `avg_limited(±15°, σ=8°)` | `PROFILE(F5, F1, 0.5)` |
| 4 `danis_smoke_norot` | 无 | `PROFILE(F5, F1, 0.5)` |
| 5 `danis_smoke_s5orig` | 无 | 直接用处理后的**原图 #5**（`F5`） |

## 烟雾强度归一化

不同来源的贴图白度、覆盖率、空间分布不同，直接换类型会「忽浓忽淡」。
viewer 用 `SmokeTexEntry::alpha_scale` 乘在逐粒子 `color.a` 上：

| 类型 | alpha_scale | 实测平均变暗量 |
|---|---|---|
| web_others | 0.79 | 5.22 |
| other 关键帧5 | 0.58 | 5.40 |
| 火7.1 冒充烟 | 0.85 | 5.22 |
| danis 旋平 | 0.48 | 5.21 |
| danis 无旋平 | 0.39 | 5.32 |
| danis S5=原图#5 | 0.44 | 5.39 |

定标方法：

1. 先按「整张 atlas 的平均 alpha」估初值；
2. 再用 viewer 实际渲染校准：同一火、同一相机、同一帧数，分别在
   `--smoke-density 0`（baseline）与正常密度下拍摄，统计
   `y=40..400, x=430..850` 区域的平均变暗量；
3. 迭代微调 `alpha_scale`，使 6 种类型的平均变暗量差 < 4%。

另有面板滑条「烟雾浓度」（0~3），在上述归一化之上做整体微调。

## 版权

- `danis_smoke_F5.png`：Danis 提供的手绘/AI 生成素材。
- `web_others_smoke.png`：来源待确认（第三方现成 flipbook）。
  **合并进公开仓库前必须确认授权**；若无法确认，建议只在本地实验分支使用。
