# 水皮 V2 设计：单像素多法线采样（Mip the ground-truth sea）

> 状态：**设计定稿（2026-11-25）**；**实现已落地**（同日，分支 `feature/20261125-jpov-water-skin-v2`，
> 文件清单见 §12）。gold 与谱截断仍不在本 PR 内（见 §11）。
> 实现与本文的偏差只有两类，都已在正文里就地注明：椭圆**半轴**（§5.1）、相位局部系的做法（§8.1）。

---

## 0. 一句话

把「近场逐像素解析 + 一堆远场补偿」改成：

> **一个公式 + 每像素 `S` 个法线采样 + 平均**。
> 不给距离阈值、不给谱截断、不给补偿项——因为像素本来就是面积的积分器。

并把 9 个魔数/补偿一次性退休。

---

## 1. 为什么做 V2（V1 的四个口径问题）

V1 的近场着色函数 `ShadeAbove()`（`water_skin_shader.h`）本身是对的（解析法线 + 解析菲涅尔 +
探针 + GGX），但它在**掠射/远距离**必然崩，因为它有四个口径问题：

| # | 问题 | 后果 |
|---|---|---|
| 1 | `F` 是**半球平均**（`RoughFresnelAlbedo`），`refl` 是**单方向点采样** ⇒ `C = F·L(r̂)` 把 `L` 移出了积分 | 地平线附近天空有强梯度时误差大 |
| 2 | 高光带宽 `a2 = 盘角² + footprint_a2 + 2·normal_var_px` 混了**物理带宽**和**屏幕空间 AA 量**，还靠 `min(…, 0.25)` 兜底 | 掠射时 `dFdx(H)` 爆掉 ⇒ 太阳变成 26° 大糊斑 |
| 3 | `cos_i` 由解析法线给出，波峰/波谷极点处相邻像素剧变，**`F` 本身没有过滤** | 波峰交界麻点/颜色噪点 |
| 4 | `env_lod_cap = mix(1.5, 3.0, smoothstep(0.05,0.35,rdir.y))` 是**遮丑**（探针下半球是地色） | 各向同性圆盘模糊，且盖住了探针缺陷 |

**共同根源：V1 用「一个 (pos, N) + 若干二阶量（方差/屏幕导数）」去近似「一个像素内的面积分」。**
补偿项都是在替"没做采样"擦屁股。

---

## 2. 心智模型：大 gt 海 + mip

- **大 gt 海**：把近场公式延伸到**无限分辨率**，且**不做任何谱截断** —— 每个像素是一个点，72 条波全部作为几何/法线参与。这是海色与透射率的**真值**。
- **我们的处境**：分辨率有限 ⇒ 像素是**面积的积分器** ⇒ 需要一个**降采样**。
- **V2 就这一步**：用 `S` 个法线采样做**数值 mip**。

> V2 **不做**解析 mip（谱截断 / 闭式 BRDF）。那是 V3 的话题（见 §11）。

### 2.1 一个必须承认的物理事实：噪声起点比"远海"近得多

最短的表内波是细节波的 **0.3 m**。当 `footprint ≳ 3 × 0.3 m ≈ 0.9 m` 时，采样开始不够：

| 视线仰角 | `footprint` 近似 | 噪声起点 |
|---|---|---|
| 45° 俯视（`\|rd_y\|≈0.7`） | `t · 2.3e-3` | **≈ 400 m** |
| 10° 俯视（`\|rd_y\|≈0.17`） | `t · 9.4e-3` | ≈ 100 m |
| **1° 掠射（`\|rd_y\|≈0.017`）** | `t · 9.4e-2` | **≈ 10 m** |

**这是 V2 要测的核心现象**：「多远开始摩尔纹」。它会在掠射视线下**十几米**就出现。

---

## 3. 三段式契约

