# JPOV 换装 + 衣物蒙皮（clothes rig via weight transfer）设计

> 目的：让 JPOV 具备「mixin 骨骼下，给人物角色换装（贴身衣物）+ headless 渲染验证」的一条
> 程序化链路。衣服 = 一种「特殊的人体 mesh」：不自己建 rig，靠**从身体贴合的 weight transfer**
> 绑到同一套骨骼，跟随同一套骨骼动画纹理。
> 对应 2026-09-08 与 Danis 的可行性收敛。本文给出**缝合脚本（weight transfer）的设计细节**，
> 作为落地 PR 的功能设计。本 PR 范围只做**贴身单层**，宽松/叠穿明确划出范围外。
>
> 关联：`docs/jpov_gen3d_design.md`（文本→静态 PBR GLB）、`docs/jpov_crowd_instancing_arch.md`
> §6.2-B（骨骼动画纹理）、`interface/skeleton_types.h`（SkeletonType v3）。

## 0. 现状（已核实）

- 仓库 `tools/jpov/interface/skeleton_types.h`（v3，2026-09-07 收敛）：
  - `struct SkeletonJoint { int parent; Vec3f rest_offset; }` —— **无 name 槽、无 rest 旋转**。
  - `struct SkeletonType` 存在，但**无 Mixamo static 模板、无任何预设骨架**。
  - 文件 160 行，注释重申铁律「同一骨架的 pose 之间才能插值」。
- `src/skeleton/skeleton_manager.h`：GPU 侧（骨骼纹理 / instancing）manager，本次脚本**不触碰**。
- **rest-mesh 顶点属性预埋**（gpumesh.h / mesh_manager.h）：`JOINTS_0 → vertex loc3`、
  `WEIGHTS_0 → vertex loc4` 已预留为 skinned 每顶点权重输入（见 skeleton_types.h 头注释）。
- 生成链路现状：`gen3d/static`（jpov_gen3d 静态部分，工具 `gen3d_static.sh`）现在产出的是
  **静态 PBR GLB（无骨骼）**；将来要拿「蒙皮+统一骨骼」，需在 lowpoly **之后**补一步 Tripo rig，
  `spec=mixamo`，落在新的 `gen3d/skeleton`（工具 `gen3d_skeleton.sh`）。
- 角色现状（Danis）：**4 套人体 mesh**（角色因素差异，scale/tint 不考虑），每套**一套 mixamorig
  骨骼**（rig 用 `spec=mixamo`，rest pose = T-pose）。要做的衣物是**贴身的短袖/长袖等**，用
  Tripo **参考生成法**贴着指定人体生成。

## 1. 目标架构（本次 PR 范围）

一段可重复的「给 1 号人体做 N 件贴身衣物」缝合链路：

```
输入:
  body.glb                       ← 含 mixamorig 骨骼、已蒙皮(JOINTS_0/WEIGHTS_0)、
                                   rest = T-pose、与绑骨同 rest pose
  clothes_k.glb                  ← 第 k 件贴身衣物，Tripo 在【同一个 T-pose body】上参考生成，
                                   尚无骨骼/权重

缝合脚本 clothes_rig (新工具, 放 tools/jpov/clothes/):
  ┌─────────────────────────────────────────────────────────┐
  │ 1) weight transfer：为衣物每顶点算 JOINTS_0/WEIGHTS_0   │
  │    = 身体最邻近三角形重心坐标插值权重灌给衣物顶点        │
  │ 2) 质量门控：gap 顶点 / normal 朝向 / skin gap 校验      │
  │ 3) 输出 clothes_k.rigged.glb（带权重、绑同一套骨）        │
  └─────────────────────────────────────────────────────────┘

JPOV loader(既有加载动画的扩展) → SkeletonType::Mixamo 模板对位
  → 骨骼动画纹理驱动 → gold image 逐帧并排展开观察关节
```

范围边界：**本次 PR 只做「weight transfer 缝合脚本 + mixin 模板」这一半**，
把「衣物绑定到骨骼、贴身不穿模」跑通闭环。Tripo「生成贴合衣物」与渲染展开在 PR 外
由人/既有链路执行，本文聚焦那台脚本的算法细节与验收。

## 2. 关键认知 & 铁律（写进代码注释，防返工）

### 2.1 衣物绑骨 ≡ weight transfer，无他法
对没有自建 rig 的独立衣物网格，「给它绑骨」的定义就是 **weight transfer**：把身体已绑好的那套
骨骼+权重，**按几何邻近关系映射/复制到衣物每个顶点**。两者不是两件事。

