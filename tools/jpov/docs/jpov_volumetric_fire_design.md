# JPOV 体积火（逐像素 raymarch）接入 WBOIT —— 「薄层写入」方案设计

> 日期：2026-10-15 ｜ 状态：**设计（待实现 v1）** ｜ 工作区：`/james_pm/ai_infra_4`
> 作者：James / Danis。
> 关联文档：`jpov_particle_fire_design.md`（广告牌粒子火，MVP 已实现 `#180`）；
> `jpov_transparent_glass_design.md`（WBO 玻璃 FBO + 代表深度 `z_g`）；
> `jpov_froxel_design.md`（froxel 体积雾 + `z` 区间 O(1) 反解）；
> `/james_pm/fire_hero/DESIGN.md`（Mantaflow 体积资产 + 逐像素 raymarch 原型）。

---

## 0. TL;DR

**在既有 WBOIT 架构下，让「逐像素 raymarch 体积火」也往透明纹理里写一个「薄层」**：

- raymarch 沿视线积分出 `(L, α)`（预乘发射 + 遮挡），**压成一个带代表深度 `z_repr` 的透明片段**，
  写进玻璃/WBO 纹理（`accum / reveal / depth`）——和玻璃、粒子走**同一套顺序无关合成**。
- **吸收项 `τ = ∫σ dt` 精确**；**发射项 `c = L`**；**深度 `z_repr` 是近似**（取累计 α 阈值穿越处，或期望深度）。
- **v1 只写 1 个薄层**：对「体积深度范围内没有别的透明物穿插」的场景即正确。
- **Potential 升级**（本设计预留）：**多薄层切片** → **MBOIT**（用矩重建透射率，天然处理穿插、并给出真实 `T(z)` 分布）。

---

## 1. 背景：为什么要从「广告牌火」走向「体积火」

前期（`#180` / `jpov_particle_fire_design.md`）走的是**广告牌粒子 + flipbook**——这是 UE/Unity
的业界主流，便宜、可控。但在本面上撞到天花板：

1. **平面卡的通病**：单张卡俯视「躺下」、竖卡俯视变「一条扁线」；交叉卡（crossed quads）只在
   「量」上改善（更亮更宽），治不了**方卡直边 / 平面感 / 交叠接缝**。
2. **素材分辨率**：手里的 CC0 flipbook 只有 64px（业界 2K~8K）→ 糊。
3. **火苗卡之外全缺**：热畸变、点光、bloom、烟层——单靠火苗卡永远像贴纸。
4. **本质**：**火是一个"体"**（介质），而广告牌是"面"。用面去逼近体，误差是结构性的。

→ 主角/近景火需要**真正的体积**：即 `fire_hero/DESIGN.md` 的 **逐像素 raymarch**
（Mantaflow 烘 3D 密度/温度场 → 运行时沿视线积分）。

---

## 2. 物理模型与「三条离散化轴」

体积渲染 = 参与介质（participating medium），统一由**辐射传输方程（RTE）**描述：

```
C = ∫ T(t) · σ(t) · c(t) dt          T(t) = exp(−∫₀ᵗ σ ds)
      └─ 注入(发射/颜色)  └ 吸收(消光)
```

**同一个积分，只有三处不同的"离散化轴"**——理解它们是本设计的基石：

| 轴 | 代表技术 | 输入单元 | 难在哪 | 近似来源 |
|---|---|---|---|---|
| **沿光线离散** | **Raymarch** | **连续体场（3D 纹理）** | 数值**求积** | 步长 + 抖动 |
| **沿深度分布离散** | **WBO / MBOIT** | **片元**（面/粒子/sprite） | **顺序** | 经验权重 / 矩 |
| **沿空间离散** | **Froxel** | 视锥网格 | **分辨率**（糊） | 网格分辨率 |

**关键结论**：
- **Raymarch 的"顺序"是免费的**（沿 `t` 从近到远走，天然有序）；它只近似**求积**。
- **WBO/MBOIT 的"顺序"要推**（片元乱序到达）；它们近似的是**顺序**。
- **Froxel** 是"把体烘进低分辨率视锥网格"，xy 分辨率低 → 糊（`fire_fog` 的已知天花板）。

---

## 3. 关键认知：raymarch 出口是「层」，WBO/MBOIT 输入是「片」

这是接口设计的根本：

