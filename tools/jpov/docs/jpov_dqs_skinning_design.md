# JPOV 蒙皮：对偶四元数（DQS）设计

> **状态**：2026-09-18 落地（分支 `feature/20260918-jpov-dqs-skinning`）。
> **前身**：LBS（矩阵线性混合）——旧公式在 `src/skeleton/skinning_shader.h` 的「保留」注释段与
> 测试的 `SkinMeshOnCpuForTestLinearBlend` 里各留一份，作为对照/历史。
> **配套**：`docs/jpov_skeleton_manager_design.md`（资源层/方案甲）、
> `docs/jpov_skinning_shader_m1_design.md`（M1 基础链路，其矩阵混合描述已被本文取代）、
> `geom/math/dual_quat.h`（数学实现的权威注释）。

---

## 1. 为什么换（问题与收益）

蒙皮要把「每根骨各自的**刚体**变换」按顶点权重**混合**成一个变换。旧写法（LBS）混合的是
**矩阵**：

```
位置  v' = Σ_i w_i · M_i · v          n' = Σ_i w_i · mat3(M_i) · n
```

`Σ w_i·M_i` 是一堆旋转矩阵的**加权平均**，一般**不再是旋转**（正交性被破坏）：关节弯折处的
顶点被“拉向弦”，表现为**体积塌缩**；扭转关节则出现**糖纸（candy wrapper）**伪影。

对偶四元数（DQS，Kavan et al. 2007）混合的是**刚体变换本身**，混合后归一化**仍是刚体** ⇒
不塌不拧。本工程的量化实测（`jpov_dqs_skinning_test`）：

| 场景 | 原始距离 | DQS | LBS |
|---|---|---|---|
| 90° 弯折处（权重各半的两点） | 0.02 | **0.02**（保距） | 0.0141421 = 0.7071×原始 = cos45° |
| 两帧插值 ratio=0.5 | 0.02 | **0.02** | 0.0158114 |

> 这条 `cos45°` 就是“肘部塌进去”的数学本质：矩阵平均把长度按夹角余弦压扁。

---

## 2. 数学定义（本工程约定）

**刚体变换**用单位四元数 `r`（旋转）+ 平移向量 `v` 表达：`T(p) = r·p·r* + v`。
**对偶四元数**（`ε² = 0`）：

```
q̂ = q + ε·t          实部 q = r（旋转）；对偶部 t = ½·v̂ ⊗ q
                     （v̂ = (0, v) 纯四元数，⊗ 为 Hamilton 乘法）
```

反解平移（`dual_quat.h` 文件头有推导）：

```
t ⊗ q* = ½·v̂ ⊗ q ⊗ q* = ½·v̂      ⇒      v = 2·vec(t ⊗ q*)
```

于是变换写作 `T(p) = r·p·r* + 2·vec(t ⊗ q*)`。**GLSL 里的等价展开式**（避免在 VS 里构矩阵）：

```glsl
// 旋转部分（同 geom::RotateVector）
rot = p + 2·q.w·(q.xyz × p) + 2·(q.xyz × (q.xyz × p));
// 平移部分（= 2·vec(t ⊗ q*)）
tra = 2·(q.w·t.xyz − t.w·q.xyz + q.xyz × t.xyz);
p'  = rot + tra;
```

**两条必须遵守的约束**（下文算法逐条对应）：

1. **归一化**：单位对偶四元数满足 `|q| = 1` 且 `q·t = 0`。加权和 `Σ w_i·q̂_i` 两者都会被破坏
   ⇒ 混合后必须把**实部与对偶部同除** `|Σ w_i·q_i|`（只除实部会让平移尺度错）。
2. **抗对偶（antipodality）**：`q̂` 与 `−q̂` 表示**同一个**刚体变换；两者直接相加会互相抵消
   （合成 `|q| → 0` 的垃圾）。故混合前必须把每个 `q̂` 与**参考**统一到同一半球。

---

## 3. 算法（逐条 = 代码里的实现）

`src/skeleton/skinning_shader.h`（主 pass `kSkinnedVs` / 阴影 pass `kSkinnedShadowVs`，
**两份逐字同公式**）与测试真值 `SkinMeshOnCpuForTest` 完全一致：

