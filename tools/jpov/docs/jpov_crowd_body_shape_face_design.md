# JPOV 人群 per-instance 体型与表情（整体 scale / 骨通道膨胀 μ / 「脸是装备」）— 设计

> 目的：给「1000 人密度」补上两条**个体差异来源**——**体型**（高矮 + 胖瘦）与**近景表情**——
> 同时不破坏人群架构的基准：「一份共享骨架 + N 种共享 mesh + 每 instance 只 carry 少量
> selector/标量，一次 instanced/batch draw」。
>
> 状态：本文是**设计定稿**（2026-09-23 与 Danis 逐条收敛）。**对应 PR 只记设计、不含实现**，
> 实现清单见 §6。
>
> 相关：`jpov_crowd_instancing_arch.md`（§2 廉价差异表 / §4 part 模型 / §6.1 第一坑 / §6.3 LOD 归用户）、
> `jpov_dqs_skinning_design.md`（DQS 蒙皮）、`jpov_skeleton_manager_design.md`（pose atlas）、
> `interface/skeleton_types.h`（`SkeletonType` / `InstanceTransform` / `SkinnedInstanceState`）。

---

## 0. 结论速览

| 需求 | 落点 | 机制 | 每实例成本 |
|---|---|---|---|
| **高矮** | 实例级 | 复用 `InstanceTransform::scale`（**零新机制**） | 0（字段已在） |
| **胖瘦 / 部位粗细** | 骨架级定义 + 实例级取值 | **骨通道膨胀 μ**：蒙皮前对 rest 顶点做「骨局部系横径缩放」 | 2×vec4 = 8 自由度 |
| **长度比例**（腿长/上下身比） | —— | **明确不做**，由整体 scale 替代 | 0 |
| **脸型/五官（千人静态）** | 实例级 selector | **「脸是装备」**：`part_pool` 头部变体 + 独立 selector | 几个 int |
| **表情（近景）** | CPU 侧 + 单独 draw | 近邻 K 张脸：CPU 微操 rest 顶点 + 共享骨架 DQS 蒙皮 | K≈10，退出批 |

三条设计主线：

1. **体型变换不能挂 per-mesh**（一个骨架配 N 种 mesh）⇒ 只能定义在**骨局部坐标系**上。
2. **膨胀 μ 必须作用在 `v_rest`（顶点）上，不能塞进对偶四元数**（DQ 无缩放）。
3. **脸走装备池廉价做千人，表情只给近邻少数**（LOD 归用户，见 §6.3）。

---

## 1. 背景与已定约束

### 1.1 为什么「变换约定」只能落在骨架侧（Danis 2026-09-23 提出）

**一个骨架要适配 N 种 mesh ⇒ 顶点变换的约定不可能 per-mesh** ⇒ 唯一可挂的地方是
**骨局部坐标系**（原点 = 关节、**局部 +Y = 骨长轴**；JPOV 已定：`rest_offset` 沿 +Y，
`bind_rotation` 管朝向，见 `skeleton_types.h`）。

mesh 只提供 `JOINTS_0`（loc3）/ `WEIGHTS_0`（loc4）——蒙皮 mesh 必然有 ⇒ **骨架级定义的任何
顶点映射自动适配任意 mesh，不需要 per-mesh 约定**。

> **通用性的来源不是"更聪明的顶点映射"，而是"参数定义在骨局部坐标系的轴/原点上"。**
> 「胖瘦」与「长度比例」通用性强，正因为它们就是骨局部系的**两个本征方向**：**横径 μ、沿轴 λ**。

### 1.2 与现状对齐的三条事实（已核实）

| 事实 | 位置 |
|---|---|
| `inverse_bind` 是**派生量**，`SkeletonManager` 构造时用 `ComputeInverseBind()` 现算 | `src/skeleton/skeleton_manager.cc:121` |
| glTF 加载**不再读** `skin.inverseBindMatrices`（外部资产的 IBM 共轭另议） | `src/gltf_loader.cc:960` |
| 蒙皮 VS 里**拿不到** `p_j`（关节 bind 位置）与 `R_bind_j`——IBM 已折进 pose atlas（方案甲） | `src/skeleton/skinning_shader.h` |

