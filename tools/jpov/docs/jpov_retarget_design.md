# JPOV Retarget — 骨骼动画重定向子系统（设计）

> 目的：让「外部动画源（FBX/动作库）」能正确驱动「任意带骨 mesh（Tripo 生成 / 手建）」。
> 本文只做**设计**，不含实现。实现按 §7 的里程碑分 PR 落地。
>
> 背景与踩坑全过程见 `memory/2026-09-10-jpov-skinning-math.md`（概念对齐 + M 缺失发现）。
> 本文是 Danis 2026-09-10 晚口述方案 + 业界对比后的定稿设计。

---

## 0. 问题陈述（已实测确认）

`--fbx` 播动画的 M1 尝试（分支 `feature/20260910-jpov-model-viewer-upgrade`）**已判定作废**：
动画在动，但**姿态全乱**（四肢朝向错乱，非可辨人形）。本次只留设计，代码分支不管。

### 0.1 根因（逐条实测）

| # | 发现 | 证据 |
|---|---|---|
| 1 | **不是 shader bug** | 烘焙数学 `T(rest)·R(pose)`×`inverseBind` 无误，M1 gold diff=0 已证 |
| 2 | **`mixamo_male.glb` 不是 Mixamo 官方导出** | `asset.generator="Tripo"`，节点名 `tripo_node_...` |
| 3 | **Mixamo 只统一【骨名】+【骨数/拓扑】** | 同名骨骨长比从 90x 到 240x 不等（见下表） |
| 4 | **`SkeletonType` 是资产相关的，无法对应 Mixamo** | 不存在可直接用的"Mixamo 标准骨架" |
| 5 | **rest 朝向是主因，不是 `M`** | glb 的 `LeftArm` rest=179.3°、`LeftShoulder`=115.4°；FBX rest≈恒等 |

同名骨骨长对比（FBX vs glb 实测）：

| 骨名 | FBX | glb | 比值 |
|---|---|---|---|
| Hips | 104.27 | 0.534 | 195x |
| Spine | 10.18 | 0.051 | 200x |
| Spine1 | 10.00 | 0.072 | **139x** |
| Neck | 16.87 | 0.072 | **234x** |
| RightArm | 10.84 | 0.121 | **90x** |

→ **比值本身就不整齐**，说明不只是单位差，而是**骨长比例完全不同**。

### 0.2 概念澄清（本次定稿）

- `local(j) = T(rest_offset[j]) · R(joint_rotation[j])` —— **先平移、后旋转**
- **旋转不参与自己的定位**：位置只由 `rest_offset` 决定；`R` 只决定**朝向**与**子骨方向**
- **朝向(j) = 从根到 j 路径上所有 `R` 的连乘**（与 `rest_offset` 无关）
- `rest_offset` 在**父关节系**；根的 `rest_offset` 在**原父系**
- **`root_offset` 当前未接线**（`skeleton_manager.cc` 零引用）→ 播不了 root-motion
- 顶点 joint 信息 = **4 对 (索引 uint8, 权重 float)**，和恒 1；`POSITION` 是 rest 坐标

---

## 1. 设计原则

### 1.1 基准是 `SkeletonType`，不是某个资产

**这是本设计的核心判断**（Danis 定，且优于业界常见做法）：

> retarget 的基准**不能是某个 fbx 或 mesh**，而是 **`SkeletonType` 本身**。
> 一个 `SkeletonType` 定了之后，它会面向**一系列动作和 mesh**。

对比业界：Unity 要求**每个humanoid资产手动配一个 Avatar**（`Create from this Model`），
每个 Avatar 是一次人工映射。JPOV 以 `SkeletonType` 为基准，**避免"每资产一 Avatar"的重复劳动**。

### 1.2 辅助姿态（auxiliary pose）显式建模

业界标准做法（`retargeting-threejs` / sketchpunk 算法，Unity Avatar 同源）：

> Given two skeletons, an animation can be approximately retargeted using an
> **auxiliary pose shared by both skeletons**.

