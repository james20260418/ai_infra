# JPOV 植被（植物大作战）设计调查

> 日期：2026-10-08
> 状态：**调查 / 认知文档**（未定稿、未实现）。给 Danis 看方向，等拍板。
> 动机：引擎要能处理「大雨林」量级的植物，不重蹈 Tripo 单图重建橡树（`assets/models/samples/oak_negative/`）
> 的覆辙——那棵树本质是「一堆朝向随机的离散面片」，透空率高、充满塑料碎纸感。
> Danis 定调：**植物不求厉害细节，但求量大可靠。**

---

## 〇.5、锁定决策（2026-10-08 Danis 拍板）

以下为 Danis 本轮明确拍板的方向，后续设计以此为准：

1. **blend（透明混合）先不做。** 整条透明混合链路（排序 / 不写深度 / 与 MRT#1 场景深度的冲突）本轮不碰。
2. **双面渲染**（`gl_FrontFacing` 翻法线 + 关背面剔除）。
   - 原定案是“默认开启”，但**实现改为按材质开关**（详下）：全局强制会改到既有 gold，按资产 flag 走 = glTF 规范做法 + 零回归；
     要的薄片资产（Tripo 的 oak / mixamo_male / harness_vest / 纱裙 都标 `doubleSided=true`）照样双面。
   - 做法：批级 `glDisable(GL_CULL_FACE)` + FS 里 `if (!gl_FrontFacing) N = -N;`。成本 ≈ 0，不用 shader 变体。
3. **cutout（alpha test）走独立 shader program**，**不**塞进现有 program 的 uniform 分支——
   因为 `discard` 只要出现在 shader 里，编译器就保守地把 early-Z 的 depth-write 快路径降级，即使那批从没走到 discard。
   - 现有 PBR program 保持**无 discard**（opaque，zero overhead）；
   - 新增一个 **cutout 变体 program**（含 discard）；
   - **按材质（`PBRMaterial.alpha_mode`）在 draw 时选 program**，不是 `Draw*` 内部临时判断。
4. 所以「**先修旧 shader**」= 给旧 shader 出一份 **opaque / cutout 两个变体**（+ 双面）。

---

## 〇.6、本轮实现状态（2026-10-08）

- **cutout 已实现（object3d 普通 renderer）**：`PBRMaterial.alpha_mode{kOpaque,kMask}` + `alpha_cutoff`；
  loader 读 glTF `alphaMode`/`alphaCutoff`；`ShaderManager` 支持编译期 `defines` 生成**同源变体**
  （FS 里 `#ifdef JPOV_ALPHA_CUTOUT ... discard`），opaque program 不含 discard；
  `renderer` 按 `alpha_mode` 选 program（`DrawObject3DProg*Cutout`）；saver 写回 `alphaMode`/`alphaCutoff`。
  ⚠️ **skeleton renderer 尚未接 cutout**（下个 PR）。
- **双面已实现**：`PBRMaterial.double_sided` ← glTF `doubleSided`；draw 时按它关 `GL_CULL_FACE` + FS 翻法线。saver 已能写回 `doubleSided`。
- **glTF 外置纹理导出**：`gltf_saver::WriteGltf`（`.gltf + .bin + 独立贴图`）；model editor 加“导出 gltf(外置)”按钮 + CLI `--save_format gltf`。
- **blend 未做**（按决策 1）；**shadow pass 未 discard**（裙子投影仍实心轮廓）—— 已于 §0.7 补齐。
- 验证：`//tools/jpov/test/object3d:all` 14/14、`jpov_gltf_saver_test` 12/12 绿。

---

## 〇.7、后续实现状态（2026-10-09）

- **skeleton renderer 接 cutout + 双面**（PR #156）：蒙皮带骨实例此前只走不透明 program，现按
  `alpha_mode==kMask` 选 cutout 变体（`JPOV_ALPHA_CUTOUT`，含 `discard`）、`double_sided` 时关背面
  剔除 + FS 翻法线 —— 与 object3d 对齐。
