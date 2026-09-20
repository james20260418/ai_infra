# JPOV root_offset 语义与 root-motion 重定向 — 设计

> 目的：把「根位移（root-motion）」这条一直**没接线**的链路补全，使人形动作**不再原地滑步、
> 不再悬空**。
>
> 状态：本文件是**定稿**（2026-09-20 与 Danis 收敛）。实现见 `src/fbx_loader.cc` /
> `interface/skeleton_retarget.h` / `src/skeleton/skeleton_manager.cc`。
>
> 相关：`jpov_retarget_design.md` §0.2 / §6.2 / §6.3（本文件是它们的落地版）。

---

## 0. 问题（为什么要做）

FBX 动作重定向到 glb 目标骨架后，肉眼可见两个症状：

1. **原地滑步** —— 角色脚在动，但整个人不前进（位移全丢）。
2. **腾空** —— 角色整体悬在地面之上。

根因（2026-09-20 排查）：

| # | 根因 | 位置 |
|---|---|---|
| **A** | `root_offset` **从未接进烘焙** —— 烘焙公式只看 `pose.joint_rotation`，根位移零引用 | `src/skeleton/skeleton_manager.cc` |
| **B** | FBX 侧存的 `root_offset` **语义不完整** —— 存的是根骨全量 local 平移（未减 bind，静息时 ≈ 一个腰高） | `src/fbx_loader.cc` |
| **C** | 重定向侧 **不搬** `root_offset`（硬置 0），且跨资产单位/尺度不同 | `interface/skeleton_retarget.h` |

→ 三处都要修，缺一无效。

---

## 1. 定稿：`root_offset` 的语义

```
root_offset ≜ 根关节（Hips / 根骨）
              在【其父坐标系】下，
              相对【其 bind 位置】的平移量（= 当前平移 − bind 平移）。
              单位 = 该 SkeletonType 的长度单位（JPOV 统一为米）。
```

**逐条拆解**：

| 项 | 取值 | 说明 |
|---|---|---|
| **坐标系** | **根关节的父坐标系** | 不是「根自己的 bind 系」。见 §1.1 |
| **原点** | 父坐标系的原点 | 对「根是顶层骨」的资产，父系 ≡ **模型系**（scene root） |
| **参考位置** | 根的 **bind 平移**（`joints[0].rest_offset`） | 所以是**增量**，不是绝对位置 |
| **静息语义** | `root_offset = 0` ⟺ 根停在 bind 位置 | ⚠️ **与改动前的行为逐字一致**（见 §1.3）|
| **单位** | 米（该 SkeletonType 的长度单位） | FBX 源在 loader 内部换算 |

### 1.1 为什么是「根关节的父坐标系」，不是「根自己的 bind 系」

四个理由：

1. **与烘焙代码天然对齐**：`skeleton_manager.cc` 里根骨的局部变换是
   `jointLocal(root) = T(rest_offset[root]) · R(bind) · R(pose)`（根无父，它就是世界变换）。
   `root_offset` 必须与 `rest_offset[root]` **同一个坐标系**，才能写成
   `T(rest_offset[root] + root_offset)` —— 最小改动。
2. **「根自己的 bind 系」是循环定义**：根的 bind **位置**本身就要相对某个系来量（即父系），
   再拿它当自己的原点，绕一圈没有增量信息。
3. **与 glTF / FBX 原生表示同构**：glTF 的 `translation` 通道、FBX 的 `Lcl Translation`
   都是「相对父」。定义为父系可直接互转，不必引入第三套基准。
4. **与 `joint_rotation` 语言对称**：`joint_rotation` = 「相对父的**旋转**增量」，
   `root_offset` = 「相对父的**平移**增量」。同一套语言，读代码不必换脑子。

> ⚠️ 常见混淆：对 **Mixamo / Tripo glb 这类「根是顶层骨」的资产**，三者在数值上塌缩成同一个
> （模型系 ≡ 根的父系 ≡ 根的 bind 系，仅差根 bind_rotation 那个可忽略的微旋转）。
> **别因此把定义写成「bind 系」** —— 那是特例成立的巧合，不是普遍定义。取父系才在所有
> 资产（含「零长包装 Root → Hips」两层的拓扑，如 `mixamo23_skeleton.h`）上站得住。

### 1.2 量纲问题（Danis 提问的答复）

「pose 本来是尺度无关的无量纲量，加上 `root_offset` 就有量纲了，是否不对劲？」

**答复：不是问题，但有一条必须写明的约束。**

