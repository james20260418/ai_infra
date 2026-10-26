# JPOV 植被风（vegetation wind）设计文档

> 2026-10-15 起稿，2026-10-18 对齐到最终实现；**2026-10-26 顶点风单关节化**（§10）。
> 目标：**无状态程序化顶点风** —— 位移全在顶点着色器里算（不做 CPU 模拟、不做物理解算），
> 因此确定性、可 gold、可逐物体配置、零帧间状态。
> 前置阅读：`docs/jpov_vegetation_design.md §方向3`、`docs/jpov_instance_attr_lag_design.md §2.1`（16 槽硬约束）。

---

## 0. 一句话

**Pivot Painter 式逐枝枢轴旋转**（刚体链、保长、接头焊死）+ **朝向继承度 hard↔soft** +
**链外平动湍流** + **阵风包络**；关节表由 Blender 侧烘焙，随资产走。

行业对照：UE Pivot Painter 2.0、SpeedTree per-bone wind（flexibility / gravity）、GPU Gems 3 ch.16。

## 1. 范围（重要）

| 路径 | 本版 |
|---|---|
| **object3d**（`DrawObject3D` / `DrawGltfObject`） | ✅ 接入 |
| instanced 静态实例 | ✅ 接入（2026-10-19） |
| 蒙皮 | ❌ 不动 |

- 风参数挂在**命令上**：`Object3DCommand::wind` / `InstancedObjectCommand::wind`
  （都是 `std::optional<WindCommand>`），**逐物体/逐批**：同场景两棵树可以有各自的风向 /
  相位 / 强度。**不存在「全场景一份风」的全局命令**（这正是它没有放进 `RenderCommandList`
  的原因）。实例路径另加逐实例 `InstanceState::blow_away_ratio`。
- 生效闸 = **两个条件同时满足**：命令里设了 `wind` 且 mesh 带 `MeshVertexFlags::kWind`
  （渲染时体现为 `uWindJointCount > 0`）。无风数据的 mesh（地面、石头、人）加不加 `wind`
  都完全不受影响 ⇒ **未使用 = 零开销、零回归**。
- 算法只有一种：**没有 `mode` 开关，没有平移式退化路径**。

## 2. 数据模型

### 2.1 逐顶点（2026-10-26 定稿：单关节）

| attribute | 内容 | 语义 |
|---|---|---|
| `loc6 = vec2` | `(joint_id, phase)` | joint_id = 该顶点所属刚性段在关节表里的下标（唯一、无歧义）；phase = 连续相位场 `[0,1)` |

- **为什么只需要一个关节**：joint_id 由烘焙端按**拓扑**直接定（同一枝的同一采样环共享一个
  关节），分叉处的连续过渡靠「环共享关节」保证，不靠空间插值。v1 那套「空间最近的两个关节 +
  1/距离权重 + 5mm 焊接」是给**只能猜归属**的资产打的补丁，v2 起没有存在理由（§10）。
- 路径长 `L`（吹倒权重）**不进顶点属性**：VS 沿关节链现算（§10.2），少 4 字节/顶点、
  也少一次载入期平滑（那平滑同样是为了抹掉 v1 的标签跳变）。
- 来源：glTF `TEXCOORD_1`（vec2 = joint_id, phase）→ GPU 缓冲原样上传，**载入期不做推断**。

### 2.2 每 mesh 一份关节表（node extras `jpov_wind_joints`）

每关节 **7 float**：

```
pivot.xyz  parent  level  phase  soft
```

- `parent = -1` 表示根；
- `level` = 距根的关节数（θ 的增益与频率按此分档）；
- `phase` = **逐关节**相位（秒）；同一刚性段内所有顶点共享 ⇒ 段内不剪切；
- `soft` = 朝向继承度 `0..1`（0 = 硬，全继承父朝向；1 = 软，只随自己的 θ）。

同树的多个 primitive（trunk / leaves）**共享同一张表**。

### 2.3 GPU 侧

每 mesh 一张 **RGBA32F，低 2N texel × 1**：

```
texel(2j)   = (pivot.xyz, parent)
texel(2j+1) = (level, phase, soft, 0)
```

占 `kTexUnitWindJointTable = 12`；**必须留在 0..15**（GL 3.3 的 texture unit 上限）。
该槽与 `kTexUnitPoseAtlas` 同号 —— 蒙皮与植被风不同时活跃，故意复用。

> **契约**：凡「逐关节」的量必须**只依赖关节自身**（`level / phase / soft` 全编进表），
> 否则同一关节对不同顶点给出不同 θ ⇒ 段内不再刚性 ⇒ 拉伸 / 撕裂。

## 3. 算法（VS，根→梢递推）