- **阴影 pass 接 cutout + 双面**（本 PR）：object3d 与蒙皮**两条阴影路径**都拆出 cutout 变体
  （同源 + `JPOV_ALPHA_CUTOUT`），按材质选 program + 采样 baseColor 的 alpha 做 `discard`；
  `double_sided` 时关背面剔除（两面都投影）。不透明阴影路径零行为变化。
  ⇒ **叶片 / 蕾丝这类镂空资产的投影不再是实心轮廓。**
- 因此「①透明切孔」这条线在引擎侧的底座已齐：[主 pass cutout（object3d #155 / skeleton #156）] + [阴影 pass cutout（本 PR）]。

---

## 〇、一句话结论（先给答案）

**真实感植物不是「建出来的」，是「骗出来的」。** 全行业（含 3A）的植物管线 = 三层谎言叠加：

1. **叶子 = 纸片（alpha card）**，不是真叶子几何。一张（或几张交叉的）带透明切孔的四边形贴图冒充一簇叶。
2. **同模型 × instancing × LOD**，靠「数量 + 距离分级」压出雨林的密度，靠 **impostor（远景替身图）** 收尾。
3. **风 = VS 里的程序化顶点偏移**（无状态、确定性），**四季 = 参数/贴图渐变**，都不是物理、不是双份几何。

→ 所以 Danis 的 4 个方向，本质是这 4 条工程线的名字：**①切孔贴图与 overdraw ②程序化生成器 ③VS 风 ④材质季节化**。
其中 **①的「透明切孔」是当前引擎最大的硬缺口**（object3d 的 PBR 材质完全没有 alpha 语义）。

---

## 一、植物一般咋整 —— 一条标准管线

### 1.1 资产层：叶子永远是「卡片」（blade card）

- 一片「叶子几何」在引擎里 = **一个/几个 quad**，贴上「一簇叶子」的贴图，alpha 通道抠出叶形（**cutout**，非渐变透明）。
- 常用形态：**十字交叉 quad（X-card）**、**扇形 cluster card**。近处多少片、怎么摆，由生成器决定。
- 贴图通常打包成 **atlas**：一簇叶子 / 一根树枝 / 一块树皮各占一格，减纹理切换。
- 卡片法线的著名技巧：**「球形/弯曲法线」**——法线不取卡片面法线，而取「从卡片中心指向顶点再外张」的方向，让叶子不是死平面、受光有体积感。这是「不像纸片」的关键一手。
- 双面渲染（关 backface cull，背face 翻转法线）。

### 1.2 生成层：程序化，不是 AI 重建

- 业界主流：**L-system（语法）** / **Space Colonization（空间殖民，Runions）** / **SpeedTree 式参数化**（trunk 样条 + 分支规则 + 卡片摆放）。
- 输出物**天然是 instancing 友好**的：① 一张**枝干网格**（不透明，走普通 PBR）② 一批**叶片卡片实例**（同一个卡片 mesh 摆 N 次）。
- 变化来自 **seed × 参数**，不是「每棵树一个专属网格」。这正面回答 Danis 的「不可能树都长一样」——靠种子 + 生长参数发散，而不是重建。
- ⚠️ **AI 单图重建对植物是死路**，`oak_negative/` 已留证：信息量不足以推断「每片叶在哪、朝哪」，拉面数只救枝干、救不了树冠。

### 1.3 渲染层：三条性能支柱

| 支柱 | 手段 | 解决什么 |
|---|---|---|
| **压 draw call** | Instancing（同 mesh 万份，per-instance 矩阵） | 一整个森林几次 draw |
| **压像素/几何** | LOD + **impostor 替身**（远处整棵树 = 1 张预烘图的四边形） | 距离越远越"假"，但肉眼认不出 |
| **压 overdraw** | cutout 走不透明 pass + 前后排序 + LOD | 半透明叠层是像素杀手 |

### 1.4 光照层

- 常规 PBR 就够（我们已有）。
- 可选增强：**叶片次表面/透光（translucency）**——背光时叶子透红/透绿，是「活」的关键观感。属加分项，非必需。

---

## 二、逐条对照 Danis 的 4 个方向

### 方向 1 — 透明纹理的制备与渲染（叶子）+ 如何避免 overdraw

**制备**：alpha 是**切孔（cutout）**语义——绝大多数像素 alpha ∈ {0,1}，边沿极少中间值。DCC（SpeedTree/Blender）或程序化（噪声阈值）均可，关键是把「一簇叶」画进一张图，别留大片全透明区（那是纯粹的浪费）。

**渲染（业界两条路）**：

| 路 | 手段 | 排序 | 早深度（early-Z） | 影/拾取 |
|---|---|---|---|---|
| **A. Alpha-Test（cutout）** ✅主流 | FS 里 `discard`（alpha < cutoff） | **不需要排序** | **可用**（不透明 pass，写深度） | 正常 |
| B. Alpha-Blend | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | 必须远→近排 | 不能（要读 dst） | 麻烦 |

→ **叶子第一选择永远是 A（cutout）**。它让植物搭上现有不透明管线（含 CSM 阴影、picking），代价只是 `discard` 的边缘有锯齿。

**关于 overdraw（这是重点，要纠正一个直觉）**：

- **`tile culling` 不是解 overdraw 的工具**。引擎里已有的 tile culling 是 **Forward+ 光源剔除**（每 tile 只算命中光源），治的是「光源太多」。overdraw 是「同一像素被画太多遍」，两码事。
- overdraw 的正解是这四把刀（按性价比）：
  1. **cutout + 深度写入**（让被挡的片元死在早深度，而不是走完 FS）；
  2. **绘制顺序**：先不透明 → 再 cutout 植被**从近到远**（乱序会让早深度失效）；
  3. **LOD / impostor**：远处叶子根本不是几何，是最便宜的一刀；
  4. **减少卡片数与尺寸**（生成器层面：叶密度旋钮）。
- **可选进阶：Alpha-to-Coverage（A2C）**——用 MSAA 把 alpha 转成采样覆盖率，既不排序又能抗锯齿，是 3A 常用配置。**本引擎有 MSAA 路径**（Linux headless），具备落地条件，但属第二步。
- ⚠️ JPOV 特有的清醒认知：**llvmpipe 是软件光栅器**，早深度/HW overdraw 优化的收益模型与真 GPU 不同；JPOV 的首要约束是**确定性**（gold 逐字节可复现），overdraw「优化」更多是**为真 GPU 的前瞻设计**，不是本地 fps 指标。

**对应到引擎的缺口（关键）**：`PBRMaterial` 目前**完全没有 alpha 语义**（无 `discard`、无 `alpha_mode`/`alpha_cutoff`）。
唯一带 alpha 的 3D 通路是 `kText3D`（字形 atlas 覆盖率，`text3d_depth_alpha_*` 已验证）——证明了「带 alpha 的贴片能画」，
但它不是通用的 PBR cutout 材质。**这是植物战役的第一块砖。**

### 方向 2 — 植物模型的智能化构造（生长期、枝叶比例……）

Danis 猜「这里有生成器」——**完全正确，且这正是正解**。业界叫 **procedural vegetation / parametric tree**。

- 经典算法：**L-system**、**Space Colonization**（对着「向光吸引点」生长，效果自然、实现简单）、**SpeedTree 参数化**。
- 可控旋钮（正是 Danis 要的「生长期 / 枝叶比例」）：`age`（生长阶段）、`seed`、`branch_angle`、`branch_ratio`、`leaf/branch ratio`、`phyllotaxis`（叶序）、`taper`……
- 产物 = **枝干网格 + 叶片卡片实例列表**，**确定性（同 seed 同结果）**——和 JPOV「确定性沙盒」的气质天作之合。
- 变体策略：一个「树种」= 一组参数 + 一个种子范围；同屏一棵树 N 份实例化，靠 **per-instance 随机（seed/朝向/整体缩放）** 出差异，**不各建网格**。

**建议**：JPOV 做一个 **CPU 侧、GL-free、可单测的 `VegetationGenerator`**（`geom/` 风格纯函数），
输入参数 + seed，输出 `{branch MeshData, leaf card 实例列表}`。天然契合 JPOV 的「先做对，再做全」。

### 方向 3 — 风（VS 风矢量 + 拓扑噪声 + 是否需要物理）

**结论：不需要物理引擎。** 树叶风动是**无状态程序化顶点动画**，不是物理解算。

- 经典做法（GPU Gems 1 ch7 / GPU Gems 2 ch1、SpeedTree）：**正弦叠加**
  `offset = 风向 × 振幅 × 风权重(顶点) × Σ sin(ω·t + φ)`
- **「风权重」= 该顶点多容易摆**：树干 ≈ 0、枝梢 ≈ 1。来源两种：
  - **烘焙进顶点属性**（生成器直接给）——最稳；
  - 或 **从 rest 位置/枝序推导**（越高/越末梢越软）。
- **「拓扑相关噪声」Danis 这个直觉是对的**：每个顶点/每根枝应有**不同的相位 φ**（否则整棵树同步摆动，露假）。相位可由 **枝序层级 + 世界位置哈希** 派生——这就是「拓扑相关」。
- **分层风（hierarchical）**：树干慢摆（低频大振幅）+ 枝叶快颤（高频小振幅），叠加出自然的「呼吸感」。
- 进阶：把枝干当「少量骨骼」做类蒙皮变形（论文里的 foliage skeleton LOD）——**但那是远距离的 LOD 取舍，不是首选**。
- JJOV 已有先例可复用：`clothing/random_pose_driver.h` 就是一套「确定性程序化姿态驱动」（splitmix64 派生相位 + 正弦）。
  **风 VS 是它在顶点着色器里的近亲。**

**建议**：`wind` 指令字段 = `{direction, strength, time}`；VS 里 `pos += wind_term`，权重来自顶点属性。
**时间由用户推（确定性），引擎不持有时钟**（同 pose 系统约定）。

### 方向 4 — 四季、雪

Danis 猜「纹理的渐变，总不至于是画两遍 Dither 吧」——**方向对，细节要修**：

- **四季 = 参数/贴图渐变（lerp），不是 dither，也不是两套几何。**
  - 最简：季节 uniform `s∈[0,1]`，shader 在「夏贴图」与「秋贴图」之间 `mix`（albedo/roughness/normal 各 lerp）。
  - 落叶（defoliation）：让叶片卡片的 **alpha 或 wind 权重** 随 `s` 降低 → 叶子变稀/消失，不是换 mesh。
- **雪 = 「朝上的面才积雪」的叠加**：
  `snow = snow_amount × clamp(N.y,0,1) × snow_mask`
  → 把雪 albedo 混进去 + 抬高 roughness + 降低 metallic。`snow_mask` 可用顶点色/贴图控制「哪里挂得住雪」。
  这本质是 **一个朝向相关 + 一个全局量的贴图混合**，一次 FS 里完成。
- **dither（抖动）真正的用途 = LOD 切换的渐变**（远近 LOD 之间用 Bayer dither 抠像交叉淡入），
  以及 **随机透明/soft shadow**。**不是四季**。所以 Danis 的直觉方向对（渐变），但把 dither 用错场景了。

**建议**：材质加 `season` / `snow` 两个全局 uniform + 面向雪的朝向项即可，零几何改动、零额外 draw。

---

## 三、JPOV 现状 vs 植物所需（缺口清单）

| 能力 | 现状 | 植物需要 | 缺口 |
|---|---|---|---|
| 静态物体 instancing | ❌ **只有骨架蒙皮 instancing**（`DrawSkinnedMesh`）；**非骨架 `DrawInstancedObject` 未做**（#106 已标注「下一步」） | 同 mesh 万份 | **要补** |
| Instancing 基建（per-instance buffer/RAII） | ✅ 已有（`instance_buffer.h`，PR #106） | — | 复用即可 |
| **Alpha cutout 材质** | ❌ **无**（PBR 无 alpha；仅 `kText3D` 有覆盖率 alpha） | 叶子切孔 | **最大缺口，第一块砖** |
| 双面渲染 / 法线翻转 | ❌ 无 | 叶片双面 | 要补 |
| tile culling | ✅（但治的是**光源**，不是 overdraw） | — | 不是本用 |
| 阴影 CSM | ✅ | 植物投影 | 复用（shadow pass 已支持 `discard` + 双面，见 §0.7） |
| LOD / impostor / billboard | ❌ 无 | 压远处 | 后续 |
| 植被生成器 | ❌ 无 | 树/草 | 新增（CPU/GL-free） |
| 风 VS | ❌ 无 | 摆动 | 新增 |
| 季节 / 雪 | ❌ 无 | 四季 | 新增 |
| 地形（承载雨林） | ❌ 未开发（已定：高度场 + CDLOD） | 地面 | 后续，可先平地 |
| 撒布 / placement | ❌ 无 | 铺满雨林 | 后续 |

> 注：Danis 说「object3d 引擎目前还没有 instancing 接口」——**基本准确**：
> 基建（per-instance buffer）已在，但**只接到骨架蒙皮**，静态物体那条 `DrawInstancedObject` 确实还没做。

---

## 四、建议的推进路线（小步、每步可独立验证）

> 遵循 JPOV 老规矩：**先做对再做全**，每步产出可跑 gold / 可单测。

**Step 0 — 两块地基（不含植物，通用能力）**
1. **Cutout 材质**：`PBRMaterial` 加 `alpha_mode{none|mask} + alpha_cutoff`，FS 加 `discard`；
   **shadow pass 同步 `discard`**（否则叶子影是方片）。→ 这一步本身就是引擎级升级。
   ✅ **已实现**（2026-10-09）：object3d（#155）/ skeleton（#156）主 pass + 两条阴影 pass（本 PR）均已支持 cutout + 双面，见 §0.6/§0.7。
2. **静态 `DrawInstancedObject`**：补完 instancing 第二条腿（Danis 在 #106 已点名它是下一步）。

**Step 1 — 第一棵「不丢人」的树**
3. `VegetationGenerator`（CPU/GL-free）：参数 + seed → 枝干网格 + 叶片卡片实例；单测验确定性。
4. 风 VS（正弦 + 拓扑相位），用户推 time。

**Step 2 — 量大**
5. 撒布 placement（先平地铺一片）+ 静态 instancing 批量绘制。
6. LOD / impostor（远树 = 一张替身图）。
7. （可选）A2C 抗锯齿切孔。

**Step 3 — 四季 / 雪**
8. `season` / `snow` 全局 uniform + 材质渐变。

---

## 五、待 Danis 拍板的几个点

1. **第一步只做「地基」还是直接冲「一棵树」？** 我建议先做完 Step 0 两块砖（cutout + 静态 instancing），
   它们独立可验证、且被所有后续复用。
2. **叶片走 cutout（不透明 pass）还是 A2C？** 建议先 cutout（简单、能吃现有阴影/拾取），A2C 留作第二步。
3. **生成器算法选型**：L-system vs Space Colonization？我倾向 **Space Colonization**（自然、实现短、确定性好）。
4. **风要不要接「分层风（干+枝+叶三频）」**，还是一期先单频？
5. **承载面**：一期用平地还是先动地形（高度场 + CDLOD）？

---

## 六、纠正的 3 个直觉（供记录）

| Danis 的直觉 | 修正确认 |
|---|---|
| 「避免 overdraw 是不是靠 tile culling？」 | ❌ tile culling 治**光源**；overdraw 靠 **cutout + 排序 + LOD/impostor** |
| 「风或许需要物理引擎？」 | ❌ 风 = **无状态 VS 程序化动画**（正弦叠加 + 拓扑相位），确定性、便宜 |
| 「四季是纹理渐变，总不至于是画两遍 dither？」 | ✅ 是**渐变（lerp）**；但 dither 的真正用途是 **LOD 切换**，不是四季 |