- `SkeletonType.rest_offset` 本来就是**带量纲**的（米）—— 骨架的「形状」由长度定义。
  `joint_rotation` 无量纲、`root_offset` 带量纲，两者共存是**正常的**：前者是姿态，后者是位移。
- 两者对尺度缩放的响应**一致**：骨架等比缩放时，`rest_offset` 与 `root_offset` 都线性跟随，
  `joint_rotation` 不动。所以并没有把「苹果和橘子」混在一起。

**必须写明的约束（真实陷阱）**：

> `root_offset` 的长度量在 **`SkeletonPose` 里**，而骨架尺度在 **`SkeletonType` 里**。
> 二者是**独立**的对象 —— **缩放骨架不会自动缩放已经产出的 pose**。

⇒ **`pose` 与 `skeleton` 强绑定这条既有铁律，现在被 `root_offset` 进一步强化**：
改了资产的 bind（骨长 / rest 朝向 / 根位置），**针对它重定向过的所有 poses 必须重新重定向**。

这条与既有的「跨骨架 pose 不能插值」是同一类约束，不是新增负担。

### 1.3 向后兼容性

新定义下 `root_offset = 0` 的含义 = **根停在 bind 位置**，
与改动前「输出恒 0 / 烘焙忽略它」产生的效果**逐字一致**。

⇒ 本改动**不会让任何既有 pose 悄悄改变含义**；所有既有 gold 图在「不喂 root-motion」的
路径上应零回归。

---

## 2. 三处实现

### 2.1 FBX 载入：存「相对 bind 的增量」（修根因 B）

`src/fbx_loader.cc`，`LoadFbxAnimation` 逐帧循环里：

```
// 改前（错）：存全量 local 平移（静息时 ≈ 一个腰高）
pose.root_offset = tf.translation;

// 改后：减去根的 bind 平移 → 静息时恰为 0
pose.root_offset = tf.translation - bind_translation_of_root;
```

其中 `bind_translation_of_root` = `clip.skeleton.joints[0].rest_offset`
（`LoadFbxAnimation` 传 `unit_scale = 1.0` ⇒ 源原生单位，与 `tf.translation` 同单位，直接相减）。

⚠️ 根骨 = `i == 0`，即 `CollectBoneNodes` DFS 出来的**第一个 bone 节点**。
对「Hips 是顶层骨」的源，它就是 Hips；对「零长包装 Root → Hips」的源，它是包装 Root
（此时包装 Root 的 bind 平移 ≈ 0，语义仍然正确）。

### 2.2 烘焙接线（修根因 A）

`src/skeleton/skeleton_manager.cc`，逐骨烘焙循环里根的局部变换加一项平移：

```
jointLocal(root) = T(rest_offset[root] + pose.root_offset) · R(bind) · R(pose_rot)
```

实现上不需要改 `geom::math::JointLocal` 的签名（它被多处在用）—— 在调用点对根的
`rest_offset` 加 `root_offset` 即可。

**非根骨不受影响**：`root_offset` 是「相对父的平移增量」，而 `SkeletonPose` 的契约是
「只驱动旋转，骨长由 `rest_offset` 给」—— 非根骨没有平移自由度（见
`skeleton_types.h` 的类型注释）。故只有 `parent == kSkeletonNoParent` 的那一根吃这一项。

### 2.3 重定向搬运 root-motion（修根因 C）

`interface/skeleton_retarget.h`，`BodyRetargetPose` 里：

```
root_offset_t = Q_body · root_offset_s · (leg_t / leg_s)
```
三项各有理由：

| 因子 | 作用 | 依据 |
|---|---|---|
| `leg_t / leg_s` | **尺度归一**（scale adaptation）| 设计文档 §6.2/§6.3：业界称 root motion 的 scale adaptation，跑步/走路够用 |
| `Q_body` | **朝向归一** —— 横移方向跟着角色朝向一起转，源朝 +Z / 目标朝 +X 时不会「往前走变成往侧面滑」 | `Q_body = M_t · M_s⁻¹` 已在 `BuildBodyRetargetPlan` 里算好，零额外成本 |
| （不额外做单位换算） | 尺度因子已含单位归一 | 见下 |

**腿长的量法（Danis 定）**：取 **bind 时根关节世界位置的高度 y**（= 腰高）。

- 「根是顶层骨」时，根的 rest 世界位置 = `rest_offset[root]`，其 y 分量即腰高。
- **两侧必须用同一种量法**：都用 `RestWorldPositions(skeleton)[root]` 的 y。
- ⚠️ 单位：两侧的骨架各自带自己的单位（FBX 源若走 `LoadFbxAnimation` 的 clip.skeleton 是
  源单位，走 `LoadFbxSkeleton` 是米）。**比值**把单位约掉了，但要求**两侧量法一致**。