⇒ 要实现膨胀 μ，**必须补一份骨架级静态表**（`p_j` + `R_bind_j`），见 §3.4。

### 1.3 一条铁律：变换必须「逐顶点插值良定义」

父/子骨参数不同时（如大腿 μ=1.3、小腿 μ=1.0），跨关节的顶点权重混合区必须平滑。
因此所有顶点映射都表达成**「每骨算子作用于该骨偏移」，再按权重插值**（**不是**矩阵混合）：

```
v' = Σ_i w_i · f_bone_i(v)
```

---

## 2. 整体 scale = 高矮（per-instance，零新机制）

- **载体**：`InstanceTransform` 已有 `float scale`（`interface/skeleton_types.h:195`，语义：
  **先缩顶点、再旋转平移**）。千人按实例传即可，**不需要新字段、不需要新 attribute**。
- **语义**：等比缩放 ⇒ **高的人自然更粗**；要「又高又瘦」就配 μ（`scale = 1/k` 配 `μ = k`）。
- **注意**：整体 scale 会改变脚相对地面的位置 ⇒ 用户侧要按 scale 抬根高度（否则入地/悬空）。
  这与既有 `Object3DCommand::scale` 的注意点相同。
- **决策（Danis 2026-09-23）**：**「长度比例」不做**，用整体 scale 替代即可获得高矮区分。
  理由：λ 会改关节位置 ⇒ ① 每骨每 pose 的 `jointWorld·IBM` 都要重烘（千人共用一张 atlas 的前提下
  per-instance 不可行）；② 牵动影子/光空间 AABB/脚落地/动画接触点。
  若将来真需要，正确落点是**骨架级分档**（参数化生成 N 档 `SkeletonType`，每档一个 batch），
  不是 per-instance 连续值。

---

## 3. 骨通道膨胀 μ = 胖瘦 / 部位粗细（per-instance）

### 3.1 语义与公式

**膨胀 = 在骨局部坐标系里对"顶点相对关节的偏移"做横径缩放**（只缩横截面，保留沿骨轴长度）。
几何上等价于「绕**过关节的骨轴直线**做径向缩放」——像给骨头套一只变粗的袜子。

```
o     = v − p_j                                  // p_j: 该骨 bind 位置（骨架级静态表）
o_loc = R_bind_jᵀ · o                            // 转骨局部系：局部 +Y = 骨长轴
o_loc = vec3(o_loc.x · μ, o_loc.y, o_loc.z · μ)  // 只缩 XZ，保留 Y（长度不变）
v'    = p_j + R_bind_j · o_loc
```

多骨（4-bone）按权重插值：

```
v' = Σ_i w_i · ( p_j + R_bind_j · S(μ_j) · R_bind_jᵀ · (v − p_j) ),   S(μ) = diag(μ, 1, μ),  j = joint(i)
```

**为什么"只缩横径"而不做各向同性**：各向同性会把沿骨轴的肉顶到关节之外（肉越过关节）。
前者 = 「腿粗一圈但长度不变」，后者 = 「肉往两头鼓」。

### 3.2 为什么作用在 `v_rest` 上（三条论证）

| 备选 | 为什么不行 |
|---|---|
| 塞进**对偶四元数** | DQ 只能表达刚体（旋转 + 平移）。塞缩放会破坏 `\|q\|=1` 与对偶部约束 → 蒙皮塌陷/漂移。 |
| 改 **`rest_offset`（骨长）** | `rest_offset` 一变，`jointWorld` 与派生 `inverse_bind` 同步变 ⇒ **bind pose 下 `M = JW_bind·IBM = I`，静止画面零变化**；且 pose atlas 要重烘（§2 已论证）。 |
| **改 rest 顶点（本文采用）** | 等价于「换一张 rest mesh」：蒙皮公式一个字不改，`μ≡1` 时数学上是恒等（零回归的保法见 §3.3-4）。 |

**配对关系仍成立**（见 `jpov_retarget_design.md` §5.5）：μ 只缩「顶点相对关节的偏移」，
`p_j` 不变 ⇒ IBM 不需要变、关节坐标系不变 ⇒ 顶点在新 rest 下仍正确挂在关节 j 的 bind 帧里；
**姿势一动，变粗的肉跟着骨头走**。

