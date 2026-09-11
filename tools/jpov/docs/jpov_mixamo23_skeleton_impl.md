# JPOV Skeleton — Mixamo23 骨架工厂 + bind 形态健全化（实现记录）

> 本文记录 `Mixamo23Skeleton` 的落地（2026-09-11，PR 内容）以及一路上的概念澄清与踩坑。
> 设计纲领见 `jpov_retarget_design.md`（本文是其 §3 的可执行落点 + 数据来源实据）。

---

## 1. 本次交付

| # | 内容 | 位置 |
|---|---|---|
| 1 | **公共 mat4 工具**（列主序 4x4 + 仿射求逆 + 骨架组合式） | `geom/math/mat4.h`（+ `mat4_test.cc`） |
| 2 | **`SkeletonType` 健全化**：加 `bind_rotation`，删 `inverse_bind` 字段 | `interface/skeleton_types.h` |
| 3 | **`SkeletonType::ComputeInverseBind()`**（派生量，由骨架自算 IBM） | 同上 |
| 4 | **`SkeletonPose::Identity(n)`**（全恒等 pose） | 同上 |
| 5 | **`Mixamo23Skeleton(height)`**（Mixamo 官方 23 骨，程序化生成） | `interface/mixamo23_skeleton.h`（+ `mixamo23_skeleton_test.cc`） |
| 6 | loader/烘焙适配 + 退役 `skinning_bind_pose.h` | `gltf_loader.cc` / `fbx_loader.cc` / `skeleton_manager.cc` |

---

## 2. 核心概念（本次澄清，Danis 逐条确认）

### 2.1 「布局」与「姿势」是两件事

| 概念 | 内容 | 落在哪 | 变不变 |
|---|---|---|---|
| **骨架布局** | 骨名 / 拓扑 / 骨长 / **骨朝向** | `SkeletonType` | 骨架定死 |
| **姿势（pose）** | 每关节相对父的旋转 | `SkeletonPose` | 每帧变 |

### 2.2 `bind_rotation` 是「骨朝向」，不是恒等

```
jointLocal(j) = T(rest_offset[j]) · R(bind_rotation[j]) · R(pose[j])
```

- `rest_offset` 管「**多长**」（方向恒为局部 +Y）
- `bind_rotation` 管「**朝哪**」——**通常非恒等**（如 `LeftShoulder` ±90° 级旋转把手臂掰水平）
- **pose 全恒等 → T-pose**（bind 朝向已烘进骨架，pose 不再叠旋转）

→ 所以 `SkeletonPose::Identity(n)` 配合 Mixamo23 骨架 = **T-pose**。T-pose 是**结论**，
  不是输入；故函数名里不带 `TPose`（`Mixamo23Skeleton`，非 `...TPoseBindSkeleton`）。

### 2.3 「Mixamo」这个词管的是**布局**，不管 bind 姿势

- Mixamo 官方标准 = 骨名 + 拓扑 + 骨朝向（**布局**）
- bind 姿势理论上可以是任意（T-pose / A-pose）；Mixamo 生态**实际**全用 T-pose，但
  这不属于「Mixamo」这个词的语义
- 对**我们自己造**的骨架：既然照官方布局造，`bind_rotation` 就 = 官方 T-pose 朝向 → 必然 T-pose bind

### 2.4 `inverse_bind` 是派生量，不是字段

```
inverse_bind[j] = JW_bind[j]⁻¹        （JW_bind 由 joints + bind_rotation 决定）
```

留作字段会造成「输入 + 输入的导出物」两份数据打架 → 删字段，改 `ComputeInverseBind()`。
**对外部资产**：仍应优先用资产自带 IBM（经共轭），但那是「资产规范化」那一步的事
（见 `jpov_retarget_design.md` §5.5）；`SkeletonType` 只认自己的 bind 形态。

### 2.5 术语：**骨架空间**（skeleton space）

旧称「原父系」已统一改为 **骨架空间 / skeleton space** = 骨架自身坐标系的原点。

---

## 3. 数据来源（重要）

### 3.1 官方**没有**骨架数值文档

调研（2026-09-11）结论：
- Mixamo 官网只说「自动 rig 一个 full human skeleton」，**零数值**
- 社区资料（gist / 工具）**只有骨名映射表**，没有骨长/朝向
- 唯一「半官方数值源」是 MakeHuman 的 rig 数据，但那是**MakeHuman 骨架**，不是 Mixamo

→ 按 Danis 约定「**优先上网查，查不到则用资产反推**」：**从 FBX 反推**。

### 3.2 用 `Hip Hop Dancing.fbx`（**不是** `mixamo_male.glb`）

| | `Hip Hop Dancing.fbx` ✅ 采用 | `mixamo_male.glb` ❌ 不用 |
|---|---|---|
| 来源 | Mixamo 官方导出 | **Tripo 生成**（`asset.generator="Tripo"`） |
| 骨数 | 65（含手指/叶端） | 23 |
| 骨长轴 | **纯 +Y** | 主体 +Y，Hips/UpLeg 为 Z |
| 手臂朝向 | **±X（T-pose 水平）** | ±Z（Tripo 自己的朝向） |
| 单位 | cm（`unit_meters=0.01`） | m |

