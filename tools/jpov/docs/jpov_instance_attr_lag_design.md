# JPOV 人物 instancing per-instance attribute 布局重排 + 布料 LAG 副运动设计

> 目的：在不新增骨骼、不改 pose atlas 结构的前提下，给蒙皮实例（人物 / 衣物）加**惯性滞后副运动
> （LAG / secondary motion）**，并为此**重排 per-instance attribute 的 16 个槽**，用「一个槽 = 4 float
> / 8 short / 16 byte」的三视图约定把信息量压进去。
>
> 状态：**设计稿**（2026-10-08 与 Danis 连续推演收敛；本文档只描述设计，尚无实现代码）。
> 相关：`docs/jpov_crowd_instancing_arch.md`（instancing 架构锚点）、
> `docs/jpov_dqs_skinning_design.md`（DQS 蒙皮）、`docs/jpov_partial_rotation_design.md`（部位额外旋转）、
> `docs/jpov_crowd_body_shape_face_design.md` §3（部位粗细）。
> 代码锚点：`src/instance_buffer.h`（`InstanceAttrSpec` / `InstanceBuffer`）、
> `src/skeleton/skinning_shader.h`（`kSkinnedVs` / `kSkinnedVsShadow`）、
> `src/skeleton/skeleton_renderer.cc`（`DrawSkinnedMesh` / `UploadSkinningInstanceAttributes`）、
> `interface/skeleton_types.h`（`SkinnedInstanceState`）、`src/texture_units.h`（纹理单元分配）。

---

## 0. TL;DR

- **目标**：一件衣服（斗篷）上部顶点绑骨不参与摆动、下部顶点**随玩家运动实时甩动**；
  **不新增骨骼**，LAG 只作用于**骨骼层**（pose 与摆放矩阵）。
- **手段**：给每个实例送**两套 pose**（`pose` 当前 + `pose_lag` 滞后）与**两套 partial rotation**，
  VS 里按**逐顶点「松弛度」**对两者蒙皮结果做 `mix`。`pose_lag` 的生成方式（历史回放 / 一阶滤波 /
  二阶）**完全在接口外由用户决定**——接口只做 lerp，故天然兼容延迟 / 一阶 / 二阶三种副运动。
- **约束**：不新增顶点属性以外的槽扩张；per-instance attribute 仍是 **16 槽硬上限**（loc0..15），
  本次把 loc6 空出来给**顶点属性**（松弛度），per-instance 用满 loc7..15。
- **三视图约定**：**一个 attribute 槽 = 16 byte = 4 float = 8 short(half) = 16 int8**。
  存储统一按 `uvec4`（4×uint32=16B）上传，shader 侧按视图解包。
- **可维护性铁律**：编码表 + 解码逻辑**单一来源、编解码相邻**，并配**编解码往返测试**。

---

## 1. 背景与需求（为什么是 LAG）

### 1.1 场景

一件斗篷：上部顶点绑骨（挂肩）**不参与摆动**；下部自由顶点要在游戏中**随人物动作/速度实时甩动**。
驱动量是玩家的运动速度 / 加速度（逐帧变）。

### 1.2 为什么是「骨骼层滞后」而不是「顶点层」

- 顶点层 = 每顶点独立求解，**无法落地千人**（CPU/带宽爆炸）。
- 骨骼层 = 滞后发生在「骨骼运动」层面，状态量 = 骨数，**天然摊薄到千人**（每个实例送少量
  索引即可）。**本设计走骨骼层。**

### 1.3 为什么优先「纯延迟（回放历史 pose）」而非滤波

- **纯延迟**：`pose_lag = pose(now - delay)`，从历史环形缓冲回放。实现最简、零积分状态、绝对稳定。
- **一阶滤波**：`S1_dot = -k(S1-S0)`，平滑突变、无过冲。
- **二阶 / Verlet**：有过冲与摆动。
- **关键洞察**：三者**只差「`S1` 如何演进」，接口（`mix(S0, S1, 松弛度)`）完全一样**。
  故接口**不写死**任何一种，`pose_lag` 由用户提供 ⇒ **接口天然支持一阶 / 二阶**（用户能存一阶的
  state 就存得下二阶的两个 state）。

### 1.4 已接受的取舍（Danis 拍板）

- **一件衣服只能适配一种 LAG**（一个实例一组 `pose_lag` / 部分旋转），不做「同件衣服既要又要」。
  **可接受。**