### 3.3 与 DQS 的关系（Danis 2026-09-23 问「我们用的是 DQS，这套公式该不会有问题吧？」）

**正交，互不干扰。** DQS 运行时只吃两样：`v_rest` 和折好 IBM 的 DQ（`jointWorld(pose)·IBM`）；
μ 只改前者。关键在 **DLB 的混合发生在 DQ 空间**：

```
q̂_blend = Σ w q̂ / |Σ w q|            // 混合的是「变换」，与顶点无关
v_posed = q̂_blend ⊗ v ⊗ q̂*_blend
```

DQS 唯一的退化风险来自 `|Σ w q| → 0`，而它**只取决于 DQ 与权重，与顶点位置无关**
⇒ **μ 把顶点吹多大都不会让 DQS 的混合质量变差**。

顺序固定为：

```
v_rest --[μ：逐顶点插值]--> v'_rest --[DQS 蒙皮]--> v_posed
```

μ 那一步**只含缩放、不含旋转** ⇒ 逐顶点插值天然平滑，无 LBS candy-wrapper 类塌缩。

**⚠️ 四处必须小心（都属"非位置"的派生量）**：

1. **绝不能把 μ 塞进 DQ**（见 §3.2）。
2. **法线**：横径缩放不是均匀缩放 ⇒ 在骨局部系用**逆转置**，再交给 DQS 的旋转部：
   `n' = normalize(vec3(n.x/μ, n.y, n.z/μ))`；或走「变形后重算 N」（`interface/mesh_geometry.h`）。
   混合区（膝关节/髋关节）用"平均逆转置"是**近似**，需图像验收。
3. **AABB / 光空间 AABB / 剔除要放大**：μ>1 会让身体变粗，阴影覆盖与剔除若仍用旧 AABB 会错。
   简单做法：按 `max μ` 保守放大。**shadow pass 必须与主 pass 同一公式**（同 `jpov_crowd_instancing_arch.md` §6.2 的 4-pass 一致原则）。
4. **零回归要靠开关，不能靠“乘 1”**：`p_j + R_bind·(R_bindᵀ·(v−p_j))` 数学上是恒等，
   但数值上是「旋转→反旋转」，会有 ~1e-7 的浮点噪声 ⇒ **不能靠 μ≡1 保逐字节零回归**。
   正确做法：骨架级一个 **uniform 开关**（如 `uShapeEnabled`，无通道表时为 false）
   ⇒ 旧骨架/旧场景**直接跳过整段 shape 运算**，逐字节不变；新场景 μ≡1 时允许容差级 diff。

### 3.4 数据模型：骨架级定义 + 实例级取值

**骨架级（静态，一次上传，跟 `SkeletonType`/`SkeletonManager` 走）**

```cpp
// 通道划分：一个 SkeletonManager 一种编法（Danis 2026-09-23 定）。
// 空 = 该骨架不做体型膨胀（全部按 μ=1）→ 天然零回归、向后兼容。
struct SkeletonShapeChannels {
    std::vector<uint8_t>     bone_channel;   // size == bone_count；每骨一个通道号
    std::vector<std::string> channel_names;  // size == channel_count；给面板/配置/调试用
    int  channel_count() const;
    void Validate() const;  // bone_channel.size()==bone_count、每值 < channel_count、
                            // channel_count <= kMaxShapeChannels；违规 LOG(FATAL)
};
```

人体骨架示例（7 通道）：`Hips`→ch0（胯）、`Spine/Spine1/Spine2`→ch1（腰）、
左腿三骨→ch2、右腿三骨→ch3、左臂三骨→ch4、右臂三骨→ch5、`Neck/Head`→ch6。

⚠️ **通道划分本身是 JPOV 用户的事**，不是 JPOV 的语义：JPOV 只提供「表结构 + `Validate()`」
（骨→通道、通道名表）；**分几个通道、哪根骨进哪个通道由用户/资产决定**。
上面那 7 通道只是 demo 的一个候选分组（具体待定）。

