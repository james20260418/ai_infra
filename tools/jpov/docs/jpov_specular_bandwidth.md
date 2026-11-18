# 镜面高光的带宽（δ 光源 × δ BRDF）

> **状态：已修复（2026-11-18，PR「specular bandwidth」）。**
> 本文是当初用来定方案的设计记录 + 现在的落地口径，**从 `known_issue.md` §11 迁出**
> （问题已解决，不再是 known issue）。以后改带宽相关的东西先读这一份。
>
> 一句话：完美镜面 + 平行光/点光源 = 两个 δ 相乘 ⇒ 逐像素采样只能拿到 ∞ 或 0。
> 解法是给光源一个**角半径 θ**（太阳 0.004654 rad、月亮 0.004521、点光源逐像素 `asin(R/dist)`），
> 并进 GGX 的 `a2 += (θ/2)²`；再修掉 `distributionGGX` 的 `max(denom,1e-7)` 护栏
> （它会把亚像素带宽的峰值钳暗，阈值 `a2 < 1.8e-4` = `roughness < 0.115`）。
>
> 验收工件（都是「机位算死、亮斑落在画面正中」的可读图）：
> - 平行光：`jpov_skylight_viewer --capture-mirror-specular`；gold `test/object3d:jpov_specular_bandwidth_gold_test`
> - 点光源：`jpov_skylight_viewer --capture-point-specular`；gold `test/object3d:jpov_point_specular_gold_test`
> - 水皮：`--capture-water-sun`
>
> **没有"关闭带宽"的开关**：`DirectionalLight::angular_radius` 填 0 会被 renderer CHECK 打回；
> `PointLight::physical_radius` 填 0 由 CPU 侧钳到 `kMinPointLightAngularRadiusRad`（≈1 屏幕像素）。
>
> 仍未做：点光的**可见性 v**（盘被挡一部分时的能量）、像素足迹（`dFdx(H)` 斜率方差）
> 并入 PBR 的 `a2`、三份 `distributionGGX` 收敛成一份共享 GLSL。

## 1. 为什么：缺的是「带宽」，不是「项」

> 触发（2026-11-17）：查水面为什么不能做平行光高光。结论是**所有**「无限小光源 + 无限光滑表面」
> 都会撞上，与模型无关，只与 `α_eff` 有关。水只是最极端的例子（法线恒定、无 roughness）。

### 1.1 判据：缺的是「带宽」，不是「项」

完美镜面 + 平行光 = 两个 δ 相乘：

```
L_o = [E⊥/Ω_s · δ(ω−ω_s)]  ∗  [F·D(α)/(4 cosθi cosθo)]      α→0
```

两个 δ 卷积仍是 δ ⇒ **点值是无穷大，只有积分（一个像素覆盖的立体角）有限**。
渲染是点采样 ⇒ 拿到 ∞ 或 0，像素中心一动就翻转 ⇒ 闪烁 / 噪点，而不是「没有高光」。

因此 `total_specular` 这项**一直都在**（PR #60 起未改）；不可用的是它的**取值**。

带宽有四个来源，任意一个非零即可采样：

```
α_eff² = roughness² + θ_light² + σ_normal² + σ_pixel²
```

### 1.2 JPOV 当前状态

