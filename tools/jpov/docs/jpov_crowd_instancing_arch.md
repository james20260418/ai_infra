# JPOV 城市级人群批量实例化（Crowd Instancing）架构锚点

> 场景：用 JPOV 做「类 Cities:Skylines 的城市」——场上同时有**上千个各有差异的独立个体**，
> 第一优先是**压住 draw-call 卡脖子**（instancing / batch 化），单体画质与动作丰富度延后。
> 本文档记录 2026-09-03 与 Danis 连续架构推演的**收敛结论**，供后续 agent 直接照此实现，
> 避免把已经想透的边界重新推翻。
>
> 前置阅读：本文档是 `docs/jpov_engine_integration.md`（JPOV 作 0 级引擎的边界收敛）的
> 直接延续——那里的「世界演化归用户/渲染归 JPOV」「每帧全量 cmds 缝是真实接口」「static meshing ✅ /
> 分层/局部更新 ❌」结论在这里原样适用。文档描述以代码现状为准。

---

## 0. TL;DR（给急读的 agent）

- **本系统的核心不是「一个能动画的高画质角色」，而是「上千个廉价、各有差异的个体」。**
  第一优先 = **instancing / batch 解决 draw-call 瓶颈**；单体动作丰富度、真实布料、精细 rig 全部后排。
- **统一心智模型：肉体与衣物在 GPU 数据格式上退化成同一种东西**——都绑定到**同一套共享骨架**的
  rest-mesh「部位变体」，进 GPU 后按**共享骨架**蒙皮渲染。一个 instance = 一串 **part selector**（int 索引），
  不是自己背一整份几何结构。
- **凡一个 instance 要廉价切换的东西（肤色 / 长相 / 衣物 / 装备 / 身体微调），一律不换「几何」，
  而是换「一个廉价索引/tint」去 select 共享区的大块数据**（材质 / 贴图 / 骨架矩阵 / rest-mesh 变体）。
- **肢体拆分门槛（属 S0 打底结构）**：至少能区分「小臂 / 大臂 / 头部」这一档（不是简约整身，
  也不是精细到手指），part_pool 按此分部位可拼。
- **S0 边界**：S0 只聚焦「把千人 instancing + selector + 到臂/头部位的 rest-mesh 蒙皮通路跑起来」。
  **风动物理与颜色微调列为 TODO / 后续里程碑（见 §7），不进 S0。**

---

## 1. 目标场景与第一优先

### 1.1 目标画面

类 Cities:Skylines 的城市远景：**一次到至多次 batch draw 画出一整片街道的人**，
每个人看起来有差异（个体样貌 / 肤色 / 穿着 / 体型微差），但**不需要每个都是独立高精角色**。

### 1.2 为什么「第一优先是 instancing，不是画质」

- 画「1000 个独立个体」，若每人一个 `Object3D`-风格 draw call / 独立 uniform，
  **draw-call 数量会直接卡脖子**（甚至在 shadow / picking / highlight 每个 pass 都再放一张 for 全扫一遍，
  见 `src/renderer.cc` 现有对 `object3d` 的四 pass 逐 object 循环）。这是当前架构**明确不以人数规模**为本格的证据。
- 因此骨架完整起见：**人的系统第一目标是「密度× 差异达到能看」，不是「单帧单角色画得很细」。**

### 1.3 与「单人高质量」的分工（取舍，不背锅延后项）

下列**明确不是本首屏目标**，延后（避免把首屏做太重、方向被画质牵走）：
- 单个角色的高模级几何精细度；
- 真实布料 / 裙摆 / 披风的逐 instance 物理解算（独立动态形变，见 §6 坑1 的便宜档 vs 贵档）；
- 精细到手指的 rig、复杂动作状态机、真人级动作混合；
- bindless 纹理 / compute skinning / GPU 驱动逐 instance 独立动画（「每个 instance 自己动自己的动作」那台更大的机器，见 §6 坑2 的取舍说明）。

---

## 2. 核心心智模型：rest 网格(共享) + 共享骨架 + per-instance selector

这是整篇的基准。**一个 instance 不是「一份几何 / 一份动作」，而是一条『如何从共享素材池
长出我看得见的这一份』的纯描述符（state）。**它由一小撮**廉价 selector / tint / 标量**组成，
去 select 共享区的大块数据；不把大块数据逐 instance 复制。