```
① ResolveSurfaceForPixelCenter(scene, camera, pixel) -> (pos, N, t)   // 几何，唯一真状态
② if occluded(t_plane, sceneDepth) discard                            // 可见性
③ (C, alpha) = mean_{i=0..S-1} Shade(N_i, rd; Lighting, ior)          // 着色，纯函数
   emit(C, alpha, t*alpha)                                            // WBO 三槽
```

### 3.1 中间变量（卡片）

| 字段 | 类型 | 单位 | 来源 |
|---|---|---|---|
| `pos` | `vec3` | 米 | 求交（**唯一真状态**） |
| `N` | `vec3`（单位） | — | 解析梯度（**唯一真状态**） |
| `rd` | `vec3`（单位） | — | **派生** = `normalize(pos − cam)` |
| `footprint` | `float` | 米 | **派生** = `t · pixel_angle / max(\|rd.y\|, ε)` |

- 着色只需要 `pos + N`（+ 相机 + 光照）。
- `rd`/`footprint` 作为字段保留只为了：省一次 `normalize`/除法，以及**逐字节一致性**（
  `normalize(pos−cam)` 与 NDC 反构的 `rd` 末位可能差 1e-7）。

### 3.2 三个"深度"别混

| 量 | 定义 | 谁用 |
|---|---|---|
| `t`（射线距离） | `\|pos − cam\|` | **WBO att3（`t·α`）**、合成趟 `FieldAt(ro,rd,z)`、footprint |
| `z_view`（视轴深度） | `t · dot(rd, fwd)` | **与场景深度比较**（遮挡） |
| `gl_FragCoord.z` | 投影后 [0,1] | 深度图（MRT#1）里实际存的 |

> ⚠️ att3 必须是 `t`。写视轴深度会让层被放早 `1/cosθ`（角落 ~1.5×），且贴水面那段水体的注入被吃掉。

---

## 4. V2 口径表

| 项 | V1 | **V2** |
|---|---|---|
| 表内 72 条波（8 主 + 64 细节） | 按 `λ/footprint` 截断（3~8 px smoothstep） | **全部采样，不截断** |
| MSS（毛细波 / 短重力波） | 进 `var_p_mean` | **保留**：σ 偏置，喂 `Albedo` / lobe / env_lod（见 §6） |
| 法线 | 逐像素解析（1 个） | **S 个采样** |
| 反射率 `F` | `RoughFresnelAlbedo(cos_i, var_p_mean)` | `RoughFresnelAlbedo(cos_i, σ²_MSS)` **逐样本** |
| 天空反射 | `textureLod` + `env_lod`（各向同性） | 每个样本 **LOD0 采样**，模糊只来自平均 |
| 太阳高光 | GGX，`a2 += footprint_a2 + 2·normal_var_px`，上限 0.25 | GGX，`a2 = 盘角² + 2·σ²_MSS`，**无上限** |
| `far` 概念 | 二分流（near/far） | **不存在** —— 同一公式跑到底 |
| 平均 | 无 | **每样本各自着色 → 平均结果** |

### 4.1 V2 删掉的 9 项

| # | 项 | 为什么能删 |
|---|---|---|
| 1 | `kWaveLodPxMin/Max` + 全部 `lod_amp` | 不截断 |
| 2 | `var_p`（入射面投影） | 无方差通路（MSS 之外） |
| 3 | `var_p_mean` | 换成逐样本的 `cos_i` + `σ²_MSS` |
| 4 | `normal_var_px = 0.5·\|dFdx N\|²` | 样本自己给出 |
| 5 | `footprint_a2 = 0.25·(\|dFdx H\|²+\|dFdy H\|²)` | 同上 |
| 6 | `env_lod` 的 `+normal_var_px` 项 | 同上 |
| 7 | `env_lod_cap = mix(1.5, 3.0, smoothstep(…)` | 不再靠钳 LOD 遮丑 |
| 8 | `a2` 的 `min(…, 0.25)` 兜底 | 没有会爆的 dFdx 了 |
| 9 | 远场二分流（`far` / `FarWeight`） | 同一公式 |