**关键**：两者**骨名相同、拓扑相同，但 rest 朝向不同源** —— 这正是上次 retarget 失败的根因
（见 `memory/2026-09-10-jpov-skinning-math.md`）。所以官方布局必须取 FBX。

### 3.3 23 骨的选取

从 65 骨取 23 = **1 个包装 Root + 22 具名人形骨**。

**验证「65 − 23 = 42 个未选中骨全是叶子」**：实测确认，未选中的 43 个（40 手指 +
`HeadTop_End` + 2×`Toe_End`）**没有一个是我们 22 骨的祖先** → 23 骨在 FBX 里完整可用。

补 `Root` 的理由（Danis 定）：
1. 与 glTF 资产（`Armature`/`Root` 包装层）结构一致，便于跨源对位
2. 给整体位移/旋转（root-motion、模型摆放）一个统一抓手（23 骨比 22 骨操作自由度大）

### 3.4 单位与身高

- FBX 是 **cm** → `rest_offset` ×0.01 转米（`bind_rotation` 是纯旋转，原样搬）
- **`height` 语义 = 骨骼身高**（脚底到头顶，**不含皮**，米）
- 官方骨骼身高实测 ≈ **1.600 m**（Head 骨末端 y=1.5993、ToeBase y=0.0009）→ `kMixamoOfficialBoneHeight`
- 缩放 = `height / 1.600`，对 `rest_offset` **等比缩放**（骨长比例严格保持官方）

**按业界思路不引入「缩放」概念**（调研结论）：
- 单位换算是 **loader** 的职责（`ufbx` 已给 `unit_meters`）
- 引擎侧惯例是「模型空间归一化到 1 unit = 1 米、根节点 scale = 1」（Unity/UE/three.js 一致）
- 「巨人版」属于**放置层**（`SkinnedInstanceState.scale`），不是 `SkeletonType` 的属性

---

## 4. 微调系数（调查结论，**本 PR 不做**）

业界的答案：**没有独立的「微调系数」概念**，而是把骨长微调建模为 retarget 的一部分。

| 引擎 | 机制 |
|---|---|
| **UE5** IK Retarget | 每 chain 的 `Translation Mode`：`None`(不动) / `Globally Scaled`(×全局比例) / `Absolute` / `Stretch Bone Length Uniformly` / `Non-Uniformly`(逐骨)；配 `Translation Alpha`(0~1 权重) |
| **Unity** HumanDescription | `upperArmTwist`/`upperLegTwist`(0~1 分配)、`armStretch`/`legStretch`(允许拉伸量)——**没有绝对骨长系数** |
| **Blender** Rigify | 不设系数，**直接挪骨头**（Edit Mode 拖 head/tail）；且约定 **1 unit = 1 米** |

**共性三层**（从粗到细）：① 全局比例（单标量）→ ② 部位比例（per-chain）→ ③ 逐骨。
且都带「要不要应用」的开关。

**Danis 决策**：本 PR **只做 ①**（`height`）；**微调留给「从资产反推骨长比例」的独立函数**
（调骨头不如调模型）。部位级微调不为 `Mixamo23Skeleton` 引入参数。

---

## 5. 验证

| 项 | 结果 |
|---|---|
| `geom/math:mat4_test` | **15/15** |
| `interface:mixamo23_skeleton_test` | **18/18**（含 **T-pose 世界坐标自证**：头顶 1.599m / 手 ∓0.713m 水平 / 脚踩地 / 腿 ±0.082m） |
| 全量 `bazel test //geom/... //tools/jpov/... --jobs=1` | **60/60，154 test cases 全绿** |
| skeleton gold 图 | **MD5 未变** → `bind_rotation` 重构**行为零变化** |
| Windows 交叉编译 | ✅ `--config=windows` 通过 |

**gold 零回归的意义**：`skinning_bind_pose.h` 的硬编码表内容 == glTF 的 `node.rotation`，
所以「退役硬编码表 + 改走 `bind_rotation`」在行为上完全等价 —— 这是重构正确性的硬证据。

---

## 6. 踩坑 / 注意

1. **`geom::Quaternion` 无 `Length()`** —— 向量范数方法叫 `Norm()`，单位化叫 `Unit()`/`Normalized()`。
2. **匿名 namespace 里的 `using` 别名不外泄** —— `skeleton_manager.cc` 的匿名 namespace 里
   定义的 `using Mat4 = ...` 在其外的成员函数里不可见，需写全 `geom::math::Mat4Mul`。
3. **FBX 的 `Hips` 无父** —— 官方 FBX 里 `mixamorig:Hips` 就是顶层（无 `Root` 包装），
   与 glb 不同；我们**主动补**一个恒等 `Root` 当 0 号。
4. **glb 的 `Hips` 是 +Z 不是 Y/Z 颠倒** —— `Root` 层带 −90°X 旋转，`(0,0,0.534)` 经它
   之后就是 `+Y` 0.534m。T-pose 世界坐标实测正常（这是推翻了曾经的误判）。

---

## 7. 后续（不在本 PR）

- 「**从资产反推骨长比例**」函数（含部位微调系数）—— 架构 §5 / 编辑校准
- glb 简易编辑器（M5）
- FBX 重定向数学（M4，设计文档 §1.2 核心式）
- 火柴人 mesh 生成 util（M2）—— 本骨架是其输入