- **不做帧间插值的「纯延迟」语义更纯**；本设计仍给 `pose_lag` 配 a/b/ratio 两帧插值（见 §4），
  由用户决定 ratio 是否为 0/1（单帧回放）。

---

## 2. 两条硬约束（先记住）

### 2.1 槽位硬上限：per-vertex + per-instance 共 16 槽

GL 保证 `GL_MAX_VERTEX_ATTRIBS ≥ 16`。**顶点属性与实例属性共享同一套 location 空间**（同一张 VAO）。
当前占用：

| loc | 归属 | 名称 | 现状 |
|---|---|---|---|
| 0 | 顶点 | `aPos` | 现用 |
| 1 | 顶点 | `aNormal` | 现用 |
| 2 | 顶点 | `aTexCoord` | 现用 |
| 3 | 顶点 | `aJoint` (ivec4) | 现用 |
| 4 | 顶点 | `aWeight` (vec4) | 现用 |
| 5 | 顶点 | `aTangent` | 现用 |
| 6 | 实例 | `aInstModel` col0 | 现用（本次挪到 7） |
| 7 | 实例 | `aInstModel` col1 | 现用（本次挪到 8） |
| 8 | 实例 | `aInstModel` col2 | 现用（本次挪到 9） |
| 9 | 实例 | `aInstModel` col3 | 现用（本次挪到 10） |
| 10 | 实例 | `aInstPose` (vec3) | 现用（本次改造） |
| 11 | 实例 | `aInstThick0` | 现用（本次并入 loc12） |
| 12 | 实例 | `aInstThick1` | 现用（本次并入 loc12） |
| 13 | 实例 | `aInstPartial0` | 现用（本次挪到 13） |
| 14 | 实例 | `aInstPartial1` | 现用（本次挪到 13） |
| 15 | — | 空闲 | 本次启用 |

**⇒ 只剩 loc15 一个空槽 + 可以「腾挪 + 压缩」现有槽。这就是本次布局重排的全部动机。**

### 2.2 纹理单元也到顶了

`src/texture_units.h` 现分配 **0..15 全部用满**（tile / 材质×6 / shadow×5 / pose atlas / thickness /
partial×2）。**⇒ 本次不能靠「新增配置纹理」省槽**（VS 侧 `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` 通常仅 16，
已无余量）。**必须纯靠 attribute 内部压缩。**

---

## 3. 三视图约定（存储压缩的基础）

### 3.1 一个槽的三种视图

**一个 attribute 槽 = 16 byte，可作三种视图使用：**

| 视图 | 类型 | 容量 | 适用 |
|---|---|---|---|
| **A** | `vec4` (float32) | 4 个 float | 全精度：坐标、矩阵、flat id |
| **B** | 8 × half / int16 | 8 个 16-bit | 中精度：flat id（若 <65536）、坐标、比例 |
| **C** | 16 × int8 | 16 个 8-bit | 低精度：颜色索引、粗比例 |

### 3.2 上传方式（关键，别踩坑）

- **不要**用「8 个 half 分量」声明 attribute —— **GL 单个 attribute 最多 4 分量**。
- **正确姿势**：把整个 16 byte 声明为 **`uvec4`**（4×uint32）走 **`glVertexAttribIPointer`**，
  shader 侧自己解包：

```glsl
layout(location = N) in uvec4 aPack;   // 4×uint32 = 16 byte = 一个槽

// 视图 A：bitCast 回 float
float f = uintBitsToFloat(aPack.x);

// 视图 B：解出 half（2 个/uint32）
vec2 h01 = unpackHalf2x16(aPack.x);    // 低 16bit → x，高 16bit → y

// 视图 C：解出 int8（4 个/uint32）——⚠️ 必须用无符号移位
int b0 = int( aPack.x        & 0xFFu);
int b1 = int((aPack.x >>  8) & 0xFFu);
int b2 = int((aPack.x >> 16) & 0xFFu);
int b3 = int((aPack.x >> 24) & 0xFFu);
```

> ⚠️ int8 解包**唯一要小心**：用 `uint` 做右移，先 `& 0xFFu` 再转 int，否则高位符号扩展出错。

### 3.3 精度警示（定点 vs 浮点）

- **int8 / 定点是「绝对均匀刻度」**（如 thick 0.01~2.5，256 级 ⇒ 步长 ~0.0097），**不是相对精度**。
- **half 是「相对精度」**（尾数 10 bit，值越大绝对误差越大）。**小值域用它安全，大范围坐标慎用。**
- **相减/抵消场景**（两个大数求差）**禁用 half**：会灾难性抵消。⇒ LAG 的相对坐标必须小值域（§6 loc15）。