### 2.2 铁律 A —— 参考生成的 rest pose 必须 === rig 用的 rest pose
weight transfer（最近顶点映射）的前提 = 衣物与身体在**绑骨相同的 rest pose（T-pose）**下位置对齐。
若 Tripo 在任意/某一姿态下参考生成衣物，衣服贴合的是"那个姿态"，而身体权重绑在 T-pose——
一旦换 frame 驱动，相对错位 → 穿模。

> 顺序铁律（呼应 bone-convention 调研 + Tripo 官方原话）：
> **衣物的 Tripo 参考生成，必须基于「已 rig(spec=mixamo) 的 1 号人体、且该人体站成 T-pose」导出
> 的 mesh 作参考。** 参考体 mesh 与绑骨用的 rest mesh **是同一个 rest pose**。
> （且不要拿未 rig 的静态 mesh 当参考去生成衣服，骨骼语义对不上。）

### 2.3 铁律 B —— 宽松 / 叠穿划出本轮范围
- **宽松衣物**（裙摆/大衣/垂坠）：顶点离 body 远，最近点映射意义弱；且无布料模拟 → 必穿模。
  本轮**只做贴身**；宽松留待「烘焙垂坠 rest shape 或接受刚性」。
- **重叠衣物**（内短袖 + 外开衫叠穿）：层间互穿平方级复杂 → 本轮**只做单层**，不做叠穿。
- 因此缝合脚本可放心假设：衣物每顶点都贴近身体表面、且为单层。

## 3. 设计细节：weight transfer 算法

### 3.1 几何加速结构
- 把 body rest 网格三角化 → 构建 **BVH / uniform grid**，支持「点 → 最近三角形」O(log n) 查询。
- 衣物顶点 `v_c` → 最近 triangle `(t0,t1,t2)` + 最近点 `p` + 重心坐标 `(w0,w1,w2)`，`Σw=1`.

```
// 伪代码（每衣物顶点）
for v_c in clothes_verts:
    (tri, p, w0,w1,w2) = bvh.nearest(v_c)            // body 上最近构型
    cand = weighted_joints(tri, w0,w1,w2)            // 见下
    if dist(v_c,p) > kGapThreshold:                  // §3.3 gap 门控
        fallback_to_parent_bone_or_flag(v_c)
    joints_c = top4(cand, by weight); normalize(sum=1)
```

### 3.2 重心坐标权重插值
- body 三角形三顶点分别持有 `JOINTS_0(ivec4)` + `WEIGHTS_0(vec4)`（≤4 bone）。
- 采集 3×4 潜在 influence；按重心坐标加权后取全局 top-4 → 衣物该顶点的 `JOINTS_0/WEIGHTS_0`。
- 权重归一化到 `Σ=1`（谨防 float 累加误差）。

### 3.3 质量门控（决定"是对的" vs "差不多"）
- `kGapThreshold`:衣物顶点到 body 最近距离 > 阈值（建议默认 3~5mm，可配）→ 不算"贴合"。
  这类顶点**别硬给错骨**：给父骨 + 标记 gap（宽松处/缝线处悬挂），避免撕裂。
- **normal 朝向检查**：`dot(cloth_normal, body_normal)` 应 >0（衣物外法向与身体外法向一致），
  反号说明衣物里外穿 / 参考姿态错位 → 该处报警。
- **skin gap**（防微穿模缓冲）：rest 下允许衣物沿自身法线膨胀 0~2mm（`skin_gap`），紧身件靠这个
  "皮肤空间"吸收数值误差。此步写进 rest mesh，不改权重。
- **判定为成功**：紧身件静止贴合误差 < X mm，且在动画帧下顶点穿透深度近 0（见 §5 验证）。

### 3.4 交互面：衣物为什么能与身体天然对齐
- 衣物体 + 身体共用**同一套 mixamorig 骨骼**、同一份权重模板、同一个 rest pose
  （T-pose）。换装 = 互换衣物网格，动画时骨骼一次驱动全部可见层 → 天然不穿模。
- 衣物也是"特殊人体 mesh"，工作流 ≈ 人体 mesh 工作流（同一供应商管线、同一骨骼模板、
  同一权重约定）——正是本设计要固化的抽象。

## 4. SkeletonType::Mixamo 模板（程序侧配套，配合脚本对位）

缝合脚本产出的权重是 `mixamorig` 骨骼的；JPOV 要让"根对根"对上、并能量化验证，
需要在骨骼层内置一份**有序 Mixamo 模板**：

- **建议**:`interface/skeleton_types.h` 给 `SkeletonJoint` **加 `std::string name` 槽**
  （现在只有 parent+rest_offset，没名字无法对位）。