- **Raymarch**：在体的内部就把顺序解掉了，出口 = **一个 `(L, α)` 的"层"**（预乘发射 + 覆盖），
  **形态上就像一个普通透明片段**，可直接 `over` 叠到场景上。
- **WBO/MBOIT**：吃的是**零厚度的 `(色, 吸收)` 片段**（一个 `z`、一个 `α`），
  用权重/矩把它们排成一个正确合成。

> **接口错位**：raymarch 出口是**已衰减的发射 `L`**；WBO/MBOIT 输入是**未衰减的色 `c` + 吸收 `τ`**。
> **不能把 `(L,α)` 直接当 WBO 片段塞**（会双重计吸收）——必须做一次「层 → 片」的转换（见 §4.2）。

物理量本身**一致**（都是 RTE 的发射项 + 吸收项）；差别只在**"存的是哪一步"**。

---

## 4. First version：raymarch → 一个薄层写进 WBOIT

### 4.1 目标数据流

```
opaque 3D  ──►  玻璃 WBO pass  ──►  [raymarch 体积火：写 1 个薄层]  ──►  WBO resolve  ──►  froxel/后处理
                 (accum/reveal/depth)      (accum/reveal/depth)
```

体积火**不再单独 `over`**，而是作为**一个透明片段**汇入同一张 WBO 纹理 → **顺序无关**地
和玻璃、粒子一起合成。

### 4.2 薄层该填什么（`(z_repr, c, τ)`）

| 项 | 取值 | 精确性 |
|---|---|---|
| **吸收 `τ`** | `τ = ∫σ dt = −ln(1 − α_total)` | ✅ **精确**（= 光深；MB3DGS 亦指出 0 阶矩 = 总光深，无近似） |
| **色 `c`** | raymarch 的 **`L`**（预乘发射） | 与 MBOIT/WBO 的 `Σ cᵢ·T(zᵢ)` 语义配套（解析时乘 `T(z_repr)` = 前方几何透射率 → `L·T_front`） |
| **深度 `z_repr`** | 见 §4.3 | ⚠️ **近似**（"厚度"被压缩掉） |

**写入 WBO 纹理**（与 `jpov_transparent_glass_design.md` 同构）：
```
accum  += Σ (c · α · w)
accum.a+= Σ (α · w)            // 分母（加权平均的除数）
reveal *= (1 − α)              // 真实透射
depth  += z_repr · α · w       // 代表深度（z_g = depth / accum.a）
```

### 4.3 `z_repr` 怎么取（重要）

| 取法 | 评价 |
|---|---|
| AABB 前表面 `t_near` | 太粗（盒子前脸 ≠ 第一个有密度的样本） |
| **3D 纹理"首个非真空"样本** | 便宜（采样时顺手记），但**系统性偏前 → 过度遮挡**；飞火星/浮尘会让它抖 |
| **累计 α 阈值穿越（如 α 累计到 0.5）** ✅ | ≈ **遮挡的中位深度**，稳 |
| **期望深度 `E[z] = ∫ z·σ·T dt / ∫ σ·T dt`** ✅✅ | 物理质心，**积分里本来就有**，最合理 |

> **v1 建议**：用 **α 阈值穿越** 或 **期望深度**；**不要用"首个非真空"**（单边偏差会把火看得比实际更实）。

### 4.4 已知局限（v1 的边界，必须写清）

| 情形 | 单薄层 |
|---|---|
| 体积深度范围内**无其它透明物穿插**（火整体在玻璃前/后） | ✅ **准**（相对顺序保住；`z_repr` 取区间内任意值都行） |
| 有透明物**插在体积中间**（玻璃插进火里） | ❌ **崩**：一片表达不了"火的前半在玻璃前、后半在玻璃后" |

> **要点的张力**：*单薄层精确的场合（无穿插），你本来就不需要 OIT；需要 OIT 的场合（穿插），
> 单薄层最不准。* → 所以 v1 的价值是"**把体积火纳入统一透明合成**、并为升级铺好接口"，
> 而**不是**解决穿插。

---

## 5. Potential 升级（预留，不在 v1）

1. **多薄层切片（N 片）** ⭐ —— 把体积切成 2~4 个深度片段喂 WBOIT，覆盖绝大多数穿插，视觉接近真。
   `N=1` = v1；`N→∞`（把体采样成一批片段）= MBOIT 论文里"particle cloud"的原生做法。