- 上传形态：`uniform int uBoneChannel[N]` —— GLSL 330 的 uniform 数组支持**动态索引**，直接可用。
- 另需每骨静态几何：`uniform vec3 uBindPos[N]`（`p_j`）+ `uniform vec4 uBindRotQ[N]`（`R_bind_j`
  = 已有 `bind_rotation`，**目前尚未上传到 VS**，需补）。
  （若只要"各向同性膨胀"，可省 `R_bind_j`，但会引入 §3.1 的"肉顶过关节"问题，不推荐。）

**实例级**

```cpp
// 每实例的通道系数（膨胀自由度）。
struct InstanceShape {
    std::array<float, kMaxShapeChannels /* = 8 */> channel_mu;  // 每个通道的横径比，默认全 1
};
```

**VS 侧（要点：查表 → 取系数 → 逐骨算偏移 → 按权重混合 → 再喂 DQS）**

```glsl
vec3 vp = vec3(0.0);
for (int i = 0; i < 4; ++i) {
    int   bone = aJoint[i];
    float mu   = aInstMu[uBoneChannel[bone]];        // 骨 index → 通道号 → per-instance 系数
    vec3  o    = aPos - uBindPos[bone];
    vec3  ol   = rotateT(uBindRotQ[bone], o);        // 转骨局部系（+Y = 骨长轴）
    ol         = vec3(ol.x * mu, ol.y, ol.z * mu);   // 只缩横径
    vp        += aWeight[i] * (uBindPos[bone] + rotate(uBindRotQ[bone], ol));
}
// vp 交给既有 DQS 蒙皮
```

**四条设计约束**

1. **顶点属性零新增**：通道号由已有的 `aJoint`（loc3）查表得到，不需要新的顶点数据。
2. **一个骨架一个编法**：通道的**语义**（"这个通道叫腰"）是骨架 metadata；
   人形 7 通道、马骨架自行划分、人群低模可以 3 通道甚至 **0 通道**（= 不做体型）。
3. **无表 = 全 1** ⇒ 旧骨架/旧路径零回归，**不需要在接口上开"支持/不支持"的开关**（通道表缺省即表达"不做"；渲染侧对应 `uShapeEnabled=false`）。
4. **接口与存储解耦**：接口固定为「骨架级表 + 实例级系数数组」；存储先走 attribute（§3.5），
   将来通道数变大只换上传路径为 **per-instance params texture**：
   `texelFetch(uInstMu, ivec2(channel, gl_InstanceID))`（渲染已是 `glDrawElementsInstanced`，
   `gl_InstanceID` 现成，通道数不封顶、attribute 零成本）。

### 3.5 attribute 槽预算与带宽（Danis 2026-09-23 定：就 2 个 vec4）

| 属性 | 内容 | 字节 |
|---|---|---|
| loc6..9 | `aInstModel` 4×vec4 | 64 B |
| loc10 | `aInstPose` vec3 | 12 B |
| **现状合计** | | **76 B/实例** |
| loc11..12（新增） | 膨胀系数 2×vec4 = **8 个自由度** | +32 B |
| **合计** | | **108 B/实例** |

- **带宽不是瓶颈**：108 B/实例 × 1000 人 = 108 KB/帧，一次上传。
- **真正的硬约束是 attribute slot 数**：`GL_MAX_VERTEX_ATTRIBS` 保证 16；**这 16 个 location 是
  “顶点属性与实例属性共用的一个编号池”**（mesh 几何占 loc0–5：pos/normal/uv/joint/weight/tangent），
  当前 per-instance 部分占 loc6–10（摆放矩阵 6–9 + pose 选择 10）⇒ 已用 11 个，
  加 2 个 → 13 个，**剩 3 个余量**。
- **决策**：**2 个 vec4 专供膨胀自由度**（8 个通道，当前用 6~7 个，余 1~2 个备用）；
  其余功能"挤一挤"（`aInstPose` 的 `.w` 目前空着，slot 真紧张时可用，但正常情况建议独立 location，
  语义清楚）。