```
q_j = q_parent + R(A_parent) · (pivot_j − pivot_parent)   // 位置：完整继承
A_j = θ_j + (1 − soft_j) · A_parent                       // 朝向：按 (1−soft) 打折
p'  = q_own + R(A_own) · (p − pivot_own)                  // 顶点
θ_j = amplitude · 0.4 · (0.15·level) · sin(2π · f0(1+1.6·level) · (t + phase_j))
```

- **每段仍是刚体 ⇒ 保长**；`q_child` 与“父链对 `pivot_child` 的映射”逐点一致 ⇒ **接头焊死**。
- 根关节 `level = 0 ⇒ 增益 = 0 ⇒ 固定端不动`（树的基部是支承，不是自由端）。
- 旋转轴：`axis = normalize(cross(worldUp, windDirection))` —— 绕水平轴、在风向的竖直平面内弯。
  法线一并旋转（否则光照不跟着弯，会显“贴纸感”）。
- 链深上限 `kMaxWindChain = 16`，超出部分截断（每个环 2 次 `texelFetch`）。

### 3.1 链外平动湍流（逐顶点）

```
p  += tdir · (tamp · sin(2π · tfq · (t + phase_vtx)))
tamp = uWindTurbAmp · soft · (0.5 + 0.5·level) · gust
tfq  = uWindTurbFreq · (1 + 0.5·level)
gust = mix(1, pow(max(0, 0.5+0.5·sin(2π·fgust·(t+phase_vtx))), 4), gust_strength)
tdir = normalize(0.6·axis + 0.7·up + 0.4·wdir)     // 斜上，不在风向平面内
```

- **为什么逐顶点而不是逐关节常量**：纯平移**不能改变朝向**；而“逐关节一个平移常量”会在每个
  接头拉出 `|δ|` 的台阶。用连续 `phase` 场驱动 ⇒ 位移场连续 ⇒ 不撕裂。
- **相位必须含 `uWindTime`**（踩过：漏掉它 ⇒ 湍流变成静态位移，且“频率”退化成**空间**频率
  ⇒ 高频时位移场梯度爆炸 ⇒ 关节被扯开）。

### 3.2 确定性（gold 友好）

`time` 必须由**帧号**推（`frame / fps`），不许用 wall-clock。所有相位先归约到 `[0,1)` 圈再乘
`2π` —— 大 `uWindTime` 下直接 `sin(2π·大数)` 会因 float 精度崩坏。

## 4. 资产管线

**现役（v2）**：`tools/jpov/tools/vegetation_tool_v2/bake/`。关节由 shape 的枝折线按弧长抽稀
（`vegetation_glb_joints.cc`），逐顶点归属按**采样环**精确给出（树干按环下标 floor、
叶卡挂到 anchor 所在环的关节）。产物三件套写进 GLB：

1. `TEXCOORD_1` = `(joint_id, phase)`
2. node extras `jpov_wind_joints` = 7 float/关节
3. 关节表坐标系已转成导出坐标系

**已归档（v1）**：`tools/jpov/legacy/vegetation/wind_bake.py` + `build_tree.py --bake-wind`。
它的顶点标签靠**表面测地 Voronoi** 推断（种子只取关节自己所属样条上的顶点、按连通块多数票约束；
标签沿网格边传播 ⇒ 不会跨空跳到旁边那枝）——**这是渲染侧两关节混合的根因**：
推断出来的归属天然有跨链歧义，只能在 VS 里靠距离加权抹平。

## 5. 参数（`WindCommand`）

| 字段 | 默认 | 作用 |
|---|---|---|
| `time` | 0 | 秒；由帧号推，保证确定性 |
| `direction` | (1,0,0) | **单位向量**。只定方向；没有 speed 字段，强度由 amplitude 表达 |
| `amplitude` | 1.0 | 结构角总闸（`θ ∝ amplitude`） |
| `frequency` | 1.0 | 基频 Hz；各 level 按 `1 + 1.6·level` 分档 |
| `turb_amplitude` | 0 | 链外平动湍流幅度（模型单位；0 = 关） |
| `turb_frequency` | 2.5 | 平动湍流频率 Hz（纯时间频率） |
| `gust_frequency` | 0.6 | 阵风频率 Hz |
| `gust_strength` | 0.7 | 阵风深度；0 = 关起伏 |
| `blow_away_exponent` | 2.0 | 吹倒曲线指数 n（`d = ratio·L·(L/H)^(n−1)`；仅实例路径） |

shader 常数（`object3d_renderer.h`）：`kWindPivotGain = 0.4`、`kMaxWindChain = 16`、
`kTurbLevF = 0.5`、`kTurbGustP = 4.0`。

## 6. 实现要点 / 踩过的坑