2. **MBOIT** —— 用矩（power / trigonometric moments）**重建 `T(z)` 分布**，替换 WBO 的经验权重：
   - 颜色合成 `Σ cᵢ·T(zᵢ)` **≈ 精确 over**，误差只在"矩够不够多"；
   - 天然给 froxel **真实的 `T(z)` 分布**（而非单一 `z_g`），治 §6 的"多层会错"。
3. **热畸变 / 点光 / bloom / 烟层** —— 提升观感（见 `jpov_particle_fire_design.md` §11）。
4. **低分辨率 raymarch + 保边上采样** —— 与粒子的 1/2 分辨率 WBO 同思路。

---

## 6. ⚠️ 必须知道的下游代价（OIT 通病）

**所有 OIT 方法「每个像素没有单一深度」**（McGuire 2016）：与 **SSR / DoF / AO / TAA / 屏幕空间**
效果天生冲突。→ 本方案**必须额外导出"代表深度"**（就是 `z_repr` / `z_g`），
否则雾、SSR、DoF、TAA 又要抓瞎。**这正是我们此前纠结 `z_g` 的同一件事。**

froxel 联编（`jpov_froxel_design.md` §2.3）继续吃这个代表深度即可；单层薄片时用 `z_repr`，
多层/MBOIT 时用重建的 `T(z)`。

---

## 7. 先例（说明不是拍脑袋）

| 来源 | 做法 |
|---|---|
| **McGuire 2016**《A Phenomenological Scattering Model for OIT》| 把 **fog particles 纳入 OIT**："reduce complex scenes to a **single amalgamated layer per pixel**"；雾粒子那一级可**低分辨率** |
| **MBOIT**（Münstermann 2018, I3D）| Fig.2 即"**particle cloud + 四张透明面**"同一条光线；**原生吃 participating media** |
| **MB3DGS**（2025）| 体积（高斯）+ 矩 OIT 的中间地带；**0 阶矩 = 总光深，无近似** |
| **本引擎**（`jpov_transparent_glass_design.md`）| 玻璃↔雾已用 **`z_g`（单代表深度）** 干过同一件事 → 本方案是其推广 |

---

## 8. 本引擎落点（接口草案）

- **数据**：`Mantaflow → .jpovvol`（R=密度 / G=温度；2D array 纹理 = 帧）——**已有**。
- **渲染**：新增一个 **raymarch pass**（全屏 quad 或火的 AABB），产出 `(L, α)` 与 `z_repr`，
  **写入玻璃/WBO 纹理**（全分辨率；或复用粒子的低分辨率 WBO，按需）。
- **接入点**：`renderer.cc` 的透明阶段——玻璃 pass 之后、WBO resolve 之前，把体积火的薄层写进同一 FBO。
- **接口**（草案）：
  - `RenderCommandList` 增 `VolumeFireCommand`（绑定 3D 体纹理 / 帧、AABB、sigma 尺度、发射尺度、`z_repr` 策略枚举）。
  - `ParticleRenderer` / `GlassRenderer` 旁增 `VolumeFireRenderer`：`Begin/Draw/…` 对称生命周期（见 `SOUL.md`「Symmetric Lifecycle」）。
- **必配**：AABB clip + jitter + early-out（求积三件套）；与场景深度做遮挡。

---

## 9. 开放问题

1. `z_repr` 到底用**阈值穿越**还是**期望深度**？（先 A/B 出图定）
2. 薄层写**全分辨率玻璃 FBO** 还是**复用粒子 1/2 分辨率 WBO**？（精度 vs 成本）
3. v1 是否顺带导出**代表深度**供 froxel/SSR/TAA 用？（建议：要）
4. **多薄层（N）先做几片**？（2~4 之间 A/B）
5. 加法（发光）与覆盖（`over`）的混合口径，与 §6 的深度导出如何统一。

---

## 10. 参考

- McGuire & Bavoil, *Weighted Blended Order-Independent Transparency*, JCGT 2013.
- McGuire et al., *A Phenomenological Scattering Model for Order-Independent Transparency*, 2016.
- Münstermann et al., *Moment-Based Order-Independent Transparency*, I3D 2018 (PACMCGIT).
- *Moment-Based 3D Gaussian Splatting (MB3DGS)*, 2025.
- 本仓库：`jpov_transparent_glass_design.md` / `jpov_froxel_design.md` / `jpov_particle_fire_design.md`。