```
Retarget 核心式：
  target_local_rot = (目标父骨 bind 世界旋转)⁻¹
                   · (源父骨 bind 世界旋转)
                   · 源 local 旋转
                     └──────────────┬──────────────┘
                      这一段就是"rest 朝向差"的补偿（共轭）
```

**JPOV 的辅助姿态 = Mixamo 标准 T-pose 布局**（见 §2）：

```
aux_pose:
  - 骨长轴  = +Y
  - 骨 up   = +Z
  - rest 旋转 ≈ 恒等
```

**源（FBX）和目标（Tripo/手建）各自跟 `aux_pose` 算一次对齐变换**，
retarget = `目标对齐⁻¹ · 源对齐 · 源旋转`。

→ **新资产接入只需提供"它相对 aux_pose 的对齐参数"**，不必两两做 retarget。

### 1.3 人工校准优先，AI 看图不可靠

Tripo 资产的骨架规范化（§4）**人工做更合适** —— 美术活，做一次固化。
AI 视觉能力在"骨架与 mesh 是否贴合"这类精细判断上不可靠。

---

## 2. 骨架约定（Mixamo 布局）

### 2.1 约定内容

| 项 | 约定值 | 依据 |
|---|---|---|
| **骨长轴**（bone direction / length） | **+Y** | FBX 实测：`rest_offset` 几乎全 `(0,+L,0)` |
| **骨 up / roll 轴** | **+Z** | 业界惯例（⚠️ FBX 数据**无法证明** —— Mixamo 不把 rest 旋转存进 FBX） |
| **rest 旋转** | **≈ 恒等** | FBX 实测 |
| **原父系** | 根关节位于 `(0,0,0)`，直接 +Y 生长 | 本设计约定（见 §3） |

> ⚠️ **up 轴 +Z 是业界惯例推断，非实测**。若后续发现 Mixamo 实际用别的轴，
> 需修正此约定并重新校准所有辅助姿态。**这是本设计的一个已知不确定点。**

### 2.2 与 `SkeletonType` 的关系

`SkeletonType` 是**资产相关**的（骨长/拓扑各不相同）。本约定定义的是
**"规范化目标形态"** —— 任何带骨 mesh 若不合此约定，需经 §4 的规范化流程变成这样。

---

## 3. 功能 1：按人体参数创建 `SkeletonType`

### 3.1 目标

提供一个工厂函数：**输入人体常见参数 → 输出符合 §2 约定的 `SkeletonType`**。

### 3.2 接口草案

```cpp
// 人体测量参数（常见人体参数；单位：米）
struct HumanBodyParams {
    float height          = 1.75f;  // 总身高
    float leg_length      = 0.90f;  // 腿长（Hips → 脚底）
    float arm_length      = 0.62f;  // 臂长（Shoulder → 手）
    float shoulder_width  = 0.40f;  // 肩宽
    float hip_width       = 0.24f;  // 胯宽
    float torso_length    = 0.52f;  // 躯干长（Hips → 颈）
    float head_size       = 0.23f;  // 头长
    // ...（按需扩展；缺省值取自成年人平均值）
};

// 按人体参数 + Mixamo 布局约定创建骨架。
// 产出：joints 树（骨名用 mixamorig:* 以便与 FBX 对位）+ rest_offset（由参数决定骨长）
//       + inverse_bind（由 rest 姿态算出）
SkeletonType MakeMixamoLayoutSkeleton(const HumanBodyParams& params);
```

### 3.3 需求澄清（待 Danis 补充）

Danis 原话：「提供腿长、三维之类的输入参数（人体常见参数）。这些会决定骨头长度」

**待明确**：
- 参数集具体要哪些？（上表是草案）
- 骨名是否统一用 `mixamorig:*`？（便于与 FBX 对位，但需确认无授权顾虑）
- 骨数是否对齐 Mixamo 全身 65 骨，还是简化为 23 骨（去手指）？

---

## 4. 功能 2：骨架可视化（火柴人 mesh）

### 4.1 目标

`SkeletonType` 能在世界坐标系下**创建一个 T-pose 火柴人模型** ——
**一个带骨（skinned）的 mesh**，其关节坐标 = T-pose 时各关节的**原父系坐标**。

