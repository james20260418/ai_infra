# 粒子烟纹理（3 种，已固定）

particle viewer 的烟纹理与火焰纹理**正交**：顶部下拉「烟」单独选择，
发射逻辑共用一套（参考备份 PR #196 的烟）。

## 三种烟

| # | 下拉名 | 文件 | 网格 |
|---|---|---|---|
| 0 | `火7.1 冒充烟` | `assets/fire/mp/flames_pairs_v71_12x6.png`（复用火焰图集） | 12×6 |
| 1 | `danis S5 柔边` | `assets/smoke/danis_smoke_s5orig_soft_12x6.png` | 12×6 |
| 2 | `danis 重绘 S1..S5` | `assets/smoke/danis_repaint_12x6.png` | 12×6 |

全部为 `AlignMode::kView` + `BlendMode::kAlpha`（over）+ `kPairMorph`（`pair_len=12`）。

## 发射逻辑（与 PR196 一致）

- `rate = 0`：烟只在**火焰粒子死亡点**按概率 `spawn`（当前 0.55）。
- 锥形小团：`lifetime 2.4~3.4 s`、`speed 0.14~0.30 m/s`、`size_start 0.08~0.14 m`。
- 尺寸随寿命放大：`0.9 → 2.0 → 3.4`（末端约 0.3~0.5 m）。
- 浮力 `gravity.y = +0.30`、阻尼 `0.55`、噪声 `amp 0.5 / freq 1.1`。
- 颜色梯度：灰黑、alpha `0.10 → 0.38 → 0.26 → 0`（淡入/淡出）。

## 图集来源与生成

### 0. `flames_pairs_v71_12x6.png`

直接复用火焰 V7.1 图集，不额外生成。

### 1. `danis_smoke_s5orig_soft_12x6.png`（danis_smoke_5，柔边）

源：`/james_pm/danis/danis_smoke_F5.png`（2825×621，5 张 500×500 烟图）。

1. 5 张源图各取中心 400×400，缩到 256²；
2. 亮度按每张 p95 归一 → 软阈值 `(L-0.15)/(0.75-0.15)`；
3. 径向边缘衰减：F 节点 `0.30→1.00`、S 节点 `0.35→1.00`（长过渡，避免“整片墙”）；
4. `S1..S5 = PROFILE(Fi, F(i+1), 0.5)`；
5. S5 直接用处理后的**原图 #5**（不再用 PROFILE 中点）；
6. 6 条两段链 × 12 帧。

### 2. `danis_repaint_12x6.png`（Danis 重绘，直接当 S1..S5）

源：`/james_pm/danis/danis_repaint.png`（3840×1092，5 格 768×1092，黑底）。

1. 切 5 格；右下角「豆包AI生成」水印区域（x>3500, y>1000）置黑；
2. 每格取 `y=40..1010`，横向补黑到 970×970，缩到 256²（不拉伸）；
3. `alpha = luminance` 软阈值 `(L-0.02)/0.93`；
4. 径向边缘衰减 `0.30 → 1.00`（长过渡）；
5. 这 5 张重绘图直接作为 **S1..S5**；
6. 6 条两段链 × 12 帧：只在 S 节点之间用 `morph_frames(..., "profile")` 插值。

## 烟雾强度归一化

不同来源的贴图白度、覆盖率、空间分布不同，直接换类型会「忽浓忽淡」。
viewer 用 `SmokeTexEntry::alpha_scale` 乘在逐粒子 `color.a` 上：

| 类型 | alpha_scale | 实测平均变暗量 |
|---|---|---|
| 火7.1 冒充烟 | 0.84 | 4.55 |
| danis S5 柔边 | 0.56 | 4.47 |
| danis 重绘 S1..S5 | 0.75 | 4.47 |

定标方法：

1. 先按「整张 atlas 的平均 alpha」估初值；
2. 再用 viewer 实际渲染校准：同一火、同一相机、同一帧数，分别在
   `--smoke-density 0`（baseline）与正常密度下拍摄，统计
   `y=40..400, x=430..850` 区域的平均变暗量；
3. 每种类型都先 Reset 火/烟与随机种子，保证只差纹理；迭代微调使平均变暗量差 < 2%。

另有面板滑条「烟雾浓度」（0~3），在上述归一化之上做整体微调。

## 版权

- `danis_smoke_F5.png` / `danis_repaint.png`：Danis 提供的手绘/AI 生成素材。
- 已移除来源不明的 `web_others_smoke` 及其派生图集。
