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
- **人群动画走「骨骼动画纹理」（骨骼烘焙蒙皮），作为 JPOV 官方渲染原语**：把角色的骨骼矩阵（非顶点）按
  clip→帧烘焙进一张纹理，instance 只送「当前帧号/相位」，VS 查表 + 保留 4-bone 蒙皮。CPU 每角色求 pose + 每帧传调色板
  **两个开销从 CPU 消失**，CPU 每帧只每人传一个小 instance(相位)。显存只随**骨数×帧**涨、**不随顶点数涨**。
  这一档（非纯 VAT）保住骨骼蒙皮语义，因此与 charactor-part / 换装部位兼容（§4）。
- **纯 Vertex-VAT（顶点×帧位置烘焙）不做为人群主线**，只做为**更远距离 LOD**可选档：它连 4-bone 蒙皮都省、
  CPU 最省，但丢骨骼语义且显存随**顶点数×帧**涨，不适合要「换装部位骨架」的人群主体（见 §2.2）。
- **真·运行时蒙皮（实时骨骼）只留给近端少数主角**，不用于人群主体（CPU 逐角色求值与逐实例调色板
  无法支撑千人 instancing）。三分档详见 §6.2。
- **LOD 归用户、与 instancing/蒙皮解耦**：高模/低模各自一档 instanced draw、喂不同几何；谁切 LOD、每 instance
  归哪档全由用户可见性系统决定，JPOV 只消费「一批同档 instance + 一份 mesh」批量画掉（§5）。
- **S0 边界**：S0 =「1000 个**全低模**（考究做到人均 <1000 三角形 / 按独立 VS 顶点算更紧）人的 instancing +
  selector + 部位到臂/头 + **骨骼动画纹理蒙皮** + 外观差异跑通，相机保持街道/俯拍不带贴脸」；身体高模与
  距离 LOD 切换列 S1+。**风动物理与颜色微调列为 TODO（§7），不进 S0。**

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

下列**明确不是本首屏（S0）目标**，延后（避免首屏做太重、方向被画质/实时骨骼牵走）：
- 单个角色的高模级几何精细度、贴脸近景（想看清脸的镜头体验延后到高模/LOD 里程碑）；
- **真·运行时蒙皮（实时骨骼、IK/状态机/表情）逐 instance 铺到人群** → 只留近端少数主角，人群主体用骨骼动画纹理(§6.2)；
- bindless 纹理 / compute skinning / GPU 驱动逐 instance 独立实时动画（更大的机器，见 §6.2）；
- 真实布料 / 裙摆逐 instance 物理解算（§6.4 便宜档延后）；
- 纯 Vertex-VAT 不做人群主体，只留给更远 LOD，详见 §6.2。

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
  cheap_body_scale   —— 几档全局/轴向 scale 标量（身高胖瘦微差，见 §6.1）
  face_index         —— 肤色/五官外观 → texture-array / 材质纹理变体索引
  tint (vec3)        —— 肤色 / 衣物颜色微调（廉价乘子）
  seed (int)         —— 派生上面各 selector 的确定性随机源（同 seed 同长相，可复现）