**净效果：`ShadeAbove` 里不再有 `dFdx`。** 历史上反复追的「导数必须先于 `discard`」这条
不变式**整条消失**（结构性地不可能再犯）。

---

## 5. 采样

### 5.1 参数

| 参数 | 值 | 说明 |
|---|---|---|
| `S` | **默认 4，滑条 1…16** | `S=1` = 纯中心法线（退化） |
| 抖动图案 | **Hammersley 2D（静态）** | 低差异、确定性 ⇒ 摩尔纹**静态可复现**，便于诊断 |
| 分布域 | **椭圆足迹内均匀** | **半轴**：长 `footprint/2`，短 `footprint·|rd.y|/2`（推导见下） |

**半轴口径（实现时修正，2026-11-25）**：`footprint = t·pixel_angle/|rd.y|` 是**一个整像素**
沿地面径向的世界宽度，所以像素只覆盖 `±footprint/2`。把 `footprint` 直接当半径会让采样盘
变成 2×2 像素（面积 4×）= 白送一级 mip，并把「噪声起点距离」系统性拉近约 2×
（而那正是 V3 定分工线要用的表）。
短轴的物理量是 `t·pixel_angle`（横向方向本身就在水面内，不被 1/|rd.y| 拉长）：
`t·pa = footprint·|rd.y|`。原稿写的 `pa_x/pa_y` 只在方形像素下等于 1（⇒ 圆盘），
漏掉了 `|rd.y|` 这个真正的各向异性因子（掠射时短/长 = |rd.y| ⇒ 1° 时 0.017，横向成条）。

### 5.2 为什么静态、不做时域抖动

- 静态 ⇒ 摩尔纹**不闪**，能一眼分辨"采样不足"与"帧间噪声"；
- 静态 ⇒ **确定性**，未来可做逐字节 gold；
- 时域抖动（黄金角 + TAA）是 V3（见 §11）。

### 5.3 位置与法线

```glsl
// 采样位置：像素中心的平面点 + 椭圆抖动
vec2 p_i = p_center + jitter_i;              // p_center = 中心交点（含高度置换）
vec3 N_i = WaveNormalAt(p_i);                // 8 主 + 64 细节，同一公式，无 lod_amp
```

- **主波与细节波用同一个公式**（都是 `sin(k·dot(d,p)+φ)` 的解析梯度）。
- **MSS 不进采样**（它没有方向/相位）。
- 每像素算一次的量（如 `pixel_angle`、`σ²_MSS`）在循环外算好，S 个样本复用。

---

## 6. 每样本着色（必须预乘）

$$\big(C,\alpha\big)=\frac1S\sum_{i=0}^{S-1}\Big(\underbrace{F_i\cdot \mathrm{Env}(\hat r_i)}_{\text{反射}}+\underbrace{\mathrm{Spec}(N_i)}_{\text{高光}}\Big),\quad
\alpha=\frac1S\sum_i\Big(1-\frac{1-F_i}{n^2}\Big)$$

```glsl
// 记号：sigma2_mss = 0.5·trace(MSS 张量) = MSS 的**各向同性斜率方差** σ²。
//（MSS 是斜率张量的迹 = 2σ²；代码里的 uShortWaveMss 就是迹，别把 √迹 当标准差用。）
float Fi  = RoughFresnelAlbedo(cos_i, sigma2_mss, uWaterIor);   // MSS 保留
vec3  rdi = ClampAboveHorizon(reflect(rd, Ni));
float lod = Clamp(log2(max(2*sqrt(sigma2_mss), 1e-6)/kEnvTexelAngle), 0.0, 1.5);
vec3  refl_i = Fi * textureLod(uEnvCube, rdi, lod).rgb * uEnvScale;
vec3  spec_i = GgxSun(Ni, rd);                                   // 内部已含 F
float a_i    = 1.0 - (1.0 - Fi)/(n*n);
```
累加 `refl_i + spec_i` 与 `a_i`，最后各除以 `S`。

### 6.1 ⚠️ 三条必须写死的纪律

