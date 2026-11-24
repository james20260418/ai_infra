# 远场海面像素颜色：现状、为什么不行、业界怎么做（2026-11-24）

> **一句话**：现在的像素着色是「**逐像素求交 + 单一法线**」，它的前提是「一个像素只盖住
> 一小块可分辨的海面」。远场这个前提不成立 —— 一个像素盖住成千上万个波面，
> 颜色是**斜率分布上的积分**，不是某个交点处的值。继续在求交里加护栏/淡出/带宽只能遮丑，
> 正解是**分层 LOD：几何 → 法线 → BRDF**（Bruneton et al. 2010）。
>
> 本文只做调查与方案，不改代码。分支 `feature/20261124-jpov-water-farfield-color`（base=main）。

---

## 1. 现状：远处一个像素的颜色是怎么算出来的

入口 `water_skin_shader.h::main()`（480 行左右，7 个阶段）：

| 步 | 做什么 | 关键实现 | 远场的问题 |
|---|---|---|---|
| ① | 视线 `rd`、半向量 `H`、太阳像的像素足迹 `footprint_a2` | `:157` `0.25·(\|dFdx H\|² + \|dFdy H\|²)` | 屏幕空间量：波被截断后它才是主导项 |
| ② | 平面交点 `t = (level − cam.y)/rd.y` | `:160` | 掠射时 `t` 可以到几公里（正常） |
| ③ | 像素足迹 `footprint = t·pa / max(\|rd.y\|, 0.15)` | `:177` | `0.15` 是人为地板（≈8.6°），见 §2.3 |
| ④ | **求交**：`WaveSurfaceHit(cam, rd, level, footprint, t, pos, N)` | `water_wave_math.h:507` / `water_wave_shader.h:387` | **本问题的核心**，见 §2 |
| ⑤ | 斜率方差 `var_p`（谱侧被截断的方差 + 短波 MSS） | `:204` `WaveSlopeTensor`；`water_wave_math.h:231` `WaveFilteredVariance` | 已经存在，但它只被当「粗糙度」用，不是积分 |
| ⑥ | 反射率 `F = RoughFresnelAlbedo(cos_i, var_p, ior)` | `:243`；`water_skin_shader.h:131` | 用**角度平均**近似替代积分（拟合式） |
| ⑦ | 太阳的像：`a2 = 盘角² + footprint_a2 + normal_var_px` → GGX | `:264` | `normal_var_px = 0.5·\|dFdx N\|²`（`:197`）—— 波一旦被截断，`N` 变平 ⇒ 这一项归零 ⇒ 远场高光退化成 δ |
| ⑧ | 天空反射：`reflect(rd,N)` 采样 cube，`env_lod` 由 `var_p + normal_var_px` 推 | `:330-335` | 各向同性模糊；上限还被钳在 1.5~3 级 |
| ⑨ | 透射率 `alpha = 1 − (1−F)/n²`，层色 `C = F·refl + sun_spec` | `:337-340` | 与介质（`MediumAt`）合成，介质按水平半空间 ⇒ 远场这部分**是对的** |

求交内部（`water_wave_math.h:507-650`，GLSL 同源 `:387`）：

```
amp_sum = Σ A_i · fade · lod_amp(footprint)          // 只算主波（8 条）
span    = amp_sum / max(|rd.y|, 1e-3)                // 区间半宽
[t−span, t+span] 上二分 12 次，找 f(t) = (cam.y+rd.y·t − level) − h(p.xz) 的变号点
```

**每次求交的三角函数次数**：12 次二分 × 每次 `WaveSampleAt`（8 条主波 sin/cos，含 warp/Stokes）
≈ **100+ 次 sin/cos**；收尾再算一次法线（8 主波 + 64 细节波）。

---

## 2. 为什么这套在远处不成立（四条，都可量化）

### 2.1 像素里没有「一个交点」

720p / fov 60° ⇒ 每像素约 **1.7×10⁻³ rad**。相机高 15.8 m 时，视线仰角 1° 的那一行：
`t ≈ 900 m`，`footprint = t·pa / |rd.y| ≈ 900×0.0017/0.017 ≈ **90 m**`。
仰角 0.3° 时 `footprint ≈ 1 km`。