| 步 | 做什么 | 代码位置 |
|---|---|---|
| ① | 逐骨取两帧 q̂，**在四元数空间**插值 NLERP：`dot<0` 则整体翻符号（最短路径）→ `mix` → 同除 `|q|` | `LoadBoneDualQuat` / `DualQuatLerp` |
| ② | 选**参考骨** = 权重最大者（比较用严格 `>` ⇒ 并列取小槽位下标 ⇒ 确定性） | `main()` / `DualQuatBlendWithReference` 调用方 |
| ③ | 逐骨 `dot(q_i, q_ref) < 0` 则 `(q_i,t_i)` 整体取负，再 `Σ w_i·q̂_i` | `DualQuatBlendWithReference` |
| ④ | 归一化（实部/对偶部同除 `|Σ w_i q_i|`），用结果变换顶点；法线/切线**只用旋转部分** | 同上 + `DualQuatTransformPoint` / `DualQuatRotateVector` |
| ⑤ | 退化（顶点无有效权重，或统一符号后仍完全抵消）→ 取**单位元** = 顶点保持 rest | `wsum<=0` 分支 |

**为什么参考骨要“逐顶点选最大权重”**：本工程的 pose atlas 是「散装 pose 仓库」，**不表达时间
语义**（见 `interface/skeleton_types.h` 铁律），所以烘焙期做不了“按时间连续性统一符号”这一
常规做法，只能在混合处按顶点决定参考。取权重最大者最稳（它对该顶点影响最大，符号以此为基准
最不容易翻错），且与姿态/时间无关 ⇒ 可复现。

**零回归的关键性质**：bind/rest 姿态下每骨 `skinM = I` ⇒ 每骨 `q̂ = 单位元` ⇒ ③ 后
`Σ w_i·q̂_i = (0,0,0, Σw_i)`（权重和为 1）⇒ 归一化**精确**得到单位元 ⇒ 顶点逐位不动。
这是「静态 gold 图零回归」的数学依据（`dual_quat_test.cc` 的
`BlendOfIdentityBonesIsExactIdentity` 把它钉成断言）。

---

## 4. 数据表示与 atlas 布局

| | 旧（LBS，矩阵） | 现（DQS，对偶四元数） |
|---|---|---|
| 每骨 | 4×4 矩阵 = **4 texel** | 实部 q + 对偶部 t = **2 texel** |
| 每 pose 宽 | `4×bone_count`（23 骨 = 92） | `2×bone_count`（23 骨 = 46） |
| 每行 pose 数 | `floor(2048/92)` = 22 | `floor(2048/46)` = 44 |
| 容量 | ≈45,590 pose | ≈91,180 pose |
| 纹理内存 | 2048²×RGBA32F = **64 MB** | **32 MB** |
| 每顶点 texelFetch | 4 骨 × 4 = **16** | 4 骨 × 2 = **8** |

- 布局仍是**行优先平铺**：`flat → (flat % W, flat / W)`，一个 pose 的 texel **可能跨行**，
  故逐 texel 各自回绕（烘焙 `PutDualQuatTexels` ↔ 采样 `LoadDualQuatAt` 必须同款判行）。
- **烘焙期为什么用矩阵 → 对偶四元数这条转换**：骨架链路本来就是矩阵算出来的
  （`jointWorld·inverseBind`），且该链路只含旋转/平移 ⇒ 恒为**刚体** ⇒ 转换**无损**。
  `DualQuatFromRigidMatrix` 用 Shepperd 法反解旋转四元数（`Mat4ToQuaternion`）+ 第 3 列取平移；
  若矩阵含缩放/剪切（本链路不该出现）→ 直接 `LOG(FATAL)`，不静默丢缩放。

---

## 5. 边界与已知限制（诚实的部分）

1. **不支持缩放/剪切**：对偶四元数只能表示刚体。本工程的骨架链路（`T(rest)·R(bind)·R(pose)`
   沿树复合 + 仿射求逆）恒满足；将来若要做“骨缩放 / 卡通 squash”，需另开机制（DQS 的缩放扩展）。
2. **两帧插值插的是“最终蒙皮变换”**（`jointWorld·inverseBind`），不是关节局部旋转。
   在四元数空间 NLERP 已比旧的矩阵 lerp 好（不缩体积、不会绕远路），但对相邻帧旋转差接近
   180° 的极端帧仍有差异。要更物理，应在**关节旋转**上 slerp 后重算 atlas（需 atlas 另存
   旋转，或 CPU 侧插值后重烘焙）——不在本次范围。
3. **DQS 的副作用**：它把“塌陷”换成“保体积”；极端扭曲处可能出现轻度**鼓胀**（尤其低模）。
   这是表示本身的性质，不是 bug。
4. **权重和不为 1 的 asset**：归一化 `q̂` 等价于按权重和重新归一 ⇒ 比 LBS 更稳，但与
   LBS 的“按权重和缩小”行为**不同**（glTF 规范权重和为 1，工程内不构成问题）。

---

## 6. 验证（怎么证明它对）