1. **父关节哨兵解码**：`int(floor(w + 0.5))`。GLSL `int()` 向零截断会把 `-0.5` 解成 `0`
   ⇒ 环回 joint 0 ⇒ 整株被扳倒。
2. **根关节不参与旋转**（`level = 0` 增益为 0），否则整棵树绕根部公转。
3. **level 夹 `[0,3]`**：异常资产的巨大 level 会把整株扳倒。
4. **三条 object3d 路径共用 `UploadObjectWindUniforms()`**（主 pass / 阴影 / 拾取·高亮）——
   否则开风时拾取与高亮按静止姿态算，点击错位。
5. **gate 在运行时判 `MeshHasFlag(kWind)`**（不是编译期宏）：无风 mesh 不上传关节表、不挂 loc6。
6. loc6 是**布料 LAG 让出来的副运动槽**，同一时刻只能有一个使用者
   （`MeshVertexFlags::kWind` 与 `kRelax` 互斥）。

## 7. 验证与边界

- 本地：`bazel build //...` 绿；`bazel test //tools/jpov/...` **130/130（582 cases）** 绿
  （2026-10-26 单关节化后实测）。
- **gold 门禁**（2026-10-26 补）：`test/vegetation_wind/` —— instance pass 两张 + object3d 一张
  逐像素比对（阈值内），且额外要求「开风 vs 关风」差异 ≥ 1%（否则一张完全不动的图也能"通过"）。
- 历史行为门禁（v1 查看器 `--capture-wind` A/B，工具已归档到 `legacy/`）：开/关差
  **3.3%–3.8%** 像素；相邻帧差 **3.8%–4.1%**（证明在动，不是静态位移）。现役口径以下面的 gold 为准。
- 已知边界：蒙皮路径还不吹风（布料另议）；链深 >16 的资产会被截断；
  性能未在真 GPU 实测（本机 llvmpipe）。

## 7.1 已移除：object3d 路径的「两关节混合」（2026-10-22 → 2026-10-26）

2026-10-22 曾把 object3d 的三份 VS 改成 `p = mix(apply(j0), apply(j1), w1)`，属性侧
`aFlex` 从 vec2 变 vec4 `(j0, phase, L, j1)` 并用 loc3 装权重 —— 为了跟 instanced 路径
统一（那边先这么做了）。

**2026-10-26 整套移除（§10）**：混合是给「关节归属靠 inference 猜」的 v1 资产打补丁；
v2 烘焙按拓扑直接定归属后，一个顶点只属于一个刚性段，混合既不必要，又会因为邻关节的
`θ/soft` 不同而在同一环内引入非刚性形变（最典型的症状是细枝被"抹圆"）。

## 8. 实例化路径（2026-10-19）

植被改走 `InstancedObjectRenderer`：同 mesh 摆 N 份 = 每个 primitive 一次 instanced draw。
风的公式与 object3d 路径**同源**（同一套枢轴链 + 湍流），区别只有两处：

1. **摆放矩阵来自 per-instance attribute**（loc7..10），不是 `uModel` ⇒ 风向要先从
   世界系搬进模型系：`normalize(mat3(inverse(instance_model)) * world_horizontal_dir)`。
   否则每株 yaw 不同时，一片林子会朝各自的局部方向倒（object3d 路径单株看不出这个问题）。
2. **新增逐实例「吹倒比例」** `InstanceState::blow_away_ratio`（loc11，float）+
   **逐命令曲线指数** `WindCommand::blow_away_exponent`（默认 2）。
   吹倒是**平移**（不是角度）—— 角度会被 `soft` 的继承模型干扰，软枝（soft≈1）各段只随自己的角度、
   整枝不跟风，观感诡异；平移与 soft 无关：
   ```
   旋转（风）：顶点沿自己那个关节的刚性链变换（单关节，§10）
   吹倒（平移）：p += dir_h × ratio × L × (L/H)^(n−1)     // L = VS 由关节链现算
   H = 本 mesh 模型系包围盒高（uniform uBlowAwayRefLength）
   ```
   - **为什么要 H**：n≠1 时 `L^n` 的量纲随 n 变（m^n）。直接 `ratio×L²` 时
     ratio=0.5 会把树甩出画面十几米，且换一种尺寸的资产观感完全不同。
     归一化后**梢部（L=H）位移恒为 ratio×H**（与线性一致），只是中段压得更低
     ⇒ 观感正是"下段直、上段加速压弯"（n=1 线性 / n=2 二次 / n=3 更陡）。
   - 0 = 不吹（默认）；查看器滑条上限 0.5。
   - L 由关节链现算（只依赖静止几何）⇒ 沿枝单调、不会在关节处出现台阶。