1. **不能平均法向再着色一次**：`mean(Shade(N_i)) ≠ Shade(mean(N_i))`（Fresnel/reflect/GGX 非线性）。
2. **不能平均 env 再乘平均 F**：`mean(F_i·E_i) ≠ mean(E_i)·mean(F_i)`（掠射时 `F_i` 跨度极大）。
3. **`C` 与 `alpha` 必须用同一批样本**，否则反射/透射的能量账不守恒。

`alpha` 取平均是**正确的**（透射率线性：`1 − mean(T_i) = mean(α_i)`）。

---

## 7. MSS 的定位（V2 保留）

MSS = Cox–Munk 短波斜率方差，**不在波表里**（它不是 72 条之一），因此**从来不是"可采样"的东西**。

| 性质 | 含义 |
|---|---|
| 无方向、无相位 | 它永远不会体现为法线 |
| 空间常数（给定风速） | **不可能产生纹理** |
| 只影响三处 | `Albedo`（⇒ F/alpha）、太阳 lobe 宽度、env 模糊 |

⇒ 在 V2 里它是 **σ 偏置项**：`σ² = σ²_MSS`（表内波全部采样，不进 σ）。

> **为什么"不截断"但仍留 MSS**：截断只针对**表内 72 条波**；MSS 本来就不在表内。

**为什么必须留（Danis 2026-11-25 定）**：它对观感影响大（掠射反射率、太阳高光展宽），
且需要与 V1 做对照。

---

## 8. 数值细节

### 8.1 相位精度（V2 必做）

法线是 `sin(k·dot(d, p.xz) + φ)`。V2 要在**大足迹**上采样 ⇒ `p.xz` 可能到 1e5~1e6 量级：

| `p.xz` | `k·x`（k≈0.05） | float32 相位误差 |
|---|---|---|
| 1e2（近场） | 5 | ~1e-6 rad |
| 1e4 | 500 | ~6e-5 rad |
| **1e6** | **5e4** | **~6e-3 rad（0.34°）** |

**方案：局部原点 + CPU 预算相位偏移**（纯数值等价，不改数学）

```
CPU（每帧，double）:  origin  = 1 km 量化(相机 xz)
                      φ_i'    = fmod(φ_i + k_i · dot(d_i, origin), 2π)
GPU:                  p_local = cam_local.xz + rd.xz·t        // cam_local = cam − origin（CPU 减）
                      phase   = k_i · dot(d_i, p_local) + φ_i'
```
三个实现要点（**都是实现时才发现、原稿漏了**）：

1. **必须在「算位置之前」就换成局部坐标**：`p = cam + rd·t` 这一步在 1e6 m 量级上 ulp
   已经是 0.06 m，细节波（k≈21/m）的相位会震 ±1.3 rad —— 先算 p、再减 origin 救不回来
   （减完还是那个已经丢位的数）。所以 renderer 用 double 传一条 `uCamPosXZLocal`，
   水皮的求交/求法线全在局部系里做（y 保持世界系，`uWaterLevelY` 不变）。
   V1 与天球仍传世界坐标（origin=0 ⇒ 逐位不变）。
2. **初相必须 `fmod 2π`**：origin 会把 φ' 推到 k·|origin| 量级（细节波 21 × 1e6 = 2e7），
   直接 cast 成 float 的量化误差是 0.25~2 rad。而 `origin` 每 1 km 换一次 ⇒ 换格瞬间整片波
   相位跳一个随机值（近场可见的 pop）。fmod 在 double 里做，量化误差回到 ~1e-7 rad。
3. **flow map 必须世界锚定**：局部坐标直接乘 uv 会让 flow 图案随相机每 1 km 滑动一次
   ⇒ 采样前加回 origin（`WaveFlowAt(p + uWaveOriginXZ)`）。
- 每波一个 uniform（8 主 + 64 细节 = 72 float），每帧一次；
- `origin` 取相机位置的整千量化（避免逐帧跳变）；
- **预期代价**：与不加相比结果差 **0~1 LSB**（不是数学变化，是浮点重排）。

