# JPOV 部位额外旋转（partial rotation）— 设计 / 实现

> 目的：给「拉弓射箭 / 举枪瞄准」这类动作补一条**骨骼级整体转动** —— 例：把整个上半身绕腰
> 转一个角度（**扭腰**）、把头再额外抬/低（**仰头**）。与 thickness（`jpov_crowd_body_shape_face_design.md`
> §3）同构：**骨架级配置两个关节名 + 每实例给两个模型系四元数**，per-instance attribute 上传，
> 蒙皮 VS 逐骨施加，不新增顶点属性、不改骨架拓扑、不重烘 pose atlas。
>
> 状态：本文是**实现稿**（2026-09-26 与 Danis 需求收敛，PR 见下）。对应代码：
> `interface/skeleton_types.h`（`kNumPartialRotation` / `SkinnedInstanceState::partial_rotations`）、
> `src/skeleton/skeleton_manager.{h,cc}`（`partial_rotation_config` + 通道表纹理）、
> `src/skeleton/skinning_shader.h`（主/阴影 VS 前乘 DQ）、`demo/fbx_viewer/`（扭腰/仰头滑条）。

---

## 0. 结论速览

| 需求 | 落点 | 机制 | 每实例成本 |
|---|---|---|---|
| **扭腰**（上半身整体转） | 骨架级定义 + 实例级取值 | 通道 0：pose 后绕腰关节**当前位置**、模型系旋转 | 1×vec4 |
| **仰头**（头再额外抬/低） | 同上 | 通道 1：pose 后绕头关节**当前位置**、模型系旋转 | 1×vec4 |

三条设计主线：

1. **参数定义在模型系**（不是骨局部系）：用户只面对「面朝 +X、上为 +Y」这一个坐标系，不必
   关心各骨 bind 朝向 / 局部帧怎么摆（Danis 2026-09-26 定的用户友好性）。
2. **波及范围 = 该关节的整个子树**（descendants）：pose atlas 里没有层级信息（每骨只存
   `jointWorld(pose)·inverseBind`），故**波及范围在构造期由 manager 从骨架树导出**、烘进一张
   每骨通道表纹理；VS 侧据此对**每根骨**做前乘 ⇒ 子树整体转、边界按权重平滑过渡。
3. **零回归靠 host 开关，不靠"乘单位元"**：未配通道 / 本批所有实例旋转都是恒等 ⇒ 蒙皮 VS
   整段跳过（逐字节不变）。

---

## 1. 接口（只有两处，与 thickness 同构）

```cpp
// ① 骨架级（构造 SkeletonManager / RegisterSkeleton 时给）：两个**关节名**，每名一条通道。
std::array<std::string, jpov::kNumPartialRotation /* = 2 */> partial_rotation_config;

// ② 实例级（SkinnedInstanceState）：每通道一个**模型系**四元数（默认恒等 = 不转）。
std::array<geom::Quaternion<float>, jpov::kNumPartialRotation> partial_rotations;
```

- **为何用骨名而非 index**：index 是资产内部编号（重导出/换资产就变），骨名才是语义
  （同 `BodyRetarget` 的骨名对位 / fbx viewer 的 `GlbJointIndexByName`）。
- **合法性**在构造时**立即**校验（排在所有 GL 调用之前，便于纯 CPU 死亡测试）：
  空名 = 该通道不用；查不到 / 骨名重复 / 两通道同指一骨 → `LOG(FATAL)`。
- **每骨用到的 pivot 与通道映射不进公开签名**：由 `SkeletonManager` 自己从骨架导出（派生量，
  同 thickness 的「bind 位置/朝向」不进接口）。

### 1.1 R **相对什么、定义在哪个系**（接口语义，先把它说清）

这是本接口最容易被误读的一点，所以写在最前：

- `R` **定义在模型系**（= 骨架根空间，与 rest 顶点 / pose atlas 同一空间；本工程人形资产
  **面朝 +X、上为 +Y**）。—— **不是**相对父关节，也**不是**相对该关节的局部坐标系。用户只需
  面对这一个坐标系，不必去处理骨局部系。
- 顺序是「**先摆 pose、再叠加 R**」：R 作用在该通道关节的**整个子树**上，绕该关节的**当前
  pose 位置**整体转。
- ⇒ 因为 R 的轴在模型系里是**固定**的，它的“解剖含义”取决于该关节**当前被 pose 摆成什么
  朝向**。

**例（TPose 歪头）**：

| 情形 | “仰头”（R = 绕模型 +Z）实际是 |
|---|---|
| TPose：头朝 **+X** | 绕 ⟂ 前向的 +Z 转 = **正常点头（pitch）** |
| pose 已把上身转到头朝 **+Z** | 绕 **自己的前向轴**转 = **歪头（roll）** |