- **备用技巧（slot 实在不够时）**：把一个 float 的 32 位**拆成 4×8bit**（`floatBitsToUint` +
  移位/掩码，再线性映射到 μ 的有效区间如 `[0.5, 2.0]`，分辨率 ≈ 0.006，足够），
  ⇒ 1 个 vec4 = **16 个通道**。注意 GLSL 330 **没有** `unpackHalf2x16`（那是 GLSL 400），
  所以走位运算手工解包；代价是几条位运算指令，**不增加实质算力**。

### 3.6 验收门禁

| 门禁 | 判据 |
|---|---|
| **零回归** | 骨架无通道表（`uShapeEnabled=false`）⇒ gold 图**逐字节不变**；有通道表但 `μ≡1` ⇒ 只允许浮点噪声级容差（§3.3-4） |
| **量化正确性** | `μ > 1` 时量测某截面直径应 ≈ ×μ（选权重≈1 的区段）——把"胖了多少"变成数字，不靠眼睛 |
| **混合区** | 膝/髋/肩处出近距离 gold 图，确认无捏腰/鼓包 |
| **4-pass** | shadow / picking / highlight 与主 pass 同公式（含 AABB 放大） |

### 3.7 已知取舍（明确记下，防返工）

1. **衣服/装备自动同比跟随**：μ 是"蒙皮前的顶点算子、按骨通道生效"，与 mesh 身份无关 ⇒
   绑同一骨架的衣服/装备**自动按同一 μ 膨胀**，不会豁开（这是 `jpov_crowd_instancing_arch.md`
   §6.1「第一坑」的一大半解药，且比"整体 scale"更好：长度不变、可按部位）。
   **代价**：通道表是骨架级唯一一份 ⇒ **同一根骨上身体与衣服共用同一 μ，做不出"衣服比身体松"**。
   要更松得改衣服 mesh 本身，或另加 part 级静态倍数——建议先不做。
2. **参数 mesh 无关，效果 mesh 相关**：同一个 `μ(Hips)` 在不同 mesh 上膨胀范围不同（权重分布不同）。
   这是"骨架级参数供 N mesh"的固有代价。
3. **膨胀中心 = 关节位置**：对 `Hips` 而言 ≈ 骨盆中心高度（那个 ~1.14m 的根偏移），横向放大 = 腰胯变宽 ✓；
   若某资产把该关节 pivot 放得偏低，视觉上会像"屁股往下坠"。要精细 → 允许**每骨 pivot 覆盖/中心偏移**。
4. **"只膨胀臀部"的限制**：调的是"骨"不是"解剖部位"——绑到 `Hips` 的顶点还包括腰/胯/下腹/大腿根上半段，
   会一起膨胀。"只要臀、腰不动"靠 per-bone 通道**做不到**。实用做法：`μ(Spine*)<1` + `μ(Hips)>1`
   （缩腰 + 涨胯），完全在骨语义内。
5. **原本贴着的部位会互穿**（大腿内侧、手臂贴躯干）——无碰撞处理。
6. **更局部的膨胀**（只鼓臀大肌一块）需要**空间 mask**（球/椭球范围的逐顶点权重）。
   仍可做到 mesh 无关（mask 定义在骨架空间、相对某关节的位置范围），但复杂度上一个台阶，**建议延后**。

---

### 3.8 实现现状（as-built，2026-09-24 落地）

本节记「已实现」与上面设计稿的差异，避免后来者照 §3.4 的初期设想改代码。

- **接口**（收敛后的最终形状，见 `interface/skeleton_types.h` / `src/skeleton/skeleton_manager.h`）：
  - 骨架级：`SkeletonManager(type, poses, std::array<std::vector<int>, kNumThicknessGroup> thickness_scaling_config)`
    —— 每组一串**关节 index**（不是骨名；骨名→index 由调用方注册时定位，见 fbx viewer 的 `GlbJointIndexByName`）。
    ctor 立即校验（负数 / 越界 / 一骨两组 → LOG(FATAL)）；空组 = 不用，全空 = 该骨架不做粗细。
  - 实例级：`SkinnedInstanceState::thickness_scales`（每项默认 1.0 = 原样）。
  - 组数常量 `kNumThicknessGroup = 8`（= 2×vec4，见 §3.5）。