### 8.2 多根跳变（**不治，照抄 V1**）

`WaveSurfaceHit` 的 12 次二分在区间内有多根时会收敛到不同根 ⇒ `t` 跳变（≈ span）。
Danis 2026-11-25 定：**没看到实际报障，照抄 V1，不动。**

V2 的额外好处：多采样会把这类噪声按 `1/√S` 衰减（每次跳根只扰动一组样本中的一点）。

### 8.3 地平线饱和（**已知 open problem，V2 不解决**）

`footprint = t·pa/|rd.y|`，`|rd.y| → 0` 时 → 1e6 ⇒ 抖动偏移 1e6 m ⇒ 法线近似随机 ⇒ 采样失效。
- V2 里地平线附近的观感就是"糊成一片/噪"；
- **必须的过渡机制留到 V3**（Danis 明确：没有纯 S_max 的效果前无法定这个算法）。

护栏（保留，不参与物理）：
- `kWaveGrazeFloorSin = 1e-4`（除零）
- `kWaterHorizonFloorRad = 1e-3`（地平线半像素覆盖）
- `uHorizonClampSin = sin(1°)`（反射方向；**探针缺陷补偿，TODO: 探针补下半球后删**）

---

## 9. 与 V1 的关系

**实现方式（2026-11-25 定）**：不是一个 uniform 开关，而是**两份独立的 pass**——
`WaterSkinRenderer`（V1）与 `WaterSkinV2Renderer`（V2）各自持有 program / WBO，
由 `WaterWaves::v2` 在 `Renderer` 里二选一（面板「水皮 V2」+ CLI `--water-v2` / `--water-samples`）。
理由：V2 要**删掉** V1 的 9 个口径，在同一个着色函数里 `if` 切两套逻辑会让 V1 的注释与不变式
失效；两份各自独立才能让 V1 逐字节冻结、V2 自由迭代。

| `WaterWaves::v2` | 行为 |
|---|---|
| `false` | **V1 原路径，逐字节不变**（gold 继续兜底）；天球折射按 V1 口径截断谱 |
| `true` | V2（S = `v2_sample_count` ∈ [1,16]）；天球折射同步按不截断求法线 |

⚠️ `v2` 放在 `WaterWaves`（而不是 `WaterCommand`）是因为**天球折射必须与水皮同口径**：
放在谱配置里 ⇒ 两个消费者自动同源。V2 renderer 会 `CHECK(waves->v2)` 拦住"只换水皮不换天球"的误用。

- `S=1` 在 V2 下**不等于 V1**（V2 无截断、无 `normal_var_px`/`env_lod`），这是预期的；
- 因此 V2 首次验收**不做 gold**（Danis 2026-11-25 定：先交互式验收）。V1 的 gold 继续用
  `uWaterSkinV2=0` 保护。

---

## 10. 验收方案（交互式，本轮不做 gold）

1. ocean lab 打开「水皮 V2」，`S` 滑条 1…16；
2. 同机位 A/B：`V2=0`（V1）↔ `V2=1`；
3. 看三件事：
   - **纹理**：远处是否重新出现波动感（V1 的远场是平的）；
   - **摩尔纹起点**：从哪个距离/仰角开始出现结构化噪声（对照 §2.1 的表）；
   - **连续性**：地平线处 V2 自己的过渡是否连续（V2 内部无 `far`，应当天然连续）。
4. 记录 `S` 与"噪声起点距离"的关系（这张表是 V3 设计闭式 mip 的输入）。

---

## 11. 明确不做（V3 及以后）

| 项 | 为什么现在不做 |
|---|---|
| **谱截断 / 闭式 mip**（Nyquist 分工线） | 会把"V2 设计本身的效果"和"截断的效果"混在一起，无法归因 |
| **TAA / 时域抖动（黄金角）** | 静态图案先看确定性结果 |
| **glints（碎光，M3）** | 依赖采样层，但属独立特性 |
| 泡沫 / 白冠 | 与透射 alpha 的额外遮挡有关，独立特性 |
| 地球曲率 / 地平线过渡 | 需要先有纯 S_max 的基线才能定 |

