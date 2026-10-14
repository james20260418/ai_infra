# JPOV 透明体渲染（无折射「塑料片 / 玻璃片」）—— 加权混合 OIT + 体积雾联编

> 状态：**已实现**（本 PR）。作者：James / Danis。日期：2026-10-14。
> 关联：`jpov_froxel_design.md`（froxel 体积雾；§2.3 区间反解、§7 薄/表面型透明）。
>
> 目标：给静态 Object3D 增加一个**半透明**子模式（`AlphaMode::kBlend`），在不影响实心
> 物体 early-Z 的前提下，把**无折射**的「塑料片 / 玻璃片 / 玻璃墙」正确渲出来，并让它们
> 与**体积雾（froxel）**按深度一起积分合成，而不是两张图简单叠色。

---

## 0. TL;DR

- **不透明的照旧**：新增一个 program 变体（宏 `JPOV_ALPHA_BLEND`），主 pass 的不透明
  program 不含 blend 分支，early-Z 深度快路径不受影响。
- **透明体单独一趟渲进「玻璃 FBO」**（**加权混合 OIT / Weighted Blended OIT**，
  McGuire & Bavoil 2013）：每像素得一对 `(预乘色 C_g, 透射率 T_g)` + 一个**代表深度** `z_g`。
  单趟、不排序、任意层数，`C_g/T_g` 天然就是一段「段（segment）」。
- **体积雾 composite 与玻璃联合积分**：用 froxel 的**任意 z 区间 O(1) 反解**（见 froxel §2.3），
  把像素沿视线切成 `雾(0→z_g) → 玻璃 → 雾(z_g→z_op) → 背景`，front-to-back 合成。
  无玻璃时**逐字退化为原全雾路径**（零回归）。

---

## 1. 模型：玻璃 = 表面 + 介质段

不带折射的玻璃 = **介电界面（菲涅尔反射）+ 均匀吸收介质（Beer-Lambert）**。
水和它是同一族：水 = 本框架 **+ 折射（背景位移）+ 动态法线**。**「不带折射」正是本方案
屏幕空间联编能成立的前提**——背景不被位移，才能在一次屏幕空间合成里按深度切片积分。

因此玻璃层抽象成一个**段**：`{ 预乘色 C_g, 透射率 T_g, 代表深度 z_g }`，和 froxel 的
雾切片（`(τ,S)` 段）同构，合成器只需按深度把若干段 front-to-back 串起来。

---

## 2. 材质：`AlphaMode::kBlend`

`interface/pbr_material.h`：

```
enum class AlphaMode : uint8_t { kOpaque, kMask, kBlend };
```

- `kBlend` 语义：
  - `alpha = base_color.a`（无贴图）或 `base_color.a × 贴图 alpha`（有贴图）；
  - **不写主 3D FBO 颜色 / 不写场景深度 / 不投阴影**；
  - 本版**不参与 picking / highlight**（后续可加）。
- 与 `kMask` 一样**另起 program 变体**（宏 `JPOV_ALPHA_BLEND`）——主 pass 的不透明
  program 不含 blend 分支，**不降级实心物体的 early-Z 深度快路径**。
- `double_sided`：透明薄片建议 `true`（双面都参与 WBO 才有「有厚度」观感）。

---

## 3. 玻璃 FBO 与 WBO 趟

`Renderer` 持有 **1× 单采样玻璃 FBO**（结构化数据不能被过滤/平均），主 FBO 尺寸，
三个颜色附件（无深度缓冲）：

| 附件 | 格式 | Clear | blend func | 写入（FS） |
|---|---|---|---|---|
| accum | RGBA16F | (0,0,0,0) | `ONE, ONE` | `vec4(色·α, α)·w` |
| reveal | R16F | (1,0,0,0) | `ZERO, ONE_MINUS_SRC_COLOR` | `α` |
| depth | R32F | (0,0,0,0) | `ONE, ONE` | `z_view·α·w` |

**权重函数**（`object3d_renderer.h` 的 `weight_wbo`，原式）：
`w = max(min(1, maxRGB·α), α) · clamp(0.03/(1e-5+(z/200)^4), 1e-2, 3e3)`
（颜色因子防亮/稀薄片元颜色污染；深度因子近处优先；不含 α 因子，α 累加时另乘）。