```

### 2.1 关键收益

画「一个穿了制服的宽肩瘦男」与画「一个光膀子壮汉」，CPU 只多传几个 int selector，
几何与蒙皮矩阵**全共享**。千人 = 反复 select 共享 part + 一次蒙皮共享骨架，
刷屏仍是受限次数的 instanced/batch draw —— **draw-call 卡脖子当场解除**。

### 2.2 人群主体的动画形态：骨骼动画纹理（关键分岔，本文档已定调到 §6.2）

> 早期版本此处写「CPU 跑一个 clip 采样器、算出共享骨架矩阵上传一次」，经调研后已**弃用**；

人群「上千人有差异 + 能换装部位」不能靠：
- ❌ **逐 instance 在 shader 自采样实时骨骼**——CPU 逐角色求 pose(50~200µs/个，线性随人数涨) + 逐实例矩阵调色板
  （成百上千 mat4/实例无法塞 instancing per-instance attribute）双开销，千人群 CPU 先死 GPU 没醒。
- ❌ **纯 Vertex-VAT(顶点位置帧烘焙)** 作人群主体——丢骨骼蒙皮语义，顶不掉你要的换装/部位骨架设计；且显存
  随**顶点数×帧**涨(高模爆炸)。

✅ 正确主干 = **骨骼动画纹理（texture-palette skinning）**：把角色的**骨骼矩阵**（非顶点）按 clip→帧烘焙进
一张纹理，instance 只送当前帧号/相位，VS 查表得到各关节矩阵后仍做标准 4-bone 加权蒙皮。它同时：
  - 把 CPU 「逐角色求 pose + 逐帧传调色板」两大开销从运行期拿走（矩阵常驻纹理、GPU 自取）；
  - 保住 4-bone 蒙皮语义 → 与 charactor-part 部位选择 / 服装绑定共存（§4）；
  - 显存只随**骨数×帧**涨（骨骼数 ≪ 顶点数），与顶点数解耦。
帧号 CPU 直传 per-instance，配合 instancing 千人批量。三分档（实蒙皮/骨骼纹理/纯VAT）与取舍见 §6.2。

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

> 注：本节边界在「人群通过骨骼动画纹理(§6.2-B)驱动」的主线下重新表述——「动画资产成帧烘焙」与「可见性/LOD 决策」
> 归用户/资产，JPOV 拥有的是**烘焙结果的 GPU 形态 + 4-bone 蒙皮 shader + 批量 instance 绘制**。

| 层 | 归属 | 说明 |
|----|------|------|
| rest 网格上传（`MeshData`/`GPUMesh`/`RegisterMesh`） | **JPOV** | 现有 rest-mesh 顶点管线已就位（含预埋骨骼权重 slot loc 3/4，见 `mesh.h`/`gpumesh.h`）|
| 部位 / 变体 mesh 池与材质贴图变体（`PBRMaterial` + texture-array） | **JPOV** | 肤色 tint、衣物颜色、部位变体 = 现有材质/mesh 体系可表达 |
| **骨骼动画纹理资源**（烘焙出的 bone 矩阵×clip→帧 纹理）| **JPOV** | 作为 JPOV 官方渲染资源类型（与 mesh/材质/texture 平级）持有、管理生命周期 |
| **消费该纹理的蒙皮 shader（VS 查表 + 4-bone 蒙皮）** | **JPOV** | 复用 rest 上传；shade/shadow/picking 各 pass 用同一查表蒙皮保证 4-pass 一致（防“手动/拾取错位”）|
| **把一堆 instance(相位+selector+transform…) batch 成 instanced draw** | **JPOV** | render 关心：接收“同 mesh+同 anim 一批 instance + 帧号”，一次批量画 |
| **动画资产是否/怎么烘焙**（clip 从何而来、传几帧进内纹）| **资产/用户** | 跟「素材归用户」同归一类；JPOV 消费烘焙好的纹理 |
| **每 instance 处在哪段动画 / 哪个相位 / 谁是哪个人**（世界状态）| **用户 / 骨架系统** | 同“世界演化归用户” |
| **LOD：切不切、每 instance 归哪档、高模/低模各自一档 draw 喂不同几何** | **用户（可见性系统）** | JPOV 不必知道“几个 LOD 档”；JPOV 只需“给一批同档 instance + 这份 mesh 批量画”。**与 instancing 蒙皮彻底解耦** 见 §6.3 |

边界一句话：**“画面差异”（装配的部位 + transform/tint/seed +每 instance 相位）与“可见性/LOD/动画来源”由用户给；
JPOV 拥有“烘焙动画纹理资源 + 4-bone 蒙皮 shader + 把同档同动画一批 instance batch 画掉、且四 pass 一致”的渲染职责。**
和 engine-integration doc 的 cmds 缝同构。

---  

## 6. 已知取舍 / 明确不开的门（防返工）

### 6.1 「衣物绑定共享骨架 vs 按体型微调」的冲突（第一坑，最容易咬人）

衣物 rest-mesh 是按**标准骨架**绑的。若受限于「每 instance 廉价体型标量（肩宽/高矮）改身体」，
**衣物的 rest mesh 不会跟着适配** → 宽肩穿窄衣豁、衣物与身体在关节错位。
处理分三档，**起步必须选第一档**，二、三延后：
1. **廉价体型差异只做全局/轴向 scale**：整体或单轴放大缩小，衣物跟着 scale 一起缩放仍贴合。
   这是「廉价形变(scalar) + 衣物贴合」两全的最稳档。 ✅ S0 用
2. 衣物 bake 成身体变体（每变体一套衣）——复杂，延后。
3. 接受「衣带骨、体可微调但衣物按标人」的近似——文档标注取舍，若需要再开。

### 6.2 人群角色的动画：三档谱系（本文档核心取舍，决定人群主体形态）

Danis 拍板方向：以**「骨骼动画纹理」（骨骼烘焙蒙皮）**为人群主体。完整谱系如下，供实现时选档（结构化实现细节不在此列）：

| 档 | 运行时怎么动 | CPU/帧×角色 | 换装部位/骨骼语义 | 能千人 instancing | 显存（最相关） | 定位 |
|----|------|------|------|------|------|------|
| **C. 真·运行时蒙皮（实时骨骼）** | CPU 每角色求骨骼 pose + 矩阵调色板，VS 蒙皮 | ~50–200µs/人，线性涨 | ✅ 完整（blend/IK/表情自由） | ❌（调色板 per-角色，无法批量） | 随 clip 长，不随顶点 | **只留近端少数主角**（要反应/IK/表情），不做人群主体 |
| **B. 骨骼动画纹理（texture-palette skinning，人群主体 ✅）** | 骨骼矩阵按 clip→帧烘焙进纹理；instance 只送帧号；VS 查表得关节矩阵 + 标准 4-bone 蒙皮 | ≈0（只传 per-instance 相位）| ✅ 保留 4-bone 蒙皮 → 与 charactor-part 部位/换装兼容 | ✅ | **骨×帧**（骨数≪顶点数，不随顶点张） | 人群主体；保住换装骨架语义 + CPU 开销压掉 + 千人批量 |
| **A. 纯 Vertex-VAT（顶点位置 x 帧烘焙）** | 顶点每帧位置打纹理；instance 送帧号；VS 查表直接 lerp 出顶点，无蒙皮权重 | ≈0（最省） | ❌ 丢骨骼语义（顶点直接给位置） | ✅ | **顶点×帧**（高模/长动作爆显存） | **不作人群主体**；作为**更远距离的降级 LOD** 可选档（远景低模无所谓，丢骨骼没关系） |

**为什么 B（骨骼动画纹理）是人群主体而非 A/C**：
- 对 A（纯 VAT）：你要的「小臂/大臂/头分部位 + 服装绑共享骨架」全部建立在**骨骼蒙皮语义**上，纯 VAT 把顶点直接烘焙成位置会丢这套换装地基；且 A 显存随**顶点数×帧**，高模/多 clip 会顶 8K 纹理/大显存。
- 对 C（实时蒙皮）：CPU 逐角色求 pose + 逐实例调色板，千人时 CPU 线性爆（50–200µs/人 → 千人 ≈ 几十~百 ms/帧），且调色板无法 instancing——正是人群要甩掉的两样。

**A/B/C 之间的转换**：B、C 共享「骨骼 → 蒙皮」语义，只在「pose 从哪来」（GPU 查烘焙纹理 vs CPU 实时算）不同；
因此 C→B 只需把动画烘焙成药帧纹理、网格/权重/换装/蒙皮 shader 全部复用。A 与 B/C 不共享骨架语义（A 是静态网格+查位置），是真正另一条通路。

**接受的门**：本谱系把「每个 instance 自己动自己的实时骨骼动画」（C 的任意 blend/IK 用在 1000 人身上）明确留为
**更大的机器**（需 compute skinning / GPU 角色实例化，近端少数可用 C；不把它铺到全体人群），接受换密度、牺牲自由实时组合。

**已知坑（实现时必看）**：B/A 的姿势来自「GPU 查烘焙纹理」，故**阴影/picking/高亮各 pass 若需要对蒙皮后几何做正确判定，
必须让这些 pass 的 VS 也用同样方式查表**（否则身体动、影子/拾取对不上 → 幽灵 bug）。这正是 JPOV 统一做这些原语而非
泄给用户 shader 的理由（见 §5）。

### 6.3 LOD 归用户、与 instancing/蒙皮解耦（Danis 拍板）

**LOD 的本质是「这一档用哪份几何」，与「怎么 instance / 怎么蒙皮」无关。** 正确做法:
- **按 draw call 分档**：高模一档 instanced draw、低模另一档 instanced draw，各自喂不同 mesh；每档内部仍对
  该档 instance 批量。所谓“几个 LOD 档”只是几次 draw（非 CPU 卡死），每档仍一次 batch。
- **归属**：要不要切 LOD、每个 instance 归哪档、切档策略 → **用户可见性系统**（相机距离/重要性，本质归
  “世界/相机管理”，与动画归用户同辙）。**JPOV 不必知道共有几个档**，只需“给一批同档 instance + 这份 mesh
  一次 batch 画掉”。
- 实例数量档：真正引擎还会 cull 视锥外/被遮 instance、极远景用 impostor（一张翻转 2D 图）当一次 sprite，
  根本不占 3D VS 预算——属用户可见性层可选优化，不进 JPOV 渲染语义。

因此 **LOD 决策不放进 JPOV**；JPOV 把「一批同档同动画 instance → 一次蒙皮 batch draw」当原子能力，用户爱切几档切几档。
（业界同构：NVIDIA 2007 即是“每 LOD 组 → 每 submesh → 一条 instanced draw”。）

### 6.4 风物 / 颜色微调（统一短记）

- 风：**便宜档** ✅(TODO) = per-instance 标量（风速/风向 seed），shader 对蒙皮后顶点做廉价正弦摆动（非真布料）；
  **贵档** = 真实布料/裙摆逐 instance 解算 → 延后。
- 颜色 / 4 副面孔 / 衣物 → 全走「index/tint select 共享区」，见 §3 表。

---

## 7. 里程碑阶梯（S0 → S8，可独立验证、可拼合）

> 沿用 JPOV UI 计划/engine-integration 的「每步单帧渲染自证 gold + 交互 demo」方法论，
> 每步是能独立落地的里程碑。**结构化实现细节（结构体/接口）不在此列，另开实现级文档。**

- **S0 — 人群首屏：全低模 + instancing + selector + 骨骼动画纹理跑通**（本首屏核心）：
  - 部位拆分到 **小臂/大臂/头部** 档；肉体/衣物 part_pool 以 rest-mesh 上传，全部 bind 共享骨架。
  - **人群主体驱动 = 骨骼动画纹理（§6.2-B）**：把数段 clip（walk/idle 起步）按骨架烘焙成 anim 纹理；
    N 个 instance(part_idx×几 + 相位 + transform + seed) → 一次 instanced/batch draw。
  - **镜头保持街道/俯拍，不带贴脸近景**（屏幕不需要高模怼脸）。
  - **S0 锚点：1000 位**，全部用**考究低模**（非 placeholder）：人均独立 VS 顶点预算按**有效三角形 <1000 /
    独立顶点量级 ≤2.5k** 严格控制（低模质量决定 silhouette 不露馅，是 S0 的艺术重点而非后补件）。
  - **确定性 gold**（同 seed 同画）。
- **S1 — 人物装配差异 + LOD 门**：seed → 派生肤色/长相/衣物；texture-array/材质变体 + tint；4 幅面孔(baked head 变体)；
   **LOD 解耦落地**：高/低模各一档 instanced draw、由用户可见性系统按距离/重要性切换（近端才上高模与真蒙皮 C 档）。
- **S2+（TODO/后续）**：颜色微调（per-part tint，§6.4）、风动物理便宜档（§6.4）、更远距离 VAT/Impostor LOD、
  bindless / compute skinning / 真实布料（§6.2 明确不开的门/大机器，延后）。

（本首屏 =「1000 全低模人 instancing + selector + 部位到臂/头 + 骨骼动画纹理驱动 + 外观差异」跑通；
相机不贴脸；风/色/高模/距离 LOD 切换/实蒙皮主角 明确 S1+ 或更后。）

---

## 8. 落地时 JPOV 侧要新增的 render 能力（给后续实现 agent 的指路）

现状缺口照前几轮核查（已确认，非猜测）：
1. **实例化/batch draw**：现 `Object3DCommand` 每个物体独立 `glDraw*`，四 pass 逐 object for 全扫 → 需给
   instanced/batch 语义（新命令或新 pass 组织），**shadow/picking/highlight 都对“同档同动画一批 instance”一致处理**
   （避免“身体对，影子/拾取错位”幽灵 bug）。
2. **骨骼动画纹理资源（§6.2-B）**：JPOV 新增“烘焙骨骼动画纹理”资源类型（mesh×clip→帧的矩阵纹理），
   管上传/生命周期/采样坐标。读它的 VS 做 4-bone 蒙皮。rest 姿态/单位阵时应退化为现有静态不带蒙皮的 VS
   （零回归门）。
3. **官方蒙皮 shader（查 anim 纹理 + 4-bone）**：现 VS 没读 mesh 预埋 loc 3/4（joint/weight）与 anim 纹理 →
   加官方蒙皮 VS；蒙皮后法线/切线形变跟上；**阴影/picking pass 用同一查表保证蒙皮后几何一致**。
4. **texture-array / 材质变体 / per-part tint / seed-派生 selectors** 的组织（供外观差异用）。
5. rest-mesh 上传（`MeshData`/`GPUMesh`）已含骨骼权重预埋，不重造顶点管线；关节矩阵由 anim 纹理（非 UBO）驱动。

（具体到每条 S 阶梯归属、gold 验收怎么写，留待实现 PR 按本文档排。结构体/接口细节不在此列。）

---

## 9. 一句话给下个 agent

> 这类 Cities 人群不是「给 1000 个高精角色各自跑复杂（实时骨骼）动画」，而是
> **「一份共享骨架 + 一张肉体/衣物统一 charactor-part 的 rest-mesh 池 + 上千个(part selector×几 + 相位 +
> transform + seed/标量)的薄 instance；动画走骨骼动画纹理(矩阵×clip→帧烘焙，instance 只送帧号)保住 4-bone 蒙皮语义，
> 一次 batch draw 掉 draw-call；肤色/长相/衣物/体型全走 select-共享区 的廉价索引；LOD 归用户、与蒙皮解耦（高/低模各一档
> draw）；纯 VAT 只留给更远 LOD；真·实时蒙皮只留近端主角」**。
> 肢体至少分到 小臂/大臂/头部；首屏 S0 = 1000 全低模（人均有效三角形<1000 / 独立顶点≤2.5k）、镜头不贴脸、
> instancing + selector + 骨骼动画纹理驱动跑通为锚；风/色/高模/距离 LOD/实蒙皮主角 明确后置。

---

## A. 参考文献（本结论的业界来源；如需深挖或验证按此查阅）

> 以下为 2026-09-03 与 Danis 架构推演时实际调研到的权威出处，按用途分组。供后续 agent 在实现 / 下钻时
> 直接引用，避免把已参考的结论当“新发现”重推一遍。

### 人群 / Instancing（“CPU 是瓶颈、骨骼 mesh 不为百人以上设计”的直接依据）
- **Epic 官方（City Sample 人群方案）**: 只 ~几个近处 full actor(简化骨架)，其余全 Mass/static mesh + 顶点动画动态过渡。
  （见 Epic 论坛 metahuman 回复，2026 — “骨骼 Mesh 不是为 100+ components 规模设计的”)；性能技巧节(VAT crowd / AnimToTexture)。
  - https://dev.epicgames.com/community/learning/knowledge-base/xBZp/unreal-engine-performance-tips-tricks-animation
  - https://forums.unrealengine.com/t/metahuman-skin-cache-retains-lod0-entries-for-every-npc-regardless-of-camera-distance/2728166
- **NVIDIA GPU Gems 3（skinned instancing / texture-based 骨骼蒙皮）**: 2007 的硬件 9500+ 独立动画角色/34fps；
  “每 LOD 组→每 submesh→一条 instanced draw”，材质变化用贴图 alpha tint + texture array —— 我们 §6.2-B 与 §6.3 的直接出处。
  - https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-2-animated-crowd-rendering

### VAT / 骨骼动画纹理（三分档技术细节与成本对比的直接依据）
- **Mighty Professional Tutorials「Vertex Animation Textures」**: 最详尽的 VAT 机制（烘焙、纹理格式、与骨骼 CPU 成本 50–200µs/人
  对比、三档 CPU/内存/draw 差异表、并引 Epic City Sample ~10,000 人几十 draw）。本文档 §6.2 CPU/显存论点主要来源。
  - https://mightyprofessionalgaming.com/tutorials/vertex-animation-textures.html
- **OpenVAT**: Blender 原生的 VAT 工具链（烘焙/offset 编码/顶点采样/mesh merge），用 WPO(per-vertex world offset) 更稳。
  - https://openvat.org/
- **Houdini Labs「Vertex Animation Textures」**: VAT 的 Houdini 烘焙/导出、soft/rigid 模式、CPU 负担更轻、局限（无动画碰撞等）。
  - https://www.sidefx.com/docs/houdini/nodes/out/labs--vertex_animation_textures-3.0.html
- **Epic AnimToTexture**: 官方烘焙顶点动画插件（City Sample 用）。
  - https://dev.epicgames.com/community/learning/tutorials/daE9/unreal-engine-baking-out-vertex-animation-in-editor-with-animtotexture
- **GPU Instancer CrowdAnimations(GurBu)**: Unity 侧烘焙骨骼、compute/间接 instancing + GPU culling 的结合范例（Mecanim 拖后腿 vs 直接用 clip）。
  - https://wiki.gurbu.com/index.php?title=GPU_Instancer:CrowdAnimations
- **chenjd Render-Crowd-Of-Animated-Characters（Unity,1.7k★）**: 动画烘焙成图+GPU instancing，10000 士兵 20 多个 draw。
  - https://github.com/chenjd/Render-Crowd-Of-Animated-Characters

### 工程基础（CPU/GPU skinning 成本、骨骼 mesh 上限的推算）
- **radiac/game-mechanics-optimizations §53 GPU Skinning**: CPU skinning 百个 5k-vertex ×50-bone 的数学（vs 1.5B transforms/帧 → 60ms 超预算）；
  Doom2016/Horizon/Spider-Man/AC:Unity 等的 GPU skinning 数字。
  - https://github.com/raduacg/game-mechanics-optimizations/blob/main/53_gpu_skinning.md
- **Unity 论坛/资料（SkinnedMeshRenderer 可变形 mesh: 骨骼+blend shape+cloth；CPU 动画在某规模最贵）**:
  - https://gameoptim.com/blog/post/GPUSkinning
  - https://docs.unity3d.com/Manual/class-SkinnedMeshRenderer.html
- **Khronos CPU/GPU skinning 讨论**: matrix-in-texture 只送 time 的可行性与限制（帧烘焙/在纹理查骨）。
  - https://community.khronos.org/t/skinning-on-the-gpu-vs-the-cpu/73169