三条 pass（主 / 阴影 / 拾取·高亮）共用 `UploadInstancedWindUniforms()` + 同一份
`kWindBlowGlsl`（std::string 拼接，三个实例 VS 共享，不是三份手抄）⇒ 影子和拾取
与本体同形变。无 `WindCommand` 或无关节表时 `uWindEnabled=0`，整段跳过、不挂 loc11。

验证：`--capture-blow`（钉住风时间，只扫 0 / 0.1 / 0.25 / 0.5 四档）；四档差异像素
59k → 66k → 78k，倾斜方向与风向一致；`instanced_object_attrs_test` 与
`jpov_instanced_object_gold_test` 均绿（后者逐像素零回归）。

## 9. 导出当前风形态的网格（**已归档**，2026-10-26）

> 工具随 v1 查看器一起移到了 `tools/jpov/legacy/vegetation_viewer/`（不编译）。
> 保留本节只作记录：它是当时查"跨链三角形 / 面积放大"用的手段。

```bash
jpov_vegetation_viewer --glb plants/willow_wind_tex.glb \
  --save-mesh /tmp/vexp/tree --blow 0.5 --blow-exponent 2 --wind-time 0.35
# 产出 tree.obj / tree_verts.csv（prim,vertex,joint,L,x,y,z）
#      / tree_tris.csv（prim,tri,ja,jb,jc,area_rest,area_deformed,ratio）
```

CPU 端复刻当时那份 GPU 公式（`legacy/vegetation_viewer/wind_deform.h`，含两关节混合），
**不依赖 GL**。

用它查到的结论（willow，blow=0.5 / n=2）：

- 树皮 7884 tris 中约 **10.8% 跨样条面**（早先的 15.56% 口径把同一样条的兄弟关节误算进去了）；
- 面积放大 >5 倍且变形后 >1cm² 的“板子”数量：
  逐顶点平移老版 **216** → 2 关节混合 + 逐顶点平移 **25**；
- ratio 0.1/0.25 目视为自然弯曲、无悬浮碎片；0.5 时梢部位移 = `ratio×H ≈ 3.6m`，属极端值。

（该 CPU 导出器已随 v1 归档，不再与 GPU 同步；上面这段只作历史记录。）

## 10. 单关节化（2026-10-26）—— 移除 v1 时代的三处补丁

**背景**：v2 工具（`vegetation_tool_v2`）成为资产唯一来源后，每个顶点的关节归属在
**烘焙期就按拓扑定死**（同一枝的同一采样环共享一个关节）。v1 资产靠表面测地 Voronoi
推断归属，才会出现「相邻顶点分属两条链」的撕裂，渲染侧为此叠了三处补丁：
① 载入期找**空间最近的两个关节**并按 `1/d` 加权（loc3 = 权重、loc6 = `(j0,phase,L,j1)`）；
② 近重合顶点（5mm）**焊接**成同一套权重；
③ 逐顶点路径长 `L` **Laplacian 平滑 20 轮**。
三处一起删。

**现在的数据通路**（一条直线，没有推断）：

```
烘焙: 顶点所属采样环 → 关节              → TEXCOORD_1 = (joint_id, phase)
载入: MeshData::wind_link (Vec2f)        → 直接上传 loc6 (vec2)，不查、不算、不焊
VS  : joint_id → 关节表 → 父链刚性变换   ；path_len 顺路累加（吹倒权重）
```

- **路径长 L 在 VS 里现算**：`L = |p − pivot_own| + Σ|pivot_k − pivot_parent(k)|`，
  在遍历父链的同一个循环里累加（零额外 texelFetch）。只依赖静止几何 ⇒ 逐帧稳定、
  沿枝单调 ⇒ 不在关节处出现台阶。
- **属性预算**：loc6 从 vec4 收回 vec2（-4B/顶点），loc3 彻底释放给蒙皮/其他用途，
  `GPUMesh::vbo_wind_w` 删除。
- **gold 门禁**：`tools/jpov/test/vegetation_wind/`（instance pass 开风/吹倒 + object3d 开风，
  共三张 1280×720 基准 + 一条「开风 vs 关风必须显著不同」的对照）。
- **改造前后实测**（gold 用改造前的代码出，改造后拿同一机位比对，threshold=8/255）：
  instance 只吹风 **2.19%** 像素变、带吹倒 **2.41%**、object3d **0.70%**；
  差异全部落在轮廓与极细枝条上（看图确认无撕裂、无缺失枝条）。差异来源是
  ① 去掉两关节混合后细枝不再被"抹圆"，② L 从平滑值换成链上精确值。
  gold 已按新语义重出，验收阈值 0.5%（留给"实现变了但语义没变"的抖动）。

**如果将来还要支持 v1 那种"归属靠猜"的资产**：不要恢复渲染侧插值 —— 让烘焙端把归属
算准（v2 的 `JointCoveringSample` 就是范例），渲染侧保持单一来源。