且后一种情形下，再用另一条 partial 把上身扭回 +X 也**救不回来**：嵌套合成是
`G = G_腰 ∘ G_头`（腰是祖先=外层、头是内层），**头的 R 先作用**（那时头还朝 +Z，歪头已产生），
后面的扭腰只是把这个已经歪掉的头整体转回去。

> 要“不管 pose 朝哪、仰头都相对身体点头”（= 把 R 的轴按该关节 bind→当前的朝向差共轭一下）是
> **另一种定义**，本接口**刻意不做** —— 这里选的是“**模型系固定轴**”的清晰定义
> （2026-09-26 Danis 拍板：接口清晰优先）。

---

## 2. 语义与数学

### 2.1 乙：pose 之后叠加（2026-09-26 Danis 定）

通道 `c` 配置在关节 `j_c` 上。`R_c` 是本实例给的**模型系**四元数。语义：**pose 先摆到位，
然后把 `j_c` 的整个子树绕 `j_c` 的当前 pose 位置整体转 `R_c`**（= 在 `jL(j_c)` 末尾额外乘一个
旋转，先 pose、后叠加）。世界效果是一次刚体旋转：

```
G_c = T(pos_c) · R_c · T(pos_c)⁻¹        // 绕「j_c 当前世界位置 pos_c」、按模型系 R_c 转
pos_c = final(j_c) · p_c                 // p_c = j_c 的 bind 位置；final(j_c)=jointWorld·IBM（本帧插值）
```

对某根骨 `b`（其最终蒙皮变换记为 `M_b = jointWorld(pose,b)·inverseBind(b)`，即 pose atlas 那一行）
把 `G_c` **前乘**：

```
M_b' = G_c · M_b
```

⇒ 落在 `b` 上的顶点整体绕 `pos_c` 转；`b` 在子树内就转、不在就不转 ⇒ 跨关节权重混合区自动平滑
（同 thickness 的「每骨算子按权重插值」思路）。pivot 取**当前**位置 ⇒ 无论 `root_offset`/祖先
pose 把 `j_c` 挪到哪，旋转都与身体连在一起（**不会腰斩**）。

> **为何要取 `final(j_c)`**：pivot 必须落在 `j_c` 的**当前**世界位置，而“当前位置”只有通过
> `j_c` 本帧的最终变换才能拿到（`final(j_c)·p_c`）⇒ 通道表里除了 `p_c` 还存 `j_c` 的骨号，VS 去
> pose atlas 取它。这一步就是 Danis 提醒的「坐标系换算」：`R_c` 是模型系量，落到 `j_c` 已 pose
> 的局部帧后，净效果 = 绕 `j_c` 当前位置按**模型系 `R_c`** 转（轴不随 pose 变）。

### 2.2 两通道嵌套（腰 ⊃ 头）

人体里 `Head` 是 `Spine` 的后代 ⇒ 头子树的每根骨**同时**受两通道影响。先转内层（头）、再转外层
（腰）：

```
G_b = G_outer ∘ G_inner          // outer = 更靠根的通道，inner = 更靠叶的通道
M_b' = G_b · M_b
```

`G_outer`/`G_inner` 各自绕**自己关节的当前位置**转。这样头的 pivot 会**跟着腰一起动**（头始终长在
扭转后的脖子上），而不是各转各的。通道表里每根骨存两槽 `(outer, inner)`，按 祖先→后代 排序
（构造期定，见 §3）。

---

## 3. 实现

### 3.1 两张小表（骨架级，构造期烘一次）

- `GpuHandles::partial_rotation_bind_tex` = `bone_count × 1` `RGBA32F`：**每骨一个 texel**
  `(ch_outer, ch_inner, _, _)`；-1 = 无。- 逐骨沿 `joints` 树自底向上走，先遇到的配置关节 = 内层、
  后遇到的 = 外层；单通道时只填槽 `ch_outer`（`ch_inner = -1`）。
- `GpuHandles::partial_rotation_channel_tex` = `kNumPartialRotation × 1` `RGBA32F`：**每通道一个 texel**
  `(p_c.xyz, j_c 骨号)`；未用通道 = 骨号 -1。
- `p_c`（关节 bind 位置）由 `inverse_bind[j_c]` 取逆后的平移得到（**同蒙皮同源**，不另跑一次树遍历）。
- 两张表同生同灭，只在**配了通道**时建（否则句柄 0 = 渲染侧整段跳过）。
- `j_c` 骨号的作用：让 VS 能去 pose atlas 取 `j_c` **本帧**的最终变换，算出 `pos_c = final(j_c)·p_c`
  （§2.1 的当前 pivot）。

### 3.2 每实例上传（per-instance attribute，loc13/14）