### 4.2 生成规则（按 §2 约定，Danis 定）

> **根关节在 `(0,0,0)`，骨头沿 +Y 生长，Z 是骨 up。**

即：
```
每根骨 = 一个沿局部 +Y 生长、长度为 rest_offset 的细长几何体
根关节起点 = (0,0,0)
关节坐标 = 按 T-pose 沿树累乘算出（即 JW_bind 的平移部分）
```

### 4.3 实现要点

- 输出 **`MeshData`（带 kJoints 属性）**，可直接 `RegisterMesh` + 蒙皮渲染
- **关节坐标 == T-pose 的原父系坐标** → 与 §1.2 的辅助姿态天然一致
- **做成 util**（纯 CPU 几何生成，GL-free，可单测）
- **gold test 保护**（渲一帧与 gold 图对比，确保火柴人形状正确）

### 4.4 用途

1. **人工校准工具的对齐参照**（§5）—— 有它才能肉眼判断 mesh 是否贴骨
2. **调通 FBX 预览的载体**（§6）—— 先用火柴人验证动作对不对，再上真皮

### 4.5 `inverse_bind` 走**自算**（程序化生成的典型场景）

火柴人是**我们自己按 rest 造的**（没有外部导出）：

- mesh 顶点按 §4.2 规则生成 → 顶点姿态 == `SkeletonType` 的 rest
- 因此 `inverse_bind = JW_bind⁻¹` **自算必然正确**（两边同源，天然配套）

→ 火柴人是 §5.5「程序化生成走自算」那条路的**第一个实践者**，也是最干净的验证载体。
（对比：Tripo glb 等外部资产必须用**自带的** `inverse_bind`。）

---

## 5. 功能 3：glb 简易编辑器（人工校准）

### 5.1 目标

> 打开 + 预览 + 用户调节旋钮滑条变换 + 一键保存。

**编辑时同步画骨人** → 即可实现 retarget 的人工校准。

### 5.2 工作流（Danis 描述）

```
1. 打开 glb（模型查看器）
2. 同时画出骨人（§4）—— 半透明 mesh + 火柴人叠加
3. 用户用滑条/旋钮调节变换，让 mesh 贴合火柴人
   - 平移 / 旋转 / 缩放
   - 骨长调整（缩短上半身、胳膊、脊椎…）
4. 一键保存 → 写回 glb
```

### 5.3 目标状态

> **保障 mesh 经 retarget 过程后「M = I」**

### 5.4 ⚠️ 重要修正（实测发现，与 Danis 原始判断不同）

Danis 原以为「glb 里的人物相对原父系有 scale、整体旋转、平移」。

**实测结论：`mixamo_male.glb` 的 `M` 已经是 `I`。**

| 检查项 | 实测 |
|---|---|
| 有无 scale | ❌ 全部节点只有 ±1e-7 浮点噪声，**无真实缩放** |
| 有无整体旋转/平移 | ❌ 祖先链只两层：`Armature`（**无 TRS**）→ `Root`（骨架 0 号关节自身）|
| mesh 顶点范围 | `y∈[0, 0.998]` —— 1 米高、Y-up 竖直站立 |
| 结论 | **M = I，glb 内部自洽，没有多余自由度可"调掉"** |

**所以这一步的真实目标不是"消 M"**，而是：

> **把 mesh 贴合到"标准骨架布局"** —— 即调整 `rest_offset`（骨长）与同步重算 `inverse_bind`，
> 使网格能正确蒙在骨架上。

### 5.5 ⚠️ 关键实现约束：`inverse_bind` 的归属（本次修正）

**先澄清一个易犯错的认识**：`inverse_bind[i] = JW_bind[i]⁻¹`，而 `JW_bind` 完全由
`SkeletonType`（骨长）+ bind pose（rest 朝向）决定 —— 所以它**看起来**是个可自算的派生量。

**但对外部资产，它必须用模型自带的，不能自算。** 原因是蒙皮公式里的**配对关系**：

```
v_posed = JW_pose[i] · inverseBind[i] · v_rest
                        └──────┬──────┘
                    必须与 v_rest 配套
```