而我们的谱里最长的波 ≈ 100~140 m（U=8~12 时的 1.6·λp）。也就是说：
**一行的像素足迹里装着 10⁰~10⁴ 个完整波**。可见表面在这块面积内是一组高度/斜率样本，
不存在「唯一可见交点」。求交返回的那一个 `t` 只是「像素中心那条射线先撞到的第一个波峰」，
它是一个**与像素平均值无关的随机样本**。

### 2.2 二分本身在远场失效（不是精度问题，是假设不成立）

- 区间半宽 `span = ΣA / |rd.y|`（还有个 `1e-3` 地板）。ΣA ≈ 1 m 时：
  - 仰角 1° ⇒ span ≈ 60 m ⇒ 区间内约 **2.5 个 24 m 波周期**；
  - 仰角 0.2° ⇒ span ≈ 290 m ⇒ 约 **12 个周期**；
  - 仰角 ≤ 0.06°（span 地板 1e-3 生效）⇒ span ≈ 1000 m ⇒ **几十个周期**。
- 二分 12 次的前提是**区间内只有一个变号点**。多个根时，迭代收敛到哪一个根完全由
  `f(t_lo)/f(t_hi)` 的符号和中点的采样值决定 —— 相邻像素可以跳到完全不同的波峰。
  这就是「远处波峰交界处单像素噪点 / 时间抖动」的来源（`known_issue` 里反复出现的"微抖"）。
- 12 次二分给出的 7 cm 精度是「单根假设下」的数字，多根时**没有意义**。

### 2.3 为了掩盖 2.1/2.2，历史上叠了三层补丁，每层都带副作用

| 补丁 | 位置 | 作用 | 副作用 |
|---|---|---|---|
| 掠射守卫 `max(\|rd.y\|, 0.15)` | `water_skin_shader.h:177` | 钳住"足迹→∞" | 足迹被系统性低估 3×+ ⇒ 亚像素波留在几何里 ⇒ 走样 |
| 距离淡出 `fade_start/end` | `water_wave_math.h` `WaveFadeFactor` | 把远处的波压平 | 固定距离上"波消失"；与物理无关；删掉就暴露上面的问题 |
| 谱截断窗口 `lod_px_min/max` | 同上 `WaveFilteredVariance` / `WaveAmplitudeSum` | 波长 < 3 px 不进几何 | 正确方向，但它是「**丢弃**」而非「**积分**」：被丢掉的能量只进了 `F_eff` 和模糊，没进高光方向 |

三者的关系就是现在讨论的「护栏 / fade / 截断」三条滑条 —— 它们在**同一件事上互相打架**，
因为它们都是 2.1 的临时替代品。

### 2.4 高光项的带宽是屏幕空间量，在远场恰好归零

`a2 = 盘角² + footprint_a2 + normal_var_px`。波被截断后 `N` 变成平面 ⇒ `dFdx N ≈ 0` ⇒
`normal_var_px → 0`；`footprint_a2` 只反映**视线**的变化，不反映海面本身。
结果：远场是「按粗糙面的反射率反射 + 一个 0.13° 的镜面容差」——
能量被 `F_eff` 从镜面项拿走了，却没有以宽眩光带的形式还回来。这正是
`known_issue §10.16` 里「暗 + 平」的机制（M1 只是给这个缺口打补丁）。

---

## 3. 业界怎么做

### 3.1 分层 LOD：geometry → normals → BRDF（Bruneton, Neyret, Holzschuch 2010）

> *Real-time Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF*,
> Computer Graphics Forum 29(2), 2010. doi:10.1111/j.1467-8659.2009.01618.x

这是与我们问题**一一对应**的论文（同一个作者组后来做了 Proland 的海洋模块）。要点：

1. **按波长逐条决定它去哪一层**。对每条波 train：
   - `λ > N_geom · L`（L = 网格单元尺寸）⇒ 进**几何**（位移 + 法线）；
   - `λ > N_norm · l`（l = 像素尺寸）⇒ 进**法线**（改变法线，不改位移）；
   - 剩下的 ⇒ 进 **BRDF**（统计）。
   权重用 smoothstep 在 `[N_min, N_max]` 之间过渡，Nyquist 要求 `N ≥ 2`（实测用 2~4）。