| 表面 | 各带宽项 | 结果 |
|---|---|---|
| 镜面地面（`metallic=1, roughness=0`） | `roughness`=0 **+ θ**（PR #262 起三份 PBR 片元全部接上 `angular_radius`） | **能算**：日盘像饱和区 16×50 px（掠射拉长），见 `--capture-mirror-specular` |
| 镜面地面（`metallic=1, r=0.02`） | `roughness`=0.02 本身够 + θ | **能算**（那条竖光就是它；月仰角低时纵向拉伸 `1/sin`，见 2026-11-01） |
| 水面（契约 §2.3：纯平面、无 roughness） | 四项全 0（只剩日盘角半径 4.65e-3） | **不可采样** ⇒ 契约写「不做」 |
| 水面 + 波浪 | `σ_normal` ≠ 0 | **能做**（同一项，不是新机制） |
| 点光源 | `θ_pixel = asin(R/dist)`（2026-11-18 起，R 由 CPU 钳到 `kMinPointLightAngularRadiusRad`） | **能算**：灯的圆盘像随 R 变大（`--capture-point-specular`） |
| 点光源（`physical_radius` 填 0） | 不是 δ：CPU 钳到 **0.0015 rad**（≈1 屏幕像素） | **能算**（兜底带宽；想要真实灯盘像就给真实半径） |
| 水体介质（内散射） | 不涉及 | `∮p·L dω = p·E`，太阳立体角自动消去（§5.1） |

### 1.3 业界解法（四类，按“谁提供带宽”分）