### 3.4 half 与 float 混合运算（澄清，非坑）

- **桌面 GL 3.3 没有原生 `half` 算术类型**：`unpackHalf2x16` 返回 `vec2`（float），
  之后是纯 float 运算。**「half × float」根本不存在类型坑。**
- **half 的误差在解包那一刻就注入**（输入精度），运算本身不再额外降精度。
- （未来上 GLES：`mediump` 才是真坑，需对坐标/矩阵显式 `highp`。）

---

## 4. 装置接口：双 pose + 双 partial + 逐顶点松弛度

### 4.1 顶点最终位置

```
世界位置 =
  mix( SkinnedAt(pose,      partial,      Model),
       SkinnedAt(pose_lag,  partial_lag,  Model + PosLag),
       松弛度 )                                  // 松弛度 ∈ [0,1]，逐顶点
```

- `pose` / `pose_lag`：两套 pose（各 `{a, b, ratio}`，DQS 插值后蒙皮）。
- `partial` / `partial_lag`：两套部位额外旋转（各 2 个模型系四元数）。
- `Model` / `Model + PosLag`：摆放矩阵；**位置滞后只作用于平移列**（+`PosLag`，见 §6 loc15），
  旋转/缩放不滞后。**transform 不给两份。**
- `松弛度`：**逐顶点**属性（loc6），`0` = 完全跟随当前 pose（上部绑骨），`1` = 完全滞后（下部自由）。
  **该属性是「每顶点」而非「每实例」——别放错。**

### 4.2 为什么接口能兼容一阶 / 二阶

`pose_lag`（及其 ratio / partial_lag / PosLag）**是用户逐帧演进的状态**。接口只负责 `mix`：

| 用户怎么演进 `pose_lag` | 得到的效果 |
|---|---|
| `pose_lag = pose(now - delay)`（历史回放） | 纯延迟（无过冲） |
| 一阶滤波 | 平滑拖尾、无过冲 |
| 二阶 / Verlet | 摆动、过冲 |

**⇒ 接口对状态演进方式透明；用户能做一阶，就必然能做二阶（只需多存一个 state）。**

### 4.3 平动参考系修正（「人在船上」）

纯延迟活在**世界坐标系**里，会误判「人在平动」。**必须用相对速度驱动延迟量**：

```
relative = world_velocity(bone) - character_velocity      // (+ 可选 wind_velocity)
延迟量 ∝ f(relative, wind_velocity - character_velocity)
```

- 甲板上（顺风）：relative ≈ 0 ⇒ 不飘（对）。
- 船舱里（避风）：相对风为 0 ⇒ 不飘（对）。
- 用户只需**额外给一个 `character_velocity`（vec3）**；要更完整再加 `wind_velocity`。

> 这是**正确抽象**，不是 hack：**LAG 应建立在「相对风速」上，而非世界速度。**

### 4.4 逐顶点限幅（防「时间性拉伸」）

逐顶点按不同延迟采不同时刻 pose ⇒ 相邻顶点被「时间梯度」拉开 ⇒ **布被拉长**。必须：
- **位移限幅**：`offset = clamp(pose_lag(v) - pose(v), ±maxlen)`（兜底，治拉伸）。
- **延迟系数在网格上平滑**（低频，别一个顶点 0 隔壁 1）。

---

## 5. 暂不解决 / 已知边界

| 项 | 结论 |
|---|---|
| 一件衣服多套 LAG | **不做**（一种 LAG per garment）。 |
| LAG 的「摆动/余摆」 | **纯延迟给不了**，需用户自行演进成二阶；接口支持。 |
| 平动参考系 | 需用户提供 `character_velocity`（+ 可选 `wind_velocity`）。 |
| 每顶点限幅 | 需在 VS 内实现 clamp（§4.4）。 |
| 纹理方案 | **不可用**（纹理单元已满，§2.2）。 |

---

## 6. 最终 per-instance attribute 布局（本次定稿）

### 6.1 全槽表（loc0..15 用满，16 槽）