2. **BRDF 层的输入是斜率分布**。trochoid 之间独立 ⇒ 中心极限定理 ⇒ 海面斜率是
   **各向异性高斯**，其方差是各波方差之和：
   `σ² = Σ_i [k_i,x², k_i,y²]ᵀ / ‖k_i‖² · (1 − √(1 − ‖k_i‖² w_r² h_i²))`
   —— 注意这跟我们的 `WaveFilteredVariance` 的 `½(kA)²(1−w²)` **是同一个式子**
   （小陡度展开下 `1−√(1−u) ≈ u/2`）。我们已经有正确的方差，缺的是「把它当分布用」。
3. **BRDF 用解析的各向异性高斯坡度模型**（Ross, Dion, Potvin 2005）：
   可见性概率 `q_vn(ζ, v, l) = p(ζ)·max(v·f,0)·H(l·f) / ((1+Λ(a_v)+Λ(a_l))·f_z·cosθ_v)`，
   `p(ζ)` 是各向异性高斯，`Λ` 是 Smith 遮蔽项。**这是解析积分过的**，不是蒙特卡洛，也不用 GGX 近似。
4. **天空反射 = 环境图按椭圆核预滤波**：椭圆的轴 = 高斯坡度分布的两轴
   （在他们实现里用立体投影 + 椭圆滤波，可以在 GPU 上直接按 LOD 采样）。
   平均菲涅尔用拟合式（比逐方向积分便宜，误差可接受）。
5. **折射/水体自发光**：`T = 1 − R`，且水下辐射度按**漫射**处理（多次散射）。
6. **性能技巧（很关键）**：方差按「**总方差 − 已解析部分的方差**」求，于是着色器成本
   正比于**被解析的波数**，远场最少。我们已经在做同一件事（`WaveFilteredVariance` 累加
   `1 − geom²`），可以沿用。

**对我们最直接的结论**：远场不需要「找交点」。平面交点 + 高斯坡度 BRDF + 椭圆预滤波环境图，
就足够给出正确的能量与观感。

### 3.2 几何 LOD + 着色器里保留原始高度场（Tessendorf / Gilligan）

> J. Tessendorf, *A Note on Normal Mapping Displaced-Height Ocean Surfaces*（Gilligan 渲染器）

- 现象：几何 LOD 之后，三角面比波长还大 ⇒ 面法线丢掉小尺度细节 ⇒ 高光偏置 + 走样。
- 做法：着色器**保留原始高度场**，在交点处按世界坐标重新求法线（代价 +1%~11% 渲染时间）。
- 对我们的意义：这是「几何可以粗，但法线必须按位置查」的折中；我们**没有网格**，
  所以这条等价于「法线层用解析梯度，几何层可以用平面」。正好支持 §3.1 的分层。

### 3.3 多分辨率级联（Crest / Dupuy & Bruneton 2012）

- **Crest**（Unity，SIGGRAPH 2017/2019 课程）：以相机为中心的 4~6 级 LOD 位移纹理级联，
  级间平滑混合；材质里有一个直接对应我们问题的参数：
  *"Vary Smoothness Over Distance — helps to spread out specular highlight in
  mid-to-background. From a theory point of view, models transfer of normal detail
  to microfacets in BRDF."* 也就是：**远处把法线细节转成粗糙度**（我们的 M1 的工业版）。
- **Dupuy & Bruneton 2012**（whitecaps）：投影网格 + 多尺度 choppy 波；白沫覆盖率
  用**可线性预滤波**的判据 → 直接交给 mipmap 过滤，因此从厘米到行星尺度都不走样。
  这条给我们的启发是：**凡是"亚像素就消失"的量，都要能线性预滤波**（方差/覆盖率），
  而不该走「截断开关」。

### 3.4 商业引擎的实用主义路线（Unreal）