**透明 pass** = `Renderer::DrawTransparentPass`：绑定玻璃 FBO、**关深度测试**、
按附件设 blend func，遍历 `cmds.object3d` 中 `alpha_mode==kBlend` 者，用
`Object3DRenderer::DrawObject3DBlend` 画。**手动深度测试**在 FS 内完成
（`texelFetch(uGlassSceneDepth, …)` 与 `gl_FragCoord.z` 比较，丢弃被不透明遮挡者），
故本趟**无需深度缓冲**。

WBO 解算（合成端，composite shader 内）：
```
Cavg = accum.rgb / accum.a         // 加权平均色
Tg   = reveal                      // 透射率 = Π(1−α)
z_g  = depth / accum.a             // 代表深度
Cg   = Cavg · (1 − Tg)             // 预乘段色
```

---

## 4. 与体积雾联合积分（composite 合并）

`fire_fog_shader.h` 的 `kFireFogCompositeBody`。设：

- `S_f, T_f = fog(0→z_g)`（用 froxel 区间反解，`T_f = exp(−τ_f)`）；
- `S_op, T_op = fog(0→z_op)`（`z_op` = 不透明场景深度）；
- 玻璃段 `(C_g, T_g)`；背景 `L_bg = dst`。

front-to-back：
```
final = S_f + T_f·C_g + T_g·(S_op − S_f)  +  T_g·T_op·L_bg
        └────────────── out.rgb ──────────┘   └── out.a ──┘
```
（用 `T_f·T_g·S_b = T_g·(S_op−S_f)`、`T_f·T_g·T_b = T_g·T_op` 化简，避免负指数爆炸。）
composite 以 `GL_ONE / GL_SRC_ALPHA` **就地**混进 HDR FBO：`final = out.rgb + dst·out.a`。

**零回归**：无玻璃时把 `z_g=z_op, C_g=0, T_g=1` 代入 ⇒ `out.rgb=S_op, out.a=T_op`
——**逐字等于原全雾路径**（gold 验证 `max-channel-mean-diff = 0`）。

以 `gl_FragCoord.xy` 在两处各做一次 4 邻 tile 双线性采样（`SampleFogBilinear`）。

---

## 5. Pass 顺序 / 唤醒条件

3D 段内（HDR 路径，`tone_mapping=true`）：

1. 阴影 pass（**跳过** kBlend）；
2. 不透明主 pass 画进单采样 HDR FBO（**跳过** kBlend）；
3. 场景深度在 MRT#1（单采样）；下游雾 pass 采样它时临时摘下该附件防反馈环；
4. **玻璃 pass**（若有 kBlend）：`EnsureGlassFBO` + `ClearGlassFBO`（无玻璃也清空，
   提供恒等层）+ `DrawTransparentPass`；
5. 远景仰角雾（不变）；
6. **froxel 体积雾 composite**（唤醒条件 = 有雾**或有玻璃**）：把玻璃段一并合成。

`CHECK(!has_glass || use_hdr)`：kBlend 仅在 HDR 路径支持（否则 crash，不静默失效）。

---

## 6. 关键约定与坑

1. **玻璃 FBO 是 1×**：结构化 MRT 不能被过滤/平均（同 pick id 的教训）。
2. **blend func 属 `GL_COLOR_BUFFER_BIT`**，不在 `glPushAttrib(GL_ENABLE_BIT)` 里
   ——透明 pass 结束必须**显式回写**（对三个 draw buffer 都回写）。
3. **手动深度测试**取代深度缓冲：本趟无 depth attachment；FS 采样不透明场景深度比较。
4. **单层折叠**：WBO 把该像素所有玻璃片元煮成一段（`C_g,T_g`）落在代表深度 `z_g`；
   玻璃内部夹的雾会被近似（薄片可忽略）。要真交错才需升级 K 层。