```
顶点属性（每顶点）:
  0  aPos        vec3
  1  aNormal     vec3
  2  aTexCoord   vec2
  3  aJoint      ivec4
  4  aWeight     vec4
  5  aTangent    vec3
  6  aRelax      ← 【本次新增】松弛度 [0,1]，1 float（槽 16B，余量留给未来"类松弛信息"）

per-instance attribute（divisor=1）:
  7–10  aInstModel (mat4)                       4×vec4        = 64 B
  11    aInstPoseIds  (uvPack)                  4×float       = 16 B  ← 见 6.2
  12    aInstMisc     (uint8 视图)              16×int8       = 16 B  ← 见 6.3
  13    aInstPartial  (8 half)                  8×half        = 16 B  ← pose 的 2 个四元数
  14    aInstPartialLag (8 half)                8×half        = 16 B  ← lag 的 2 个四元数
  15    aInstPosLag   (8 half，用 3)            8×half        = 16 B  ← lag 相对坐标 (dx,dy,dz)
```

### 6.2 loc11：4 个 flat id（float32）

| 分量 | 含义 |
|---|---|
| `.x` | `pose.a` 的 flat id |
| `.y` | `pose.b` 的 flat id |
| `.z` | `pose_lag.a` 的 flat id |
| `.w` | `pose_lag.b` 的 flat id |

- **flat id = `pose_idx × bone_count × 2`**（pose 在 atlas 的**平坦 texel 起点**，见
  `skinning_shader.h` / `skeleton_manager.cc` L233）。
- **flat id 范围 = [0, ~4,194,304)**（atlas 2048² 总 texel，与骨数无关）。
- **必须 float32**：4.19M < float32 整数精确上限 2²⁴≈16.7M，**刚好够**；**绝不能压成 int16**
  （65535 上限早爆）。
- ⚠️ **备选压缩**：若将来槽位告急，可改存 **`pose_idx` 序号**（而非 flat），shader 里
  `flat = pose_idx × uBoneCount × 2`（`uBoneCount` 是 uniform，乘法免费）；序号 < 65536 时可用 int16。

### 6.3 loc12：uint8 视图（16 byte 严丝合缝）

| 偏移(byte) | 字段 | 编码 |
|---|---|---|
| 0 | `ratio` (pose) | `uint8`，`/255` → [0,1] |
| 1 | `ratio_lag` (pose_lag) | `uint8`，`/255` → [0,1] |
| 2–9 | `thick[0..7]` | `uint8`，`0.01 + v/255 × 2.49` → [0.01, 2.5]，**步长 ~0.0097** |
| 10–12 | `color0` (RGB888) | R,G,B 各 1 byte |
| 13–15 | `color1` (RGB888) | R,G,B 各 1 byte |

- **total = 1 + 1 + 8 + 6 = 16 byte** ✅。
- **thick 精度写死在注释**：范围 `[0.01, 2.5]`，256 级，步长 ~0.0097，**再大变形由用户自定义
  其它通道 / 不做**（Danis 拍板）。
- **color = RGB888 真彩**（两个肉眼可辨的配色），**不是** 256 色调色板索引。⚠️ 3 byte/色**跨 uint32
  边界**，解包位序要写对（`aPack.z/y/x` 组合）。

### 6.4 loc13：pose 的 2 个 partial 四元数（8 half）

- `partial_rotations[0..1]`（`kNumPartialRotation = 2`），**模型系单位四元数**（语义见
  `jpov_partial_rotation_design.md`）。
- **半精度够**（角度精度 ~0.1°）。⚠️ **解包后必须 `normalize`**：半精度量化后 norm² 会略偏 1，
  现有 `CHECK_GT(norm2, 0.25)` 防御**不要卡太死**。

### 6.5 loc14：lag 的 2 个 partial 四元数（8 half）

- `partial_rotations_lag[0..1]`：**lag 状态下的部位额外旋转**。
- **partial 要考虑 LAG**（Danis 澄清）⇒ **本槽保留**，与 loc13 同构。

### 6.6 loc15：lag 相对坐标（8 half，用 3）

- 前 3 个 half = `(dx, dy, dz)`：**lag 位置相对锚点/模型的局部偏移**。
- VS：`lag_world_pos = Model * (pos + vec3(dx,dy,dz))`（或按 4.1 的 `Model + PosLag` 语义）。
- ⚠️ **坐标架 = 局部/锚点系，不是世界系**：half 需小值域 + 避免相减抵消（§3.3）。
  **待钉点**：明确 `[min,max]` 量级（见 §8 TODO）。
- **剩余 10 byte 预留**（写注释）。

---