- UE 的 Water 系统：近处是四叉树瓦片 + 位移网格；远处用 **Far Distance Mesh** ——
  **一块低顶点数的平面 + 一个简化材质**，只为把海平面补到地平线，并与主材质**对色**
  （`Water_FarMesh`）。
- 已知问题：一旦给主水体挂 Wave Asset（带真实波幅），远场平面与近场波面会**错位/接缝**
  （Epic 论坛有复现，官方承认无解）。
- 对我们的意义：UE 这条是「**承认远场不做物理，只对色**」。我们能做得更好，因为我们有
  解析谱（可以真的算方差），但**它印证了"远场是另一套着色"**这个结论。

### 3.5 参考速查表

| 方案 | 远场用什么 | 能量正确吗 | 与我们现状的距离 |
|---|---|---|---|
| Bruneton 2010 | 高斯坡度 BRDF + 椭圆预滤波环境图 | ✅（解析积分） | **推荐目标** |
| Tessendorf/Gilligan | 粗几何 + 着色器查高度场法线 | 部分（法线细节保住） | 抄它的"法线层独立" |
| Crest | LOD 级联 + 远处转粗糙度 | 近似 | 我们有 M1，但缺各向异性 |
| Dupuy & Bruneton 2012 | 多尺度 + 线性可预滤波量 | ✅（白沫/方差） | 抄它的"预滤波"原则 |
| Unreal Water | 远场平面 + 对色材质 | ❌（只求观感） | 下限，别学 |

---

## 4. 建议方案（分三步，每步都能单独交付/验收）

### Stage A：先把「远场」显式化（低风险，只有收益）

1. 定义开关量：`far = smoothstep(N_geom, N_geom·2, λ_max / footprint)`
   （λ_max = 谱里最长的主波；`far = 1` ⇒ 一个完整波都装不进一个像素）。
2. `far > 0` 时**不再调用 `WaveSurfaceHit`**：`t = t_plane`，`N = (0,1,0)`（可留一个
   极低频倾斜）。这直接省掉远场每像素 100+ 次 sin/cos 与 12 次二分环。
3. 保留现有着色（`F_eff` + 现有 `a2`），只在 `far` 的过渡带里做混合
   ⇒ **画面逐像素不变**，只是变快，并且把「无意义的根」从管线里拿掉。
4. 顺带把 `span` 的 `1e-3` 地板改成「区间必含变号点」的显式判据（见 §2.2），
   避免多根区间里二分静默返回随机根。

**验收**：`far` 开关在 A/B 下 ±10% 范围内逐像素差 < 2/255（无缝）；远场像素成本降到 <1/5；
CPU 参考与 GPU 逐像素对拍仍绿（把 `far` 也做成 CPU 可算的量）。

### Stage B：统计 BRDF（把「分布」真的用起来）

1. **各向异性坡度方差**：把 `WaveFilteredVariance` 的 2×2 张量做特征分解
   （主轴 ≈ 风向；特征值 σ_along² / σ_across²），短波 MSS 按 Cox–Munk 的 2.7:1 补进对应轴。
   现在只有「投影到入射面的单值 `var_p`」——那是各向同性假设。
2. **太阳项**：换成 Ross/Bruneton 的**高斯坡度 BRDF**（解析式，含 Smith Λ），
   而不是 `GGX(a2)` 的近似。峰值/宽度都由 `σ` 决定 ⇒ 远场眩光带自然变宽、能量守恒。
3. **天空反射**：环境图按**椭圆核**采样（长轴 = σ_along，短轴 = σ_across）；
   现在的 `textureLod` 是各向同性且被钳在 1.5~3 级，正好是「眩光带是圆盘而不是沿风拉长」的原因。
4. **反射率**：`R = ∫ F(ζ)·p(ζ) dζ`（解析近似已有 `RoughFresnelAlbedo`，换成各向异性版），
   `T = 1 − R`；水体自发光沿用现有 `MediumAt`（远场它本来就对）。