- 新增 `static const SkeletonType& Mixamo();`（或 constexpr 表），inline 实现于头文件（用户可见、
  可查）。内容 = 全套 mixamorig 骨架（约 60~70 骨，含手指），**名字/层级/顺序须与
  Tripo `spec=mixamo` 实际输出严格一致**。
- **⚠️ 生成方式（决定性）**：**不要手抄**——从一份真实 Tripo `spec=mixamo` 导出的 GLB，
  把它 `skins[0].joints`（node 顺序 + 名字 + inverseBindMatrices accessor）dump 出来，
  以此为准生成模板。手抄易差一两个骨名/顺序 → 对位断。
- 模板来源建议放 assets/ 一份真实 `mixamo_body_ref.glb` 作为权威基准（同 gen3d 资源惯例，
  git 忽略生成的 `output/`，基准样本可入库 `testdata/`）。

> 本模板 + name 槽改造 = 与脚本配套的程序侧改造。若不想动接口，可先做成「硬编码表」，
> 但不建议（与 glTF loader 接 skins 的长线冲突，见 §6 展望）。

## 5. 验证与验收（必须写，否则没法"放心"）

### 5.0 测试资料（人侧）
需要先在仓库外制备，本文只声明验收目标：
- 1 号人体：身一 mixamorig rig GLB（`spec=mixamo`，T-pose rest）；
- 数件贴身衣物：Tripo 参考同 T-pose 人体生成（短袖、长袖…），贴合无自建骨骼。
- Pose 序列：Mixamo 现成**舞种动作**（FBX/`mixamorig`）转 glb 动画 → 每帧即一个 pose
  （响应 2026-09-08 Danis：pose 来源**直接下载 Mixamo 现成动作库**，不做 AI 摆关键帧 + 插值）。

### 5.1 rest-pose 贴合检查（静止）
- 每件贴身衣物的衣物顶点穿透 body 深度最大值 < X mm（用 SDF/最近距离统计）。
- normal 朝向一致性：全衣物顶点违规率 < 阈值。

### 5.2 动画不穿模（动态）
- 把一段动作逐帧跑，统计衣物顶点对 body 的穿透深度分布——**贴身件应近 0**。
- 在 headless 渲染（现有 gold 机制）里**按帧并排展开成一张图（如 1 图 10 人）**，人工目检
  关节处（尤其**袖口→前臂/手指**窗口）是否撕裂/穿模 —— 这正是"火柴人关节肌肉略明显"的观察意义：
  紧身袖口在细关节处拓扑要密，weight transfer 才能跟准。

### 5.3 换装正确性（跨 body，若本轮延伸到 2~4 号）
- 若衣物要穿到 2~4 号：前提是**共用同一基准底模（rest 轮廓差在容差内）**。每件衣服对基准
  body 做一次 weight transfer，再对不同 body 做轻微 rest 贴合。**本轮 PR 若不承诺跨 body，
  §5.3 划出，避免"契合担忧"未经验证就铺 4×N。**

> 验收门：bazel test //... 全绿 + 上述三节指标达标，才可称"能放心开始搞换装"。

## 6. 落点与展望（非本次 PR）

- 文件路径建议：`tools/jpov/clothes/`（脚本工具 + ClothesRigConfig 供应商无关，仿 gen3d 分层）；
  生成衣物落盘 git 忽略的 `output/clothes/`；权威基准 mixamo body 放 `testdata/`。
- 长线（明确非本次）：glTF loader 扩展接 `skins`（读 `skins[0].joints` + IBM accessor →
  SkeletonType + inverse_bind），SkeletonPose 每骨 `{quat rot, vec3 trans}`（相对父），CPU 沿树
  解算落 pose atlas —— 与 glTF 每 node 的 local T/R 一致，未来 glTF anim / retarget 天然兼容。
- 宽松/叠穿、多 body 间迁移：等贴身单层被验证「贴合 + 不穿模」后再议，本次不碰。

## 7. 尚未决策、需 Danis 拍板（写进 PR 描述供评审）

1. `SkeletonJoint` 是否**加 `name` 槽**（建议做，长线 loader 需要；若只想先跑服装脚本，可暂缓）。
2. Mixamo 模板**从真实 Tripo `spec=mixamo` GLB dump 生成**（强烈建议）还是先手抄名字。
3. 本轮 PR 是否承诺「1→2~4 号跨 body 迁移」，还是**只做 1 号 + 验证贴合机制**（建议后者，
   契合担忧用单 body × 紧身+宽松小实验定量后再决定铺矩阵）。