- **每骨 bind 位置/朝向不进公开接口**：由 SkeletonManager 自己从 `inverse_bind` 取逆得到，烘成一张
  `bone_count × 2` 的 RGBA32F 纹理（`GpuHandles::thickness_bind_tex`，0 = 不做粗细；texel 单元
  `kTexUnitThicknessBind = 13`）。**与 §3.4 的 `uBoneChannel[]/uBindPos[]/uBindRotQ[]` uniform 数组不同**：
  走纹理既不受顶点 uniform 分量预算约束（无骨数上限），也不必把内部量摆到公开接口上。
- **§3.4 一处修正**：`R_bind_j` **不是** `bind_rotation`（那是相对父的局部朝向），必须沿树复合到骨架空间
  （与 `inverse_bind` 同源）。写绑定时按纹理**行主序**（W=骨数、H=2；不是「同一骨两 texel 相邻」）。
- **零回归**：无配置的骨架 `uThicknessEnabled = 0` → 蒙皮 VS 的 shape 段整段跳过（不是「乘 1」）。
- **§3.3-3 的「AABB 放大」在本实现里不适用**：级联正交盒由相机视锥切片推导（2026-09-17 定，不并入任何
  物体 AABB），蒙皮路径也没有 tile culling ⇒ 变粗的几何不会被剔除/裁剪错。
- **未做**（留给后续门禁）：§3.6 的量化门禁（μ → 截面直径、混合区近距离 gold）目前只有人工像素计数验收；
  CPU 侧仅 `test/jpov_thickness_config_test.cc`（配置校验 + 默认值）。

---

## 4. 「脸是装备」+ 近邻动态表情

### 4.1 脸是装备（千人静态，Danis 2026-09-23 方案）

复用 `jpov_crowd_instancing_arch.md` §4 的 **charactor-part 模型**（肉体与衣物归一）：
**脸 = 一个绑同一骨架（`Head` 骨）的 part** + slot selector ⇒ **零新机制**。
（该文档 §2 表格早已写定："「4 副面孔」的脸型/头骨几何 → 共享 N 个 baked 头部 rest-mesh 变体 + `head_idx`"。）

**最大价值：组合数从"乘积"变"加和"** —— 身体变体池与脸变体池分开，
千人 = `body_idx × face_idx` 的廉价组合，**不必烘 1000 个完整的头**。

**建议拆成三个独立 selector**，别做成一个笛卡尔积大池子（否则池子爆炸 + 美术产能跟不上）：

| 维度 | 载体 |
|---|---|
| 脸型 / 骨相几何 | 共享 N 个 baked 头部 rest-mesh 变体 + `head_idx` |
| 表情基（静态默认表情） | 表情变体 + `face_expr_idx` |
| 肤色 / 五官贴图 | texture-array / 材质变体 + `face_tex_idx` + tint |

**硬前提**：脸 part 必须**绑同一骨架**（`Head` 骨），否则共享骨架的 DQS 蒙皮走不通。

### 4.2 近邻 K 张动态表情（CPU 侧微操 rest 顶点）

**做法**：只有**最近邻 K 张脸**（K≈10）拥有动态表情；这些头**退出 instanced 批、单独 draw**
（K 次 draw call，可忽略）。

- **量级**：一个头 ~1500–3000 顶点（人群低模 <1000 tri）⇒ K=10 时 ≈ **1.5–3 万顶点/帧**的 CPU 变形
  + 十几 KB 上传 ⇒ 当前预算内可忽略。
- **落点必须与 μ 一致**：改的是 **rest(bind) 空间顶点**，再照常走 DQS 蒙皮（`Head` 骨）
  ⇒ **转头/点头时表情自动跟着走**，无需特殊处理。
- **输入用 blendshape 基 + 权重**（位移 `= Σ_k w_k · δ_k`），而不是纯程序化位移——否则做不出可信表情。
- **法线必须重算**（表情是各向异性变形）：CPU 侧顺手做（复用 `interface/mesh_geometry.h`）。
  近脸最容易被看出"光影糊"，这条比远端重要得多。