5. **删除/降级**（这是 Stage B 的真正价值）：
   - 掠射守卫 `max(|rd.y|, 0.15)` → 回到 `1e-4`（除零护栏），因为它不再是"遮丑工具"；
   - `fade_start/end` → 删除（远处变平由 BRDF 接管）；
   - `lod_px_min/max` → 收敛成常量（Nyquist 3~4 px），不再暴露滑条；
   - `normal_var_px` 与 M1/M2 的补丁 → 由统计 BRDF 自然包含，退化成 A/B 开关或直接删。

**验收**：
- 远场眩光带半角 **0.13° → 6~7.5°**（对 Cox–Munk），且**沿风拉长**（长短轴比 ≈ 2.7）；
- 能量：白天空 furnace 检查 `∫L dω ≈ F_eff·E`（±3%）；
- 时间稳定：远场逐像素时间差分不高于 Stage A 的数值；
- 无缝：`far` 过渡带 A/B < 2/255。

### Stage C（可选，之后再说）

- 地平线/地球曲率（相机高 < 100 m 时可忽略；高机位才需要）；
- 海雾/aerial perspective（与现有 froxel 雾共享）；
- 若将来要「碎光 sparkle」：给谱补毛细波段（λ ~ mm-cm，只进 MSS/glint），
  再考虑 FFT 级联（Dupuy/Tessendorf），**不是靠抖法线**（已知教训，`known_issue §10.16` M3）。

---

## 5. 开放问题 / 需要拍板

1. **σ 的覆盖范围**：统计 BRDF 想要正确的眩光带，σ 必须一直积到毛细波尺度。
   我们只有 8 主波 + 64 细节 + Cox–Munk 标量尾巴 —— 是否接受「Cox–Munk 补尾巴、
   但各向异性比例硬编码 2.7:1」？
2. **环境探针分辨率**：椭圆滤波需要比 128² cube 更高的有效分辨率，或者改成
   「解析天空模型上按椭圆核积分」。后者更准但要把 Preetham 搬进水面着色器。
3. **M1/M2 的去留**：它们在统计 BRDF 里是自然包含的。过渡期建议保留 CLI，
   Stage B 落地后删除（否则又是两套并行口径）。
4. **是否要「无缝」的严格定义**：我建议用「切换带内 A/B 逐像素 < 2/255」作为可测判据，
   而不是"看起来没有缝"。
5. **性能预算**：Stage A 是纯收益；Stage B 的 BRDF 是解析式，但椭圆滤波 + 各向异性
   Smith 会比现在贵。需要先给一个「远场每像素上限」（例如 ≤ 现在的一半）再动手。

---

## 6. 参考文献

1. E. Bruneton, F. Neyret, N. Holzschuch. *Real-time Realistic Ocean Lighting using Seamless
   Transitions from Geometry to BRDF.* CGF 29(2), 2010. doi:10.1111/j.1467-8659.2009.01618.x
   （实现：Proland `ocean` 模块，含 FFT 版本）
2. E. Bruneton 的 Eurographics 2010 讲义（层级 LOD 的权重定义、`w_p/w_n/w_r`、Nyquist ≥ 2）
3. E. Bruneton 的 BRDF 来源：Ross, Dion, Potvin 2005（各向异性高斯坡度 + Smith 遮蔽的解析 BRDF）
4. J. Tessendorf. *A Note on Normal Mapping Displaced-Height Ocean Surfaces*（Gilligan）
   —— LOD 网格丢法线细节的量化与"着色器查原始高度场"的修法
5. J. Dupuy, E. Bruneton. *Real-time animation and rendering of ocean whitecaps.*
   SIGGRAPH Asia 2012 Tech Briefs. doi:10.1145/2407746.2407761 —— 可线性预滤波的量
6. Crest Ocean System（Unity）：SIGGRAPH 2017/2019 课程 + 文档
   （*Vary Smoothness Over Distance* = 法线细节转微表面；多分辨率 LOD 级联）
7. Epic Games, *Water Meshing System and Surface Rendering*（UE4.27/UE5 文档）
   —— Far Distance Mesh + `Water_FarMesh` 对色材质；与 Wave Asset 的接缝是已知未解问题
8. Cox & Munk 1954 —— 海面斜率分布与沿风/横风的各向异性（我们短波 MSS 的出处）