> **V3 的入口条件**：拿到"`S` ↔ 噪声起点"曲线之后，再决定分工线（谱截断）画在哪、
> 以及地平线用什么机制替换 S_max 采样。

---

## 12. 涉及文件（实现时的清单）

**实际落地（2026-11-25）**：

| 文件 | 改动 |
|---|---|
| `src/water/water_skin_v2_shader.h` | **新增**：V2 的公共/水上/水下/调试片段 + 4 个 main（V1 的 9 个口径一个都不带） |
| `src/water/water_skin_v2_renderer.{h,cc}` | **新增**：整份 pass（4 program + WBO），`Inputs` 直接复用 V1 的（滑条共用），多一个 `sample_count` 参数 |
| `src/water/water_wave_shader.h` | `uWaveNoTruncate`（V2 = 1）；`WaveLodAmp()` 抽出唯一出处；`WaveNormalAtXZ()` 供按点求法线；`uWaveOriginXZ`（只给 flow map 还原世界系用） |
| `src/water/water_wave_uniforms.{h,cc}` | `WaveUniformParams` 增 `no_truncation` / `origin_x,z`；初相 `fmod 2π` 预算 |
| `src/water/water_wave_math.h` | `WavePhaseOriginM()`（1 km 量化，唯一出处） |
| `src/skydome/sky_renderer.cc` | 天球折射的谱截断开关与 `WaterWaves::v2` 同值 |
| `src/renderer.{h,cc}` | 持有两个水皮 renderer，按 `WaterWaves::v2` 二选一 |
| `interface/render_command.h` | `WaterWaves::{v2, v2_sample_count}` |
| `demo/skylight_viewer_app.h` | 面板（场景 4）：`水皮 V2（多法线采样）` + `水皮 S 采样数` |
| `demo/jpov_skylight_viewer.cc` | CLI：`--water-v2`、`--water-samples N` |
| `test/water/water_skin_v2_gpu_test.cc` | **新增** 4 条：平面等价（水上/水下，V2 == V1 逐像素）、有波浪时 V2≠V1 且 S 有影响、S 越大横向方差越小 |

**本轮没做（有意）**：`water_skin_math.h` 的 CPU 对照（V2 的抖动/σ²/env_lod 没有 CPU 参考，
所以也没有 gold 对拍）；谱截断 / TAA / glints / 地平线过渡（§11）。
=> 这意味着 V2 的**绝对**正确性目前由「平面时与 V1 逐像素相同」这条不变量兜着，
而"远场到底长什么样"要等交互式验收 + V3 的闭式 mip。

---

## 13. 决策记录（2026-11-25 Danis）

| # | 决定 |
|---|---|
| 1 | V2 = **纯 `S_max` 硬采**，**不做谱截断**（要"多远会摩尔纹"的直觉） |
| 2 | `S` 默认 **4**，滑条到 **16** |
| 3 | 抖动：**Hammersley 静态**，椭圆内均匀 |
| 4 | **MSS σ 保留**（观感影响大，需与 V1 对照） |
| 5 | **不做 gold**（先交互式验收） |
| 6 | 多根跳变**照抄 V1**，不治 |
| 7 | 相位精度（局部原点）可以做（纯数值，不改数学） |
| 8 | 地平线过渡**留给 V3** |
| 9 | 交付形态：**两份独立 pass** + `WaterWaves::v2` 二选一（不是 shader 里的 uniform 分支） |
| 10 | 采样椭圆用**半轴**（`footprint/2`、`footprint·|rd.y|/2`），见 §5.1 |
| 11 | 相位局部系：`uCamPosXZLocal`（CPU double 减）+ 初相 `fmod 2π` + flow map 加回 origin，见 §8.1 |
| 12 | 本轮不做 gold / 不做 CPU 对照（先交互式验收），正确性靠「平面 V2 == V1 逐像素」 |