| 层 | 目标 | 位置 |
|---|---|---|
| 数学单测 | 与矩阵链路等价、复合、端点、抗对偶（含反例）、非刚体 FATAL、**GLSL 展开式镜面对照** | `geom/math:dual_quat_test`（14 cases） |
| 物理量测（纯 CPU） | 90° 弯折保距 vs LBS 塌到 cos45°；两帧插值同样保距；真实资产边长比尾部统计 | `test/skeleton:jpov_dqs_skinning_test`（5 cases） |
| GPU ↔ CPU 逐像素 | 25 帧 + 6 组双帧插值（ratio 0.25/0.75，**不用 0.5**：0.5 是 mix 不动点，抓不到方向错） | `test/skeleton:jpov_skinned_multipose_test` |
| 静态零回归 | bind pose gold 图 | `test/skeleton:jpov_skeleton_gold_test` |

实测（2026-09-18，llvmpipe）：

- 除零回归外，**gold `max-channel-mean-diff = 0.00056`**（阈值 25）—— bind 姿态下与改动前
  基本一致（残差来自矩阵链的浮点误差）。
- GPU vs CPU 真值：25 帧 **mask 不一致率 0**，平均通道差 **~1e-5**；双帧插值 6 组同结论。
  （旧实现同口径是 0.02%~0.5%，因为两侧公式分别是矩阵 lerp 与矩阵 lerp 的实现差异。）
- multipose 的「门禁零」：DQS vs LBS 顶点偏差（纯 CPU，24 帧）平均 9.7e-4 / 最大 0.122 ——
  一旦有人把 DQS 换回矩阵混合，这条会立刻红。
- **负向验证（注入 bug，确认门禁真能红）**：
  - 主 pass 的平移项符号写错 → `mask 0.0497 > 0.02` → FAIL ✓
  - **只**改阴影 pass（丢掉平移项）→ 36 条门禁失败，`mask` 最大 0.57 → FAIL ✓
    ⇒ **阴影 pass 也在自动门禁覆盖之内**（蒙皮人投在地面的影子参与逐像素比对）。
- **真实资产尺度（mixamo_male.glb，353,880 条边 × 24 个极端 pose 汇总）**：
  - 边长比**中位数两边都是 1** —— 绝大多数边完整落在单根骨内，两种写法给出同一个刚体变换。
  - 尾部：明显缩短（< 0.95）的边占比 **DQS 1.86% vs LBS 2.52%**；最短 1% 分位 DQS 0.858 / LBS 0.791。
  - 同一批 pose 下顶点最大偏差 **0.122 m**（关节混合区）。
  - ⇒ 收益集中在**关节混合区**（约 2% 的边），不在全身；低模/大幅弯折的资产上收益更明显。
  - ⚠️ 同一原因，fbx viewer 里逐帧肉眼对比 hip_hop_dance（中等摆幅）时，DQS 与 LBS 的差异
    是**局部、几个像素级**的轮廓位移（实测 0.01%~0.03% 像素不同，最大通道差 ~150-190）——
    要看明显差异请用大幅弯折的动作或放大关节处看。
- ⚠️ **主 pass 与阴影 pass 必须逐字同公式**（PR #104 教训：一旦分叉，影子与身体错位）。
  GLSL 无 `#include`，两份 program 各持一份完整源码 —— 改一处必须同步另一处。

---

## 7. 代码地图

| 文件 | 职责 |
|---|---|
| `geom/math/dual_quat.h` | 对偶四元数：构造 / 刚体矩阵互转 / 变换点·方向 / 复合 / 归一化 / NLERP / 参考骨加权混合（DLB） |
| `geom/math/mat4.h` `Mat4ToQuaternion` | 旋转矩阵 → 四元数（Shepperd，w≥0 定符号） |
| `src/skeleton/skeleton_manager.cc` | atlas 烘焙：`jointWorld × inverseBind`（刚体）→ 对偶四元数 → 每骨 2 texel |
| `src/skeleton/skinning_shader.h` | 主/阴影蒙皮 VS（DLB + NLERP；LBS 旧公式按注释保留） |
| `src/skeleton/skeleton_renderer.cc` | per-instance 传「pose 平坦 texel 起点 = pose_idx × bone_count × 2」 |
| `test/skeleton/jpov_skeleton_gold_common.h` | CPU 真值（DQS）+ LBS 对照实现，两者共用矩阵链路 |

---

## 8. 成本（如实说明）

- **烘焙期**：与旧实现同量级（每骨多一次矩阵→对偶四元数的转换，一次性）。
- **顶点期**：texelFetch 从 16 降到 8、atlas 内存 64→32 MB；ALU 略增（四元数乘法 + 归一化 +
  符号比较）。净效果预期持平或略优（取数在 GPU 上更贵），**但本次未做正式 benchmark** ——
  人群量级的数字要用真实 crowd 场景实测，不要拿本节当结论。