5. **FS 里 `gl_FragCoord.z` 与场景深度同源**（同一 `mvp`、同一 viewport）——比较才成立。
6. 权重函数不含 α（α 在累加时另乘），且 `min(1,·)` 项防 firefly——实现按原式。
7. ★ **深度因子的尺度常数必须按场景尺度标定**：论文原式
   `clamp(0.03/(1e-5+(z/200)^4), 1e-2, 3e3)` 里的 `200` 是**世界单位尺度**。
   JPOV 以**米**计、玻璃多在 1~30m，照抄 `200` 会让 `(z/200)^4 ≪ 1e-5` 地板
   → 深度因子**退化成常数** → 「近处优先」失效 → 两层透明体（如「红墙 × 绿人」）
   前后权重相同 → **order-blind、前后颜色一样**。本实现改为 `z/5`（米级，可按场景调）。

---

## 7. 已知边界 / 后续

- **折射**不做（本方案前提）；加了折射得改成对背景 uv 位移采样，屏幕空间联编失效。
- **picking / highlight / 阴影**不认 kBlend（本版显式跳过）。
- **instanced / skinned 的 kBlend** 未接入（本版只做 object3d）。
- 玻璃**自发光**按 WBO 的 `result`（含 emissive）计入，非按透过率对段内积分；
  厚自发光体需另做。
- 若需**精确逐层**（相交玻璃 / 多层厚玻璃）：把 WBO 换成 K 层机架（本框架可复用 composite 的区间反解）。

---

## 性能 / Overdraw

透明 pass **无深度缓冲 ⇒ 无 early-Z**：每个透明片元都被完整着色一次（overdraw = 该像素上的透明片元数，
与「排序后的 alpha-blend 透明」相同；WBO 的价值是**单趟 + 不排序**，不是减片元数）。WBO 特有开销：
**3 个浮点 MRT 写 + 3 次 blend** / 每片元，且 FS 是整套 PBR。已做的优化：
1. **深度早退置顶**：被不透明场景遮挡的玻璃片元在 FS 开头就 `discard`（只依赖 `gl_FragCoord.z`
   + 场景深度），省掉后面整套 PBR。
2. **无 kBlend 物体时不建/清玻璃 FBO、不跑透明 pass**（composite 的 `uHasGlass=0` 不读玻璃纹理）
   ⇒ 纯雾帧省掉 3 张全屏浮点纹理的 clear + 采样。

可选的进一步优化（未做，按需）：玻璃用**轻量 FS**（略 CSM PCF / 点光 tile）、无雾时省掉第 3 个附件、远处 LOD/impostor。

---

## 附：经典 WBO 参数基线（McGuire & Bavoil 2013）

| 附件 | 格式 | Clear | blend func | 片元写入 |
|---|---|---|---|---|
| accum | `RGBA16F` | `(0,0,0,0)` | `(GL_ONE, GL_ONE)` | `vec4(c.rgb·α, α)·w` |
| reveal | `R16F`（论文原版 `R8` 精度差） | `(1,0,0,0)` | `(GL_ZERO, GL_ONE_MINUS_SRC_COLOR)` | `α` |

- 权重：`w = max(min(1, maxc·α), α) · clamp(0.03/(1e-5+(z/200)⁴), 1e-2, 3e3)`
  （① 颜色因子防 firefly；② 深度因子「近处优先」）。
- 合成：`C = accum.rgb/max(accum.a,1e-5)`、`T = reveal`（= Π(1−α)，**精确**）；
  `out = vec4(C·(1−T), 1−T)` 再 `over` 到背景。**coverage 精确、颜色加权平均。**
- 单趟、不排序。JPOV 只多一个 `R32F` 附件存**代表深度**（供体积雾联编），并把深度
  尺度 `200` 重标为 `5`（米级，见 §6.7）。

---

## 8. 验收（本地）

- `fire_fog` 全量单测 + 2 张 gold：**全过**，god ray / pointlight gold `max-channel-mean-diff=0`（零回归）。
- `object3d` / `ui` / `interface` gold：**全过**（`DrawObject3D` 抽公共 helper 后 opaque/cutout 逐字节不变）。
- ff viewer headless：round box 雾场景 + 树一侧 **MakeBox 浅红玻璃墙**（`70_glass_off` /
  `71_glass_on` / `72_glass_alpha60` / `73_glass_alpha15`）：树与雾透过玻璃可见，红色吸收
  随 α 增减，玻璃被后方不透明物体遮挡时正确消失。