```
共享 GPU 池（全部绑定同一套骨架, rest/bind 姿态、一次性上传）:
  skeleton            —— 一套关节层级 + inverseBindMatrices（千人共用）
  part_pool[]         —— 绑骨 rest-mesh 变体:
                          body   : head#0/#1 / torso / upper_arm / lower_arm / hand / torso…
                          cloth  : topA / topB / armor… / pants… （都 bind 到同一骨架）
  material_tex[]      —— baseColor/normal/metallicRoughness… 变体（肤色/穿着差异在此）

instance[i] (很薄, select 共享区):
  transform          —— 摆放 (等价现有 Object3D 的 center/up/front/scale)
  body_part_idx[]    —— 此人由哪几个绑骨 part 拼成（头/躯干/上臂/小臂/手… 各一 int）
  cloth_part_idx[]   —— 此人的衣物款式（外衣/裤/… 各一 int），可为「无」
  cheap_body_scale   —— 几档全局/轴向 scale 标量（身高胖瘦微差，见 §6 坑3）
  face_index         —— 肤色/五官外观 → texture-array / 材质纹理变体索引
  tint (vec3)        —— 肤色 / 衣物颜色微调（廉价乘子）
  seed (int)         —— 派生上面各 selector 的确定性随机源（同 seed 同长相，可复现）
```

### 2.1 关键收益

画「一个穿了制服的宽肩瘦男」与画「一个光膀子壮汉」，CPU 只多传几个 int selector，
几何与蒙皮矩阵**全共享**。千人 = 反复 select 共享 part + 一次蒙皮共享骨架，
刷屏仍是受限次数的 instanced/batch draw —— **draw-call 卡脖子当场解除**。

### 2.2 两个分岔要在实现层写死（不是 S0 就要解，但文档先把门分清）

1. **动作推演放 CPU（共享区）而非逐 instance 上 GPU**：
   千人若共享骨架 / 共享动作(clip)，正确做是 **CPU 跑一个 clip 采样器，算出那套共享骨架的
   每帧关节矩阵，上传一次**，然后所有同骨架 instance 共享这份矩阵去蒙皮。
   **不要在 shader 里让每个 instance 自采样动画**（那要把整份动画采样状态逐 instance 上传，很贵）。
   → 语义上 instance state 里的「动作」往往只是一根指向『已求值好的共享骨架矩阵 / 相位』的信号。
2. **蒙皮在「共享骨架矩阵」上做**，不是每个 instance 一套骨骼。这才保证衣服肉体都可复用同一份。
   （若日后真要做「每个 instance 独立动自己的动作 / 独立绑骨」，那是「GPU 角色实例化 / compute skinning」
   这台更大的机器，明确延后，见 §6。）

---

## 3. 表现层「select 共享区 = 廉价方案」的统一准则

> **准则：换外观就换索引/tint（texture-array / 材质变体），别换几何；走形变差别且够便宜时用标量；
> 要几何差别用『共享的 N 个 baked 变体之一』+ selector。instance 只 carry selectors，select 共享区。**

这一条同时解答「肤色 / 长相 / 衣物 / 4 副面孔廉价切换」：

| 想切换的东西 | 廉价方案 | 实现载体 | 成本档 |
|---|---|---|---|
| 肤色 / 肤质差异 | per-instance **vec3 tint** 乘共享肤质贴图；或 texture-array 取肤 | tint uniform / texture array | 最省（constant） |
| 「4 副面孔」的五官/肤(外观) | texture-array / 材质变体 + `face_index` | face_index selector | 低（一个 int） |
| 「4 副面孔」的脸型/头骨几何 | **共享 N 个 baked 头部 rest-mesh 变体** + `head_idx` | head_idx selector | 低（一个 int） |
| 衣物不同款式 | **预备 N 套绑骨 rest-mesh 变体** + `cloth_style_idx` | part_pool[:].cloth + idx | 低（一个 int） |
| 衣物/部位颜色微调 | per-part tint / 该部位材质变体 | tint selectors | 低 |
| 装备差异 | 同衣物——rest-mesh/材质变体 + idx | part_pool + idx | 低 |
| 全身随机差异复用 | 每个 instance 只存 `seed`，CPU 确定性派生成上面所有 idx/tint | seed → hash | 省 CPU 且可复现 |