**① 钳制型号 / 最小 roughness —— 最省，但是妥协**
- Amanatides 1992 最早提出：检测会走样的像素，**钳 Phong 指数**并乘 `max(n)/max(n')` 保住总能量
  （[hilite.pdf](http://www.cs.yorku.ca/~amana/research/hilite.pdf)）。
- Filament：`α = perceptualRoughness²`，并把 perceptualRoughness **clamp 到 0.089**（fp16 的
  `1/r⁴ ≥ 6.1e-5` 下限）；脚注明确写 **Frostbite 把解析光的 roughness 钳到 0.045 专为压高光闪烁**
  （[Filament §4.8](https://google.github.io/filament/main/filament.html)）。
- 反方意见（Nathan Reed）：钳到 0.04 太大，**只要不为 0**，取 1e-3~1e-4 即可；完美镜面应交给
  面光源而不是钳 roughness（[CGSE](https://computergraphics.stackexchange.com/questions/9123/)）。

**② 给光源角尺寸（解析带宽）—— 物理正确，但不自适应**
- 太阳不是点：角半径 0.2665°（直径 0.53°）⇒ `θ_sun² ≈ 2.2e-5`，是 `α_eff²` 的**物理下限**。
- 球光源用「代表点」近似（Karis 2013 / UE4；Lagarde & de Rousiers 2014 Frostbite）。
- 水面/海洋的经典口径与此一致：Cox–Munk 1954 把海面当斜率服从高斯的面元集合，glint 来自
  「倾斜到恰好把太阳反射进视线的那些面元」；太阳角尺寸与波面斜率方差在**方差域相加**
  （[Cox–Munk 参数化](https://coxmunk.readthedocs.io/en/latest)、
  [Ocean Optics Web Book](https://oceanopticsbook.info/view/surfaces/cox-munk-sea-surface-slope-statistics)）。

**③ 几何 SAA（per-pixel 导数 → 抬 α）—— 通用的那条**
- Toksvig 2005 / Hill 2012 / Vlachos 2015：用 `dFdx/dFdy` 估相邻像素半向量差，钳 roughness。
- **Kaplanyan et al. 2016**（HPG, *Filtering Distributions of Normals for Shading Antialiasing*）：
  把像素足迹投影到**斜率域**，直接对 NDF 做卷积。
- **Tokuyoshi & Kaplanyan 2019**（I3D, *Improved Geometric Specular Antialiasing*）：改在
  **投影半向量空间**求导，修正掠射/边缘处的导数误差；已被 Unity 2018.2+、Lumberyard 采用。
  2021 年 JCGT 又出了 projected-space NDF filtering 版本。

**④ 法线贴图滤波（把「看不见的高频」烘进 MIP）**
- LEAN mapping（Olano & Baker 2010）：存斜率一阶/二阶矩，用标准 MIP 线性滤波。
- GGAA（Chermain et al. 2021）：针对 glinty BRDF 的几何高光抗锯齿。

### 1.4 我们该取哪条（判据：是否自适应 / 是否守恒 / 是否损害锐度）

1. **不做通用 roughness 下限**：它同时把反射锐度换成稳定度，而镜面地板/水面最想要的正是锐度。
   只在**数值兜底**意义上保留（1e-3 量级，且只防 `1/α²` 溢出，不指望它抗闪）。
2. **`DirectionalLight` 加 `angular_radius`**（太阳/月亮 0.00465 rad，可覆盖）：物理下限，
   同时是 `known_issue.md` §10.9「日盘必须是独立光源项」的能量出处（`α² = roughness² + θ²`）。代价小，先做。
3. **水面用波浪的斜率方差做带宽**：`α² = σ_slope² + θ_sun²`；可另给一个**最小斜率方差**
   （≈0.02，语义上是「承认有不可解析的高频波」），而不是最小 roughness。
4. **几何 SAA 暂不做**：等真有法线贴图 / 高频几何再上（那时才有 `σ_normal` 可估）；
   现在没有法线贴图的资产，上了也测不出效果。

### 1.5 判据：什么闪烁该修、什么不该修

- **该修**：闪烁来自「亚像素 δ」——带宽为 0 导致命中/不命中翻转。
- **不该修**：闪烁来自**真实高频**（波面斜率、glint）—— 那是物理现象；1 spp 下它天然抖，
  业界靠 TAA/时间累积解决。**JPOV 每帧从零、要进 gold ⇒ 不可能靠累积**，只能用带宽把它压到
  可接受（这也是为什么 §1.4 第 3 条要保一个最小斜率方差）。

### 1.6 已定方案（2026-11-17 Danis 拍定；2026-11-18 落地）

**① 角度归光源，强度归光照曲线，可见性归几何 —— 三件事不能互相代替**

| 量 | 归属 | 定稿 |
|---|---|---|
| 角半径 `θ` | **光源的几何属性**（常数/逐像素） | 太阳 **0.00465 rad**（0.2665°）、月亮 0.00452；`CHECK_GT(angular_radius, 0)` |
| 强度/色温 | 光照曲线 | 已有 `SunDirectionalIntensity()`（DNI PWL）+ `SunDirectionalColor()`，不动 |
| 可见分数 `v` | **阴影/几何** | PCSS/软阴影给的盘可见面积比 ∈ [0,1]，乘 specular（+diffuse） |

⚠️ **不从 `SkyCommand` 推导/插值 θ**：太阳角径与仰角**无关**（正午、日落一样大；年周期
  日地距离只变 ±1.7%，忽略）。用仰角插值 θ 会把“日落变暗”错实现在“盘子变小”上，
  penumbra 半径与日盘像尺寸一起错。
⚠️ **`θ→0` 不等于全暗，等于不稳定**：能量仍是 `F·E⊥`，只是集中到亚像素 ⇒ 爆/闪。
  全暗只有 `v→0`（盘被遮没）或 DNI→0（强度曲线）两条路。
⚠️ DNI 那条 PWL 本身是**艺术简化**（真实太阳贴地时还亮、只是红且被大气衰减）—— 写在这里，
  以后不要当成物理。

**② 带宽合并口径（必须写对，差一半）**

半向量是光与视线的**角平分线** ⇒ 光偏 `θ` 时 `h` 只偏 **`θ/2`**。JPOV 的 `distributionGGX`
用 `a = roughness²`、`a2 = a²`（= α²），所以直接：

```
a2_eff = a2 + (θ/2)²        // 写 θ² 会过宽 4 倍；写 α+θ 量纲就错
```

反射方向仍张开 `θ`（`r` 相对 `h` 是 2 倍）⇒ 镜面上日盘像大小 = 0.53°，自洽。
⚠️ GGX 的斜率方差是**无穷**（重尾）⇒ “方差相加”是工程近似（Tokuyoshi 2016/2019 就在修这个
  估计误差）；掠射下别指望它完全准。

**③ 关键数量级（改正过一次的错误认识）**

- 720p / 60° 垂直 FOV ⇒ **0.083°/px**；太阳圆盘 0.53° ⇒ **≈ 6.4 px（横向 7.4）** ⇒ **可分辨、稳定**。
- 真正亚像素的是 `r=0.02` 自己的 lobe：α=4e-4 rad ≈ 0.023° ≈ **0.28 px**。
- ⇒ 结论：**亚像素的是“材质太光”，不是“太阳太小”**。加入 θ 后从 0.28 px 变 6 px ⇒
  同时解决「值有限 / 能量对 / 不闪」三件事（§1.5 里“水面无波时的日盘”那条按此修正）。
- `r=0.02`（镜面地板）：`α_eff` 4e-4 → **2.36e-3**（5.9×），峰值 **~35×↓**，面积 ×35 ⇒ 通量守恒。
  **不会爆亮**：GGX 的 D 归一化 ⇒ 带宽只能把同一份能量摊开，峰值单调下降。爆亮只发生在 δ 求值。

**④ `roughness = 0` 的真实行为（PR #262 之前 / 之后，别记错）**

- **之前**不是爆亮，是**静默返回 0**：`a = r² = 0` ⇒ `a2 = 0` ⇒ `D = a2/max(denom,1e-7) = 0`
  ⇒ 平行光在完美镜面上**没有任何高光**（这也是网上“roughness=0 就没高光”的同一口径）。
  实测：18×18 m 镜面地面、太阳直射、FOV 8°，地面**一个比底色亮的像素都没有**。
- **之后**（PR #262，`a2_eff = (θ/2)² = 5.4e-6`）：`D_peak ≈ 5.9e4`，有限值，日盘像
  饱和区 16×50 px（60° FOV、掠射拉长）⇒ **奇点没了**。能量仍是 `F·E⊥`：带宽只把同一份能量摊开。

**⑤ 点光源：`physical_radius` 两个作用都用上了**

- ① Karis 2013 的**代表点**（`accumulateOneLight`）：只用球半径把 `spec_dir` 换到
  “反射方向离球最近的点” —— **只改方向，不改 lobe 宽度**。它解决的是“金属面在点光下全黑”。
- ② **带宽（2026-11-18 落地）**：逐像素角半径 `θ = asin(R/dist)`，同一条公式：
  ```
  θ_pixel = asin(clamp(R / max(dist, R), 0, 1))   // dist < R 时钳到 π/2（片元在球内）
  a2_eff  = a2 + min((θ_pixel/2)², 0.25)          // 只钳**带宽项**（a2 = roughness⁴ 本身可到 1）
  ```
  ⚠️ `R = 0` **不是** δ：CPU 侧（`src/lights_texture.cc`）统一钳到
  `kMinPointLightAngularRadiusRad = 0.0015 rad`（≈1 屏幕像素；小于 1 px 又会掉回闪烁区间），
  于是光滑面上永远拿得到可采样的高光。**没有"把带宽关掉"的开关** —— 这是 Danis 2026-11-18 定的口径
  （`DirectionalLight::angular_radius = 0` 则直接被 renderer CHECK 打回）。
  同时更正了 `PointLight::physical_radius` 的注释：之前那句“面积大⇒高光柔化”只对了一半
  （代表点只改方向，柔化来自 (θ/2)² 这一项）。
- ⚠️ 太阳的 θ 是**常数**（极远），球光的 θ **逐像素**；近场（灯贴着物体）代表点近似会失效，
  那时需要解析球光 / LTC。这也是“球光在引擎里常被做成单独光源类型”的原因。
- ⚠️ `PointLight::linear_radius` 是**衰减半径**（影响范围），不是发光体尺寸，不能拿它当 θ。
- ⚠️ **蒙皮（skeleton）路径没有点光循环**：蓝人只吃太阳/ambient/env。这是另一个缺口，
  与带宽无关（不是这次的事）。

**⑥ 落地清单**

- [x] `DirectionalLight` 加 `angular_radius`（默认 0.00465）+ `CHECK_GT(0)`。
- [x] `distributionGGX` 改成吃 `a2_eff`（或传入 `angular_radius` 内部合并）；**两条调用点**
      （太阳 / 点光）都要走同一条，避免只修一半。
      ⚠️ 顺带一个已实测到的坑：现有 `distributionGGX` 的护栏是 `max(denom, 1e-7)`，
      而 `denom ≈ π·a2²`（NoH→1）⇒ **a2 < 1.8e-4 时峰值被人为钳住** ⇒ 换成
      `a2 = roughness⁴` 就是 **roughness < 0.115**（不是 0.02！早先这里把 a2 与 α 混了），
      越光滑反而越暗、且能量不守恒（水皮的 `a2=(θ/2)²=5.4e-6` 实测被压低 **1086 倍**）。
      **PR #262 已把三份 PBR 片元（object3d / instanced / skeleton）的护栏改成 1e-30**，
      并把太阳路径改成 `a2_eff = roughness⁴ + (θ/2)²`、点光路径改成 `roughness⁴ + min((θ/2)², 0.25)`。
- [x] 水皮侧已落地并出图验收（`--capture-water-sun`：205/214/225° 三张，日盘像随太阳方位平移）。
- [x] 镜面地板已出图（`--capture-mirror-specular`：`mirror_r0.000` / `r0.020` / `r0.300`，
      都是正常镜子观感）。刻意**不**出"关掉天光只剩高光"的诊断照：那种图地面几乎全黑，
      检查时容易被误读成渲染坏了（Danis 2026-11-18 定）。
- [x] 重定受影响 gold：`test/object3d:jpov_specular_bandwidth_gold_*` 改成
      skylight viewer 的镜面地板场景（可读：天光 + 大镜面 + 散布方块），
      每张只差地面 `roughness`；断言量的是「饱和（≥250）的像」，天然把"有像/没像"分开
      （修之前这两种场景的窗口 max 分别只有 231 / 216）。
- [ ] 可见性 `v`（盘被挡一部分时的能量）仍未做：它需要软阴影/PCSS 提供盘可见面积比，
      与带宽正交，单独一条。
- [x] 点光 `θ_pixel` 已落地（2026-11-18）：object3d + instanced 两份片元的
      `accumulateOneLight`（skeleton 没有点光循环），`a2_eff = roughness⁴ + min((θ/2)², 0.25)`，
      `θ = asin(R/max(dist,R))`。验收：`--capture-point-specular`（灯的像落在画面正中，
      扫 R=0.02/0.10/0.50）+ `test/object3d:jpov_point_specular_gold_*`。
      实测（正常镜子观感、饱和阈值 250）80 / 512 / 3785 px；修之前三档 0 / 0 / ~0（max 216）。
- [ ] 像素足迹（`dFdx(H)` 的斜率方差）仍未并入 PBR 的 `a2`：掠射处的带宽仍会掉到
      亚像素（gold 里那条日盘像边缘偏硬就是它）。并入时注意 `dFdx` 不能放在
      `if (NdotL > 0)` 这类非一致控制流里（水皮是挪到 discard 之前求值的）。

**⑦ 对已有结论的影响（已处置）**

- ✅ **`known_issue.md` §10.9 / 契约 §2.3 的「平面水面下日盘是 δ 函数 ⇒ 不做平行光高光」已翻案并落地**：有 `θ` 后，
  静水也会出一个 **6 px 的日盘像**。它**属于水皮这一层**（进 WBO 的 `C`），且是**加法**、
  **不吃 SSR 的 `A`**（SSR/探针都不提供日盘）。
- 「波浪」与「θ」不是一回事：`θ` 给**静水单斑**，`σ_slope` 给**有风的 glitter 路**（多个小亮斑）。
  两者互补，不能拿一个当另一个。