**守卫**：`leg_s` 或 `leg_t` ≤ 0（退化骨架）时不猜、不 fallback ——
`LOG(FATAL)` 或明确报错（与 `EstimateBodyFrame` 的退化处理同款态度）。

### 2.5 ⚠️ 单位纪律（2026-09-20 实战修 bug 后追加）

**这套公式要求「源位姿的长度量」与「`plan.source` 骨架」同一单位/尺度。**

原因：`leg_t / leg_s` 是**无量纲比值**，它只能做**比例缩放**，**不能做单位换算**。
若 `root_offset_s` 是 cm 而 `leg_s` 是按 m 量的，比值会静默错 **100 倍**。

#### 实际发生的 bug（Danis 实测：蓝色肉人飞走了很远）

| 环节 | 值 |
|---|---|
| 源帧 `root_offset`（来自 `LoadFbxAnimation`，**源单位 cm**）| 最大 ~124 cm |
| `plan.source` = `LoadFbxSkeleton` 产物（**米制**）| 骨盆高 1.0427 **m** |
| ⇒ 比值 | 0.5122（看似正常！）|
| ⇒ 重定向后 \|root_offset\| | **63.6 m**（= 应然值 0.64 m 的 100 倍）|

后果：蓝骨人 / 蓝带皮**飞出画面**。且旧 gold 只盖了 naive 对照路径（那里 `root_offset`
置 0）→ **静默通过**；已补上重定向路径的门禁。

#### 定下的纪律

1. **交米制下游前，长度量必须归一为米**。`FBXClip` 新增 `unit_meters` 字段（= 1 源单位
   多少米）供消费方显式换算；观察器统一在 `NormalizePoseLengthsToMeters()` 一处做。
2. **配骨架/位姿先看尺度**：旋转无量纲（怎么配都对），**长度量必须同尺度** ——
   这类 bug 只在“长度量”上现形，历史上用错的骨架都没暴露。
3. **火柴人也是下游**：`BuildBoneMeshInBoneSpace` 已接 root_offset（与烘焙同一条规则），
   所以它也吃这条纪律，不能拿它做“无所谓单位”的旁路。

#### 已知残余（非本 PR）

`LoadFbxAnimation` 仍“原样透传”源单位（一个刻意的loader 分工）。这让
「clip 位姿 + 米制骨架」这个组合成为一个**需要调用方自觉**的陷阱。
更彻底的做法是让 clip 直接输出米（两入口就完全同源）；但那会改变已文档化的 loader 行为 +
动到既有测试的意图，故**本 PR 不做，留给 Danis 拍板**。

---

### 2.4 边界：不搬 root_offset 的场合

`demo/fbx_viewer/skeleton_pose_transfer.h`（`TransferPoseByNameNoRetarget`）
是**刻意不做重定向的对照组**，它继续把 `root_offset` 置 0 —— 因为它没有两侧几何基准
（没有 `Q_body`、没有两侧腿长比），无从计算。**保持置 0 + 注释写明理由**即可。

---

## 3. 验证

| 层次 | 判据 |
|---|---|
| **载入** | 源静息帧的 `root_offset` ≈ 0（改前 ≈ 一个腰高）|
| **重定向（纯 CPU 单测）** | ① 源 `root_offset = 0` ⟹ 目标 `root_offset = 0`（向后兼容）；② 几何一致的两骨架（只差腿长）× 常数位移 ⟹ 输出 = 位移 × 腿长比；③ 两侧朝向差 90° ⟹ 输出位移随之旋转（验证 `Q_body` 生效）|
| **烘焙** | gold 图：喂一个带 `root_offset` 的 pose，角色整体位移；`root_offset = 0` 时与改动前**逐字节一致**（零回归）|
| **端到端（Danis 验收）** | fbx viewer 骨人**大致不悬空**（rest_offset 不同仍有微小脚入地/出地误差，属预期）|

---

## 4. 已知残余（不在本 PR 范围）

1. **标量腿长比是粗略近似** —— 更准是按链长比（「根到脚」的骨长和之比）；步幅、接触点仍受影响。
2. **接触密集动作需额外 IK 修正** —— 业界共识（引文见 `jpov_retarget_design.md` §6.3）：
   scale adaptation 对**模糊动作**（跑步/走路）够用，精确踩点需 IK。
3. **水平位移未做裁剪/居中** —— 大幅位移（跑步）时角色会跑出画面，属预期行为。
4. **姿态沿链的 rest_offset 差仍会产生微小脚入地/出地** —— 本 PR 不处理（Danis 明确接受）。