**结论：1000 人 ≈ 一份几何 + 一张材质/贴图变体池 + N 个(int seed, 若干标量 tint/scale)
→ 一次 instanced draw → draw-call 瓶颈解除 + 差异廉价富足。**

---

## 4. 肉体与衣物「归一为同一个 charactor-part 模型」

Danis 收敛点（本文档最重要的一条）：**肉体和衣物的 GPU 数据格式差别不大——都是「数个绑定到
共享骨架的 rest-mesh」传 GPU，instancing 时顶多 scale，然后按骨架姿势 + 模型 index 渲染。**

⇒ **不把「肉体」和「衣物」设计成两套系统，统一成一份「charactor-part 资源池 + per-instance selector」：**

```
共享 part_pool[]（全部 bind 同一骨架，rest 姿态）:
  PART_BODY_HEAD_#, PART_BODY_TORSO, PART_BODY_UPPER_ARM, PART_BODY_LOWER_ARM, …
  PART_CLOTH_TOP_A/B/C, PART_CLOTH_ARMOR_…, PART_CLOTH_PANTS_… , …
```

**肢体拆分门槛（本首屏要能区分的最低粒度，Danis 拍板）**：至少 **小臂(lower arm) / 大臂(upper arm) / 头部** 分部位可拼。不是简约整身，也不必到手指。上臂与小臂在肘关节处能独立选择/形变/换装是 S0 打底的结构需求。

**instance 拼人 = 每个绑骨 slot 一个 part_idx（可为空 = 露肤 / 该部位光着）**：

```
slot:  head  upper_armR  lower_armR  handR  …  torso  top  bottom …
val:   head_idx / 上臂idx / 小臂idx / …        / ... 衣idx …（可 null）
```

绘制时按 `Draw_SlotPart(part_idx)` 把选中的 rest-mesh 用**共享骨架矩阵**蒙皮，随 `transform` 摆放。

---

## 5. JPOV / 用户的边界（沿用 engine-integration 收敛）

| 层 | 归属 | 说明 |
|----|------|------|
| rest 网格上传（`MeshData`/`GPUMesh`/`RegisterMesh`） | **JPOV** | 现有 rest-mesh 顶点管线已就位（含预埋骨骼权重 slot loc 3/4，见 `mesh.h`/`gpumesh.h`）|
| 材质 / 贴图变体（`PBRMaterial` + texture-array）| **JPOV** | 肤色 tint、衣物颜色 = 现有材质可表达，不另造 |
| 蒙皮 shader（VS 按共享骨架矩阵 skin 位置/法线/切线）| **JPOV** | 复用 rest 上传 + 共享矩阵；下游（光照/阴影/picking/tone）走现有链 |
| **共享骨架矩阵的每帧求值**（clip 采样 → 一套关节矩阵）| **用户 / 骨架系统** | 跟「世界演化归用户」同归一类；JPOV 消费它 |
| **part 资源组织的资产侧**（哪些 part 存在/人物长啥样）| **用户侧资产/装配** | selector 由用户的「人物装配」给出 |
| batch / instancing 的**组织**（谁把 N 个 instance 合成一次 draw）| **JPOV**（render 关心） | 见 §8 |

边界一句话：**「画什么」的差异（assembled part + transform + tint/seed + 已求值的骨架矩阵）
由用户给，JPOV 只负责把它高效地 batch 起来蒙皮画出来——和 engine-integration doc 的 cmds 缝同构。**

---

## 6. 两个已知取舍 / 明确不开的门（防返工）

### 6.1 「衣物绑定共享骨架 vs 按体型微调」的冲突（第一坑，最容易咬人）

衣物 rest-mesh 是按**标准骨架**绑的。若受限于「每 instance 廉价体型标量（肩宽/高矮）改身体」，
**衣物的 rest mesh 不会跟着适配** → 宽肩穿窄衣豁、衣物与身体在关节错位。
处理分三档，**起步必须选第一档**，二、三延后：
1. **廉价体型差异只做全局/轴向 scale**：整体或单轴放大缩小，衣物跟着 scale 一起缩放仍贴合。
   这是「廉价形变(scalar) + 衣物贴合」两全的最稳档。 ✅ S0 用
2. 衣物 bake 成身体变体（每变体一套衣）——复杂，延后。
3. 接受「衣带骨、体可微调但衣物按标人」的近似——文档标注取舍，若需要再开。