`v_rest` 是 mesh 文件里的 `POSITION`，在**导出那一刻的具体姿态**下写出。
`inverseBind` 的唯一职责 = **撤销 `v_rest` 所处的那个姿态**。

> **谁产生的 `v_rest`，就用谁配套的 `inverseBind`。**

自算的 `inverseBind` 只知道 `SkeletonType` 的 rest，**不知道 mesh 顶点实际用的什么姿态**。
两者一旦不一致，顶点就被搬错。

#### 规则（按 `v_rest` 来源分流）

| 场景 | `inverse_bind` 来源 |
|---|---|
| **外部资产**（Tripo glb / Mixamo FBX） | **用模型自带的**；规范化时**共轭**过去 |
| **程序化生成**（§4 火柴人 / §3 参数化骨架） | **自算**（`= JW_bind⁻¹`），天然正确 |

#### 规范化时的处理：**共轭**，不是重算

改 `rest_offset`（骨长）时，`inverse_bind` 应：

```
新 IBM = (骨架坐标变换) · 原 IBM · (骨架坐标变换)⁻¹      ← 共轭
```

> **即：把原 IBM 共轭到新的骨架坐标系**，而不是从骨架重算。
> 这样「顶点 ↔ inverseBind」的**配套关系被保持**。

#### 实测印证（`mixamo_male.glb`）

`glb 的 IBM` vs `自算 JW_bind⁻¹`：全部 23 骨最大差 **1.46e-05**（浮点噪声级）。
本资产 `M = I`（`Armature` 无 TRS），两者等价 → **无法区分**。

⚠️ **待验证**：需要一个 `M ≠ I` 的真实资产，确认 glTF 的
`globalTransformOfJointNode` 在含祖先变换时相对哪个参照点（骨架 or 场景根）。
本设计**按「相对骨架原点」实现**（与 JPOV 自算同一参照系）。

#### 代码现状

`skeleton_manager.cc:161` 已预留自算路径（`has_ibm` 判断），`skeleton_types.h` 注释也写明
「空 = SkeletonManager 按 SkeletonType 链式 rest 自算；否则用显式值」
→ 设计早就认了「两条路都行」，本设计只是**明确何时走哪条**。

### 5.6 待明确

- "一键保存"要不要写回**原 glb 文件**，还是输出**新文件**？（建议新文件，不破坏原资产）
- 编辑的粒度：只调整体变换（平移/旋转/缩放），还是**逐骨调长度**？
  （Danis 提到"缩短上半身、胳膊、脊椎"，暗示需要**逐骨或按部位**调）

---

## 6. 功能 4：FBX 重定向

### 6.1 目标

把 FBX 动画源的姿态，正确搬到目标 `SkeletonType` 上。

### 6.2 三类差异的处理策略（Danis 定）

| 差异 | 策略 | 说明 |
|---|---|---|
| **旋转** | **100% 保留**（经 rest 基准共轭） | 用 §1.2 的核心式做基准对齐；"保留"= 不丢失旋转信息，非原样搬运 |
| **骨长** | **走 `SkeletonType` 自身长度** | 不动目标骨长（见 §6.3）|
| **root_offset** | **乘缩放因子**（`fbx腿长 / skeleton腿长`） | 把人物上下位移捕捉到，"原来两脚站地上，retarget 后也站地上" |

### 6.3 root_offset 缩放（Danis 方案 + 评估）

Danis：
> root offset 的话，我判断简单起见乘以一个缩放因子，`fbx腿长 / skeleton腿长`，
> 这样把人物上下的 root offset 捕捉到，尽量原来两脚站在地上，retarget 后也是站在地上。

**评估**：

✅ **方向与业界一致** —— 业界称 root motion 的 **scale adaptation**，是常规做法。

⚠️ **两个必须先解决的问题**：

1. **`root_offset` 当前没接线**（§0.2）—— 要生效必须**先把它接进烘焙**（`skeleton_manager.cc`）
2. **标量因子是粗略近似** —— 更准是按链长比（"根到脚"的骨长和之比）；但步幅、接触点仍受影响