`SkinnedInstanceBuffer` 新 `kInstancePartialAttrSpec{base_loc=13, slot_count=2, components=4,
stride=8}`；`SkeletonRenderer::UploadSkinningInstanceAttributes` 把每个实例的 2 个四元数
（xyzw）连续写入。上传时校验：分量有限、`|q|² > 0.25`（近零会让 shader 的 normalize 出 NaN）。
**属性槽**：loc0–5 顶点 / loc6–9 摆放 / loc10 pose / loc11–12 粗细 / **loc13–14 额外旋转** =
15/16（`GL_MAX_VERTEX_ATTRIBS` 保证 16）。

### 3.3 VS 施加（主 pass 与阴影 pass **逐字同公式**）

两片 VS 各持一份同公式（GLSL 无 `#include`；同 thickness 的「主/阴影必须同改」铁律，否则影子
与身体错位）：

```
顶点级（与骨无关）：对每条已配置通道 c，从 uPartialChannel 取 (p_c, j_c)，
   经 LoadBoneDualQuat(j_c) 取本帧插值后的 final(j_c)，算 pos_c = final(j_c)·p_c；
   BuildPartialGDq(c) → dq(G_c) = (R_c, ½·(vg,0)⊗R_c),  vg = pos_c − R_c·pos_c
逐骨：读 uPartialBind[bone] 的 (ch_outer, ch_inner)；若有通道：
   合成 G = G_outer ∘ G_inner（DQ 乘法，单通道时直接取）
   前乘：q ← gq⊗q；t ← gq⊗t + gt⊗q          // 与 geom::math::DualQuatMultiply 同序
```

在「逐骨取本帧 DQ」之后、DQS 混合**之前**调用 ⇒ 混合的是已叠加旋转的刚体变换。

### 3.4 开关（零回归）

host 侧：`uPartialEnabled = (骨架有通道表) && (本批至少一个实例的旋转非恒等)`。
两者任一不满足 ⇒ shader 整段跳过（连通道表都不采样）⇒ 未用本功能的骨架/场景**逐字节不变**。

---

## 4. fbx viewer 验收

- `MakeGlbPartialRotationConfig`：通道 0 = `mixamorig:Spine`（腰）、通道 1 = `mixamorig:Head`（头）。
- 面板滑条（任意「蓝带皮」模式显示，含 TPose/rest 调试）：
  - **扭腰 (通道0 ±90°)** —— 绕**模型上轴 +Y**；正角把朝向前方从 +X 扭向 −Z。
  - **仰头 (通道1 ±30°)** —— 绕**模型左右轴 +Z**；正角把 +X 转向 +Y = 抬头向上。
- headless：`--twist-deg <度>`（±90）/ `--tilt-deg <度>`（±30），与滑条同一份状态。
- ⚠️ 本资产骨架空间**面朝 +X、上为 +Y**（与 `InstanceTransform::up={0,1,0}` 一致）；轴选错会
  变成「歪头 / 卷身」，验收时先看一眼 rest 扭腰是不是**整体绕竖直轴**。

肉眼验收（rest/T-pose，3 实例模式）：

| 操作 | 预期 |
|---|---|
| 扭腰 +90° | 腿脚不动；**上半身（含手臂/头）整体绕竖直轴转 90°** |
| 仰头 +30° | 只有头相对脖子抬起，躯干/腿不变 |
| 两者叠加 | 头先抬、再随上半身一起扭 |
| 动画 + 扭腰 | 动态姿势下上半身仍额外扭，影子跟随（阴影 pass 同公式） |

---

## 5. 明确不做（防返工）

1. **不做「甲」（先 bind 转 partial、再上 pose）那一版**：本实现按 Danis 2026-09-26 定为**乙**（pose 后叠加、绕关节**当前**位置）。
2. **不做每实例逐骨 λ**：通道数固定 2（= Danis 判的「腰 + 头够用」）；要更多通道得先加 attribute
   槽预算（当前 15/16）。
3. **不做非骨语义的空间 mask**：波及范围就是「配置关节的子树」，不做球/椭球遮罩。
4. **不改 `rest_offset` / 不重烘 atlas**：只前乘最终变换 ⇒ 骨架/pose 全部复用。

---

## 6. 门禁

- 配置校验单测（纯 CPU，死亡用例）：`test/jpov_partial_rotation_config_test.cc`
  —— 骨名查不到 / 重复 / 两通道同指一骨 → FATAL；实例默认四元数 = 恒等。
- 零回归：既有 fbx 组合拳 gold（`fbx_skinned_instanced_thickness_1280x720.png`）**逐字节不变**
  （viewer 骨架虽已配通道，但实例旋转默认恒等 ⇒ 开关关闭 ⇒ 整段跳过）。
- 视觉门禁：fbx viewer 扭腰 / 仰头滑条 + 出图（§4）。