### 6.2 「风动物理」与「逐 instance 独立动画」的便宜分档（第二坑）

- 风：**便宜档** ✅(TODO) = per-instance 标量（风速/风向 seed），shader 对「衣物蒙皮后顶点」做廉价
  正弦/摆动偏移（不等于真实布料，无独立物理）；**贵档** = 真实布料/裙摆逐 instance 解算 → 撞
  「instance 不得 carry 独立形变」边界，**延后**。
- 独立动画：千人共享 skeleton/clip → CPU 求值共享矩阵，廉价。真正「每个 instance 自己动自己的
  clip / 独立绑骨」→ **GPU 角色实例化 / compute skinning**（尤其配合逐 instance 独立体型时），
  明确是更大的机器，**延后**，不做进首屏。

### 6.3 肤色/衣服颜色、4 副面孔 → 全部走「index/tint select 共享区」，见 §3 表。

---

## 7. 里程碑阶梯（S0 → S8，可独立验证、可拼合）

> 沿用 JPOV UI 计划/engine-integration 的「每步单帧渲染自证 gold + 交互 demo」方法论，
> 每步是**能独立落地的里程碑**，不是模糊的想法。

- **S0 — 千人 instancing + selector 跑通**（本首屏核心）：
  共享骨架；部位拆分到 **小臂/大臂/头部** 档；肉体+衣物的 part_pool 以 rest-mesh 上传；
  N 个 instance(part_idx×几 + transform + seed) → 一次 instanced/batch draw；
  **确定性 gold**（同 seed 同画）。
- **S1 — 人物装配 / 差异**：seed → 派生肤色/长相/衣物；texture-array / 材质变体 + tint；4 副面孔(baked head 变体)。
- **S2+（本项目 TODO / 后续）**：
  - **颜色微调**（肤色/衣物部分 mesh 色）正式化 → per-part tint / 变体（§6 分档）。
  - **风动物理廉价档**（shader 标量摆动）（§6.2 便宜档）。
  - （更远）独立动作实例化 / bindless / compute skinning / 真实布料 —— §6 已列为**明确不开的门**或大机器。

（本首屏 =「千人 instancing + selector」真正落地 + rest-mesh 蒙皮通路 + 部位分区到臂/头；
「风和色」明确 TODO，不进 S0。）

---

## 8. 落地时 JPOV 侧要新增的 render 能力（给后续实现 agent 的指路）

现状缺口照前几轮核查（已确认，非猜测）：
1. **实例化 draw**：现 `Object3DCommand` 每个物体独立 `glDraw*`，四 pass 逐 object for 全扫 → 需给
   instanced/batch 语义（新命令或新 pass 组织），且 **shadow/picking/highlight 都要一致处理蒙皮后的
   instance 几何**（避免「身体对、影子/picking 对不上」的幽灵 bug）。
2. **共享骨架矩阵 uniform/UBO**：现无「每帧喂一套关节矩阵」路径 → 需新增（进现有 shader，rest=单位阵时零回归）。
3. **蒙皮 shader**：现 VS 没读 mesh 预埋的 loc3/4（joint/weight）→ 加官方蒙皮 VS；蒙皮后法线/切线形变跟上。
4. **texture-array / 材质变体 / per-part tint** 的组织。
5. 复用现有 rest-mesh 上传（MeshData/GPUMesh 骨骼权重已预埋），不重造顶点管线。

（具体到每一条的 S 阶梯归属、验收 gold 怎么写，留待实现 PR 时按本文档排。）

---

## 9. 一句话给下个 agent

> 这类 Cities 人群不是「给 1000 个高精角色各自跑复杂动画」，而是
> **「一份共享骨架 + 一张肉体/衣物统一 charactor-part 的 rest-mesh 池 + 上千个(part selector×几 +
> transform + seed/标量)的薄 instance」，靠 instancing 一次批掉 draw-call；肤色/长相/衣物/体型全走
> select-共享区 的廉价索引机制；风与颜色微调是 TODO（分便宜档/贵档，便宜档起步）。**
> 肢体至少分到 小臂/大臂/头部；日本首屏 S0 聚焦「千人 instancing + selector + rest-mesh 蒙皮
> 通路」跑通，风/色/精细 rig/独立布料/GPU 独立角色动画 全部明确延后。