**业界的现实态度**（引文）：
> This algorithm is particularly useful to retarget **vague** animations such as running or walking.

→ 即：**跑步/走路这类模糊动作够用；精确踩点需额外 IK 修正**。

**本设计的取舍**：先做标量因子（简单有效），接触密集动作留后续。

### 6.4 实现顺序（Danis 定）

> **先用 §4 的骨人 mesh 调通，再上真皮。**

即：
1. 先用火柴人 mesh 验证 FBX 动作搬过来是否正确（形状简单，易判断）
2. 确认无误后，再换真 mesh

**这个顺序很务实** —— 火柴人排除了 mesh 本身的干扰，能把"retarget 数学对不对"和"mesh 蒙皮对不对"**解耦验证**。

---

## 7. 里程碑规划（建议）

| # | 内容 | 交付物 | 依赖 |
|---|---|---|---|
| **M1** | `MakeMixamoLayoutSkeleton(params)`（§3） | 工厂函数 + 单测 | — |
| **M2** | 火柴人 mesh 生成 util（§4） | `MeshData` 生成器 + **gold test** | M1 |
| **M3** | `root_offset` 接线进烘焙（§6.3-1） | 烘焙改动 + 测试 | — |
| **M4** | **FBX 重定向数学**（§1.2 核心式 + §6）| 重定向函数 + 骨人 gold test | M1 M2 M3 |
| **M5** | glb 编辑器（§5） | model viewer 新模式 + 保存 | M2 |
| **M6** | 真皮接入（§6.4 第二步） | 用真 mesh 跑通 | M4 M5 |

**建议先做 M1 + M2**（工厂 + 火柴人），因为：
- 它们是**所有后续工作的验证载体**
- 都是纯 CPU，**可单测 + gold test 保护**，风险低
- 做完就能肉眼看到"标准 Mixamo 骨架长什么样"

---

## 8. 需 Danis 明确的开放问题

1. **§2.1 up 轴 = +Z 是惯例推断，非实测** —— 有无权威依据可确认？
2. **§3.3 人体参数集**：具体要哪些？骨名用 `mixamorig:*`？65 骨还是 23 骨？
3. **§5.4 目标修正**：glb 的 `M` 已经是 `I`，所以 §5 的目标改为"改 `rest_offset` + 重算 `inverse_bind`"，确认？
4. **§5.6 保存策略**：写回原文件 or 新文件？编辑粒度（整体变换 or 逐骨长度）？
5. **§5** glb 编辑器是否需要**写 glb**（二进制 glTF 序列化）能力？仓库目前只有 **读**（tinygltf），写需要新增能力（tinygltf 支持写，但需验证）。
6. **§5.5 验证项**：能否提供一个 **`M ≠ I`** 的真实资产（骨架 0 号关节之上带变换的祖先）？
   用于确认 glTF 的 `globalTransformOfJointNode` 在含祖先变换时的参照点，
   以及验证“外部资产用自带 IBM”这条规则在非退化情形下的行为。

---

## 9. 业界参考

| 来源 | 要点 |
|---|---|
| `upf-gti/retargeting-threejs` Algorithm.md | 辅助姿态（auxiliary pose）算法；joint mapping → skeleton preparation → retarget joint transformations |
| Unity Manual — Retargeting of Humanoid animations | Avatar 定义 = 人工映射；**每资产一个**（JPOV 用 `SkeletonType` 替代）|
| SAN / PAN（arXiv 2005.05732 / 2306.08006） | 数据驱动跨结构 retarget；把 motion 与 skeleton shape **解耦**到共享隐空间 |
| mocaponline.com 指南 | 三个差异维度：bone names / **bone orientations (rest pose)** / **limb proportions** |

**关键引文**（`retargeting-threejs`）：
> The position with respect to its parent and children, determine in which axis a limb
> needs to be rotated to achieve a particular pose. This implies that depending on how
> the skeleton is modeled, **the local transforms of a skeleton might differ from others,
> even if they are successfully applied to the same mesh**.

> ⚠️ 本文档引用的外部资料为调研所得，**需按需复核**（链接可能失效或版本变化）。