## 7. 实施要点（host + shader）

### 7.1 host 侧（`instance_buffer.h` / `skeleton_renderer.cc`）

- 更新 `InstanceAttrSpec` 常量（loc7..15 的视图 / stride）。**这是 host↔shader 唯一 layout 约定点。**
- `UploadSkinningInstanceAttributes`：按新布局填充（flat id → float；misc → 打包 uint8；partial → half；
  poslag → half）。
- **开关式零回归**（照抄现有 `uThicknessEnabled` / `uPartialEnabled` 模式）：
  ```
  lag_on = (本批有非零松弛度 / 有 lag 配置) ? 1 : 0
  ```
  - `lag_on = 0` 时：**不 Attach loc14/loc15 的 instance buffer**（`InstanceBufferBinding` 列表按需），
    shader 里 `if (uLagEnabled == 0)` **整段跳过**第二遍 DQS / partial / mix。
  - uniform 分支全批一致，**零 warp 分化代价**；无 lag 模型 **overhead ≈ 一次 uniform 上传**。
- **数据契约**：`SkinnedInstanceState` 扩展（`pose_a_lag` / `pose_b_lag` / `ratio_lag` /
  `partial_rotations_lag` / `pos_lag`）；越界仍 **LOG(FATAL)/批次剔除**（不静默读 atlas 别人）。

### 7.2 shader 侧（`skinning_shader.h`）

- ⚠️ **主 pass VS（`kSkinnedVs`）与 shadow pass VS 必须逐字同公式**（PR #104 教训：主/阴影分叉 ⇒
  影子错位）。改一处必须同步另一处。
- 新增输入：loc6 `float aRelax`；loc11 `uvec4 aInstPoseIds`（或 vec4 float，按 §6.2 用 float32）；
  loc12 `uvec4 aInstMisc`；loc13/14/15 `uvec4`（half 解包）。
- 新增 uniform：`uLagEnabled`；DQS 主/阴影两遍 + partial 两遍 + mix。

### 7.3 编解码单一来源（可维护性铁律）

> Danis 判断：**「只要编码表写清楚、记好注释，一般工程师能 handle」** —— 成立，**但需满足三条**：

1. **编解码相邻**：打包与解包逻辑放**同一头文件**（或紧邻），shader 的 offset 注释**指向该文件**。
2. **偏移单一来源**：bit/byte 偏移写成**命名常量**（`kThickByteOffset = 2` 等），host 与 GLSL
   **引用同一命名**，禁止各自写裸数字（避免 PR#104 在 bit 层面的重演，且更难查）。
3. **往返测试**：新增 `instance_pack_test`：**打包 → 解包 → 断言（在量化误差内）**。
   **这是把「位打包」从天才代码降级为普通代码的关键**——正确性由测试保证，改布局的人敢改。

---

## 8. TODO（封版前需钉死）

| # | 待钉点 | 影响 |
|---|---|---|
| 1 | **loc15 坐标架量级**：明确 `(dx,dy,dz)` 的 `[min,max]`（建议锚点局部偏移，小值域） | half 精度 |
| 2 | **loc15 剩余 10 byte**：预留注释 | 可读性 |
| 3 | **loc12 color 3-byte 跨 uint32 解包**：写清位序 | 正确性 |
| 4 | **flat id 用 float32**（§6.2）确认；或改存 `pose_idx` + uniform 乘法 | 精度/槽位 |
| 5 | **thick 精度下限 0.0097** 写死注释 | 可维护性 |
| 6 | **编解码同源文件 + 往返测试** 落地 | 可维护性 |
| 7 | **开关式零回归**（`uLagEnabled` + 不 Attach 未用 buffer）对照实现 | 零回归 |

---

## 9. 相关文档

- `docs/jpov_crowd_instancing_arch.md` — instancing 架构锚点（千人规模、共享骨架、pose atlas）。
- `docs/jpov_dqs_skinning_design.md` — DQS 蒙皮（每骨 2 texel，DLB 刚体混合）。
- `docs/jpov_partial_rotation_design.md` — 部位额外旋转（本设计 loc13/14 的语义来源）。
- `docs/jpov_crowd_body_shape_face_design.md` §3 — 部位粗细（本设计 loc12 thick 的语义来源）。
- `src/instance_buffer.h` — attribute 布局常量（唯一约定点）。
- `src/texture_units.h` — 纹理单元分配（已满，本设计因此走纯 attribute 压缩）。