- **上传策略**：不要每帧重建 VBO ⇒ 用 **"动态头槽位池"**：固定 K 个 slot，谁最近谁占；
  **只有进槽/出槽时才更新该 slot 的 VBO**，其它帧只更新 blend 权重。
- **4-pass 一致**：动态脸改了 VBO ⇒ shadow / picking / highlight 也要用同一份改后几何
  （`jpov_crowd_instancing_arch.md` §5 点名过"防手动/拾取错位"）。

### 4.3 切档过渡（必须做）

从"装备静态脸"切到"动态脸"那一刻表情会突变 ⇒ **必须过渡**：切进去时先对齐同一个基础表情，
并加位置/时间阻尼。否则相机一走近"脸闪一下"。LOD 决策归用户（§6.3），过渡属于策略层责任。

### 4.4 K 是策略参数，不是架构参数（判据）

| K（动态脸数） | 判据 |
|---|---|
| **10** | CPU/上传可忽略 ⇒ 就按本节做 ✅ |
| **100** | 几万 → 几十万顶点/帧，需要开始盯预算 |
| **> 200** | 该转 GPU 侧（共享表情基 + per-instance 权重 / morph texture），那时**不再退出 instancing** |

⇒ 接口上把"近邻动态脸集合"当成**用户给的 LOD 决策**（同 §6.3），JPOV 不猜。

---

## 5. 明确不开的门（防返工）

1. **不做"长度比例"**（腿长/上下身比）—— 用整体 scale 替代（§2）。
   真要做得走骨架级分档（N 档 `SkeletonType`，每档自行烘 pose atlas）。
2. **不做 per-instance 的逐骨 λ**（显存/重烘代价，见 §2）。
3. **不做"非骨语义"的空间 mask 膨胀**（§3.7-6），要做另开设计。
4. **不做"衣服比身体松"**（§3.7-1）。
5. **不给人群主体做"每实例实时骨骼/表情"**（同 §6.2 谱系取舍）；表情只给近邻 K 张。
6. **不做"顶点级新增属性"**：通道号查表复用 `aJoint`，不新增顶点数据。

---

## 6. 下一步（实现清单，另开 PR）

1. `interface/skeleton_types.h`：加 `SkeletonShapeChannels`（骨→通道 + 通道名表 + `Validate()`）与
   `InstanceShape`（`channel_mu[8]`）；`SkinnedInstanceState` 挂上 `shape`。
2. `src/skeleton/skeleton_manager.{h,cc}`：上传 `uBindPos[]` / `uBindRotQ[]` / `uBoneChannel[]`
   （骨架级、一次）；导出通道名表给 UI。
3. `src/skeleton/skinning_shader.h`：**主 pass 与 shadow pass 同改**（§3.3-3）+ 法线逆转置
   + `uShapeEnabled` 开关（§3.3-4，保逐字节零回归）。
4. `skeleton_renderer.cc`：per-instance 系数上传（loc11/12）；光空间 AABB 按 `max μ` 放大。
5. viewer：按通道名表自动生成"部位膨胀"滑条（人形 7 个），用于交互验收。
6. 门禁：无通道表零回归 gold + `μ≡1` 容差门禁 + 截面直径量化测试 + 混合区近距离 gold（§3.6）。
7. 脸：`head_idx` / `face_expr_idx` / `face_tex_idx` 三个 selector 接入 part 模型；
   动态头槽位池 + CPU blendshape 微操 + 过渡（§4）。

### 顺手可清（体检发现，可选）
- **`uBoneCount` 是死 uniform**：shader 里只声明、body 零消费（注释也写明「不再乘 uBoneCount」），
  但 host 每次 draw 都 `glUniform1i` 一次（主 + 阴影 pass）⇒ 可删 shader 声明与那两句上传。
  注意 `GpuHandles::bone_count` **本身不能删**（CPU 侧算 `pose_w` 还在用）。
- `aInstPose` 是 `vec3`（`kInstancePoseAttrSpec{10,1,3,3}`）⇒ 第 4 个分量空着。若不想再加 location，
  可升为 `vec4` 白拿 1 个 per-instance float（每实例 +4 B，千人 +4 KB）；但要注意与
  「pose 选择」混在同一 attribute 里的语义清晰性。
