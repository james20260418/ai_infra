# JPOV 植被风（vegetation wind）设计文档

> 2026-10-15。目标：**无状态程序化顶点风**（VS 里算，确定性、可 instancing、可单测），
> 一版到位（主弯 + 细节颤 + 多频 + 相位），并给一个「小柳树」模特的风演示查看器。
> 前置阅读：`docs/jpov_vegetation_design.md §方向3`（风=VS 程序化）、
> `docs/jpov_instance_attr_lag_design.md §2.1`（16 槽硬约束）。

---

## 0. 一句话设计

**per-vertex `vec2(bend, phase)` + per-instance `vec4(wind.xyz, phase)` + per-draw 标量**，
三者驱动一个**多频正弦叠加**的顶点位移；`bend` 走非线性曲线，`phase` 全用**秒**。

```
disp(v,t) = Σ_k  wind.xyz · A_k · f(bend) · sin(2π f_k (t + phase_inst + phase_vert))
          (+ 细节颤：沿法线的高频小位移，权重 ∝ f(bend)）
```

---

## 1. 数据模型（三层）

| 层 | 数据 | 类型 | 语义 |
|---|---|---|---|
| **per-vertex** | `(bend, phase)` | `vec2` | `bend∈[0,1]`=沿枝归一化位置（0 基、1 梢）；`phase`=时间偏移（**秒**） |
| **per-instance** | `(wind.xyz, phase)` | `vec4` | `wind.xyz`=风向×风速（长度即风速）；`phase`=实例时间偏移（**秒**） |
| **per-draw** | `{time, amplitude, frequency, turbulence, direction, speed}` | 标量 + 风矢量 | 环境态：时间/振幅/频率/湍流；`direction·speed` 供**非实例**路径（object3d/蒙皮）用 |

### 1.1 为什么 phase 用「秒」

Danis 定：per-instance phase 单位 = 秒。为**量纲一致**（通用规则 §9：时间戳/间隔要有单位且不混用），
per-vertex phase 也统一用**秒**（一个时间偏移）。合成相位：

```
phi = 2π · f · (t + phase_inst + phase_vert)          // t, phase_* 皆秒；f 单位 Hz
```

> 存「秒偏移」而非「周期比例」的取舍：f 是运行时参数，秒偏移在改 f 时含义稳定（都是时间量），
> 比例则要绑 f。选秒，与 per-instance 一致。

### 1.2 `bend` 存「输入」不存「结果」

存沿枝**归一化位置 x∈[0,1]**，非线性映射 `f(x)` 留在 shader（改曲线不重烘资产）：

```
f(x) = x*x            // 主弯（基部硬、梢部软）
```

（可选 `x^4` 更强；见 §3。资产烘的是 `x`，不是位移。）

---

## 2. 属性槽位（16 槽硬约束下的落点）

顶点属性与实例属性**共享同一套 location 空间**（`GL_MAX_VERTEX_ATTRIBS` 保证 16）。

| loc | 归属 | 名称 | 现用（在途/已合） | 本次 |
|---|---|---|---|---|
| 0 | 顶点 | aPos | 通用 | — |
| 1 | 顶点 | aNormal | 通用 | — |
| 2 | 顶点 | aTexCoord | 通用 | — |
| 3 | 顶点 | aJoint | 蒙皮 | — |
| 4 | 顶点 | aWeight | 蒙皮 | — |
| 5 | 顶点 | aTangent | 法线贴图 | — |
| 6 | 顶点 | **aFlex** | 布料 `kRelax (lag_ratio,max_lag)` | **复用槽**：布料=`kRelax`，植被=`kWind (bend,phase)`，**互斥** |
| 7–10 | 实例 | aInstModel ×4 | 实例摆放 | — |
| 11 | 实例 | **aInstWind** | 蒙皮实例 `aInstPose`（在途） | **复用槽**：蒙皮实例=pose，静态/植被实例=`vec4(wind,phase)`，**互斥** |
| 12–15 | 实例 | 蒙皮实例专用 | 粗细/partial/lag | —（静态/植被实例用不到） |

**核心手法 = 特性分区**：一个 mesh 不会同时是「植被 + 布料 + 蒙皮人」。
`loc6` 与 `loc11` 对**非布料 / 非蒙皮实例**（= 植物）天然空闲，**零新增槽**。

约定（写进 `mesh.h` / 渲染器校验）：
- `kRelax` 与 `kWind` **不可同时**声明（同一 loc6）→ 声明即 `LOG(FATAL)`。
- 静态实例命令**不得**同时带蒙皮实例语义（loc11 复用）。

---

## 3. 风算法（一步到位）

### 3.1 主弯（整株沿风倒）

- 权重 `f(bend)=bend²`（基部 ≈ 0 不动，梢部大动）；`bend` 由资产烘或加载时派生（见 §5）。
- **多频叠加**（避免单频的机械感；取经典 Crysis 频率比的近亲）：

```
s(t) = 0.50·sin(phi)
     + 0.30·sin(phi·1.93 + 1.7)
     + 0.20·sin(phi·3.70 + 4.1)        // Σ 振幅 = 1.0
main = wind.xyz · (amplitude · f(bend) · s(t))
```
> 频率常数是「看起来对」的调参值，不是物理频率（与 Crysis 一致）。

### 3.2 细节颤（叶/枝尖的高频抖动）

沿**顶点法线**方向的小幅高频位移，权重同样走 `bend`，幅度 ∝ `turbulence`：

```
d(t) = sin(phi·4.30 + phase·1.0)
detail = uNormalWorld · (amplitude · turbulence · f(bend) · 0.25 · d(t))
```

### 3.3 合成

```
worldOffset = main + detail
p' = aPos + worldOffset            // 在 MVP 之前位移
```

### 3.4 不做（本版明确边界）

- **保持枝长 / 球面限制**：需 pivot / 枝层级（GLB 无）。本版是**平移式**弯曲；要真弯曲另开（Pivot Painter 级）。
- **物理**：明确不做（设计文档既定：风 = 无状态程序化，非物理解算）。

---

## 4. 确定性（gold 友好）

- `time` 由**帧号**推（`frame_count / fps`），**绝不用 wall-clock** → 同一帧号逐像素可复现。
- per-vertex `phase` 用**顶点位置确定性哈希**（同 `coverage_mipmap` 的哈希思路），不用随机数发生器。
- ⇒ 同输入 = 同输出，金标测试可建。

---

## 5. 资产管线（bend/phase 从哪来）

两种来源，接口同一（都是 `MeshData.wind_flex`）：

1. **程序化生成器**（未来植被分支）：生成时直接烘 `bend`（沿枝距离/高度）+ `phase`（位置哈希）。
2. **外部 GLB**（本版演示：小柳树 `willow_small_tex.glb`）：**加载时派生**
   - `bend = clamp(|vertex − 树基| / 树尺度, 0, 1)`（离基部越远越软；基部≈0，梢≈1）；
   - `phase = hash(vertex.xyz)` 映射到 `[0,1)` 秒。
   - 派生做成 **GL-free 纯函数**（可单测）。

> 说明：`bend` 用「到树基距离」是**无层级情况下的合理代理**——整株基座硬、外围软，
> 对小柳树（下垂枝、梢远）尤其合适。

---

## 6. 演示查看器（小柳树 · 风模式）

新增 `demo/jpov_wind_viewer.cc` + `demo/wind_viewer_app.h`（基于 `main`，**不依赖未合并的植被分支**）：

- **模特**：`assets/vegetation/... willow_small_tex.glb`（main 上已有；`tools/vegetation/samples/`）。
- **风模式**：面板可调 `风向(方位角) / 风速 / 振幅 / 频率 / 湍流 / 时间缩放`，
  以及**逐实例扰动**（让 N 株风矢量/相位错开，展示「风场」）；播放/暂停。
- 绘制：**instanced**（N 株一排，默认小量），走 `aInstWind` —— 同时验证 per-instance 风场。
- **headless 拍摄** `--capture-wind <dir>`：固定机位，出若干帧（t=0/1/2/3s）看摆动，及 3~4 张 A/B。
- 交互：复用 `jpov_viewer::ViewConfig`（右键转/滚轮缩放）。

---

## 7. 测试

- **GL-free 纯函数单测**：
  - `DeriveWindFlex`（到基距离→bend 单调、范围；phase 哈希确定性 + 均匀性）；
  - 风相位合成、多频求值（给定 t/phase 的解析值），确认确定性。
- **回归**：`object3d` / `instanced` / `vegetation` 既有 gold **零回归**（未设 wind 时
  `uWindEnabled=0`，位移恒 0；`main` 已有 gold 不动）。
- 新增一个小 **gold**：wind 开启 vs 关闭、固定帧（确定性）→ 像素差异非零且稳定。

---

## 8. 落地清单（本 PR 范围）

引擎：
- `interface/mesh.h`：`kWind` flag + `wind_flex`(vec2) + Validate + 与 `kRelax` 互斥。
- `src/mesh_manager.cc`：loc6 按 flag 分派上传（relax / wind）。
- `interface/render_command.h`：`WindCommand{time,amplitude,frequency,turbulence,direction,speed}` +
  `RenderCommandList::wind`(optional)；`InstanceState` 加 `wind`(vec4)。
- `src/instance_buffer.*`：新增 loc11 `aInstWind`（vec4）。
- 着色器：`object3d`/`instanced`/两条 `shadow` 的 VS 加风位移（共用一段 GLSL 片段）。
- `src/renderer.cc` / `object3d_renderer` / `instanced_object_renderer`：上传风 uniform。

演示/测试：
- `demo/jpov_wind_viewer.cc` + `wind_viewer_app.h` + BUILD + `build_jpov_wind_viewer.sh`。
- 纯函数单测 + 一个 wind gold。

---

## 9. 边界与后续

- v1 是**平移式弯曲**（无 pivot/层级）；真弯曲（保长、枝继承）→ 见 §10（已落 level 版）。
- 阴影 pass 同步施加同一位移（否则树动影不动）——本 PR 一并做。
- 四季节/落叶：风权重随季节降低（后续，非本 PR）。

---

## 10. Level 版层级风 + 相位连续性修正（2026-10-16）

> 背景：v1 的「平移式弯曲」不真弯曲。Danis 要一步到位 → 升级为**层级风（level 版）**：
> Blender 烘焙枝层级 → 每顶点 `(softness, phase)` 进 `TEXCOORD_1` → VS 按 `softness` 分档、
> 各级独立频率/振幅、子级继承父级。

### 10.1 数据模型修正（关键）

原 §1 定「`phase` = 按枝常量」。**实践发现这是撕裂的根因**，改为：

| 量 | 语义 | 为何 |
|---|---|---|
| `softness` | 沿枝层级的归一化**弧距**（根 0 → 梢 1，**连续场**） | 跨关节连续 ⇒ 不撕；子级继承父级（关节处软度相等） |
| `phase` | **位置的连续低频场** `∈[0,1)` 秒（**不是** per-vertex hash，**也不是** per-branch 常量） | 见下 |

**为什么 `phase` 必须连续**：
- 用 per-vertex **hash**：相邻顶点相位毫不相关 ⇒ 面被直接撕开（本 PR 修掉的灾难级撕裂）。
- 用 **per-branch 常量**：在**子枝接父枝的缝**上，父枝末梢与子枝根部相位不同 ⇒ 缝处撕裂（
  叶片挂在细枝上时尤甚：卡整片会被撕成两半）。
- 用**连续低频场**：`phase(x,y,z) = 0.5 + 0.5·Σ wᵢ·sin(kᵢ·p)（clamp）`。空间去同步（树冠不同区域错拍）
  但**处处连续** ⇒ 不撕；且叶卡（~0.25）远小于波长（~3-4 单位）⇒ 卡内几近常量 ⇒ 卡保持刚体。

### 10.2 叶片：按卡刚体 + 挂到最近实体顶点

- 叶卡是**独立 mesh**。若逐顶点采骨架，一张卡的 4 角可能落到**不同枝**上 ⇒ 软度/相位不一 ⇒ 卡被剪切/脱离。
- 做法：叶子**整卡**取一个 `(softness, phase)`，且该值**从最近的实体（树干/枝）顶点拷贝** ——
  这样卡既不会被软度梯度剪切，也不会与所挂细枝分离。（bake 分两趟：先实体逐顶点，再叶子逐卡就近继承。）

### 10.3 Blender 烘焙（`tools/vegetation/wind_bake.py`）

1. `sample_source(curve_obj)`：在 curve→mesh 转换**之前**采样 Sapling splines 成折线，
   并**乘 curve 的 `matrix_world` 取世界坐标**（与成品网格顶点同坐标系；否则 base/scale 会错）。
2. `_build_hierarchy`：按「子样条起点落在父样条上」重建 parent/level；`parent/attach_t` → `hd_start`（沿层级弧距）。
3. `_nearest`：顶点就近吸附到**最近骨架采样点**（环截面上所有顶点共享同一采样 ⇒ 管环刚体、不被掐）。
4. 写两层 UV：`"Wind"` = `(softness, phase)`（→ glTF `TEXCOORD_1`）、`"WindJoint"` = `(joint_id, link_w)`（→ glTF `TEXCOORD_2`），并把关节表写进 trunk 节点 `extras.jpov_wind_joints`（每关节 7 float）；全部由 `build_tree.py --bake-wind` 触发。
   > 最终资产格式与枢轴式烘焙细节见 **§11.1 / §11.2**；面向使用者的完整操作手册（命令 / 输出 / 验收 / 坑）见 `tools/vegetation/README.md §6`。

### 10.4 着色器（level 累计权重）

```glsl
const int kLv = 4;
float sN = clamp(aFlex.x,0,1)*float(kLv);
float base = floor(min(sN,float(kLv))); float frac = sN - base;
float ssum = 0.0;
for (int l=0;l<kLv;++l){
    float w = (l<base)?1.0:((l==base)?frac:0.0);          // 累计：低层全量、当前层按 frac
    float amp = uWindAmplitude*(0.40-0.10*float(l));      // 各级增益归一化 Σg=1（梢部总位移 = uWindAmplitude）
    float fr  = uWindFrequency*(1.0+1.6*float(l));        // 越高频越靠近梢部
    ssum += w*amp*sin(6.28318531*fr*(uWindTime+aFlex.y));
}
p += uWindVector*ssum;                                     // 主弯（平移近似层级旋转）
```

- `uWindVector` 长度 = 风速；`amplitude` 滑块此时≈**梢部总位移**（量纲清晰）。
- base=0 时 `ssum=0` ⇒ **根不动**（实测 softness≈0 顶点位移恒 0）。
- 抖动项（细节颤）仍按 `softness` 线性取权。

### 10.5 验证（本地）

- 几何量（重写 VS 于 Python 逐三角测边长变化）：
  - 叶片：**max 边长拉伸 1.00×**（整卡刚体、零撕裂）；
  - 树干：softness≈0 顶点位移 **0.0000**（根固定）；
  - 叶卡与最近实体顶点位移差 ≈ 0（不再脱离）。
- 渲染 A/B（`--capture-wind`，固定机位/逐秒）：关↔开约 8% 像素变化、t0↔t3 约 10%（确在动）。
- 单测 `//tools/jpov/test/wind:all` 8/8；回归 `object3d`/`instance` 除**既有 flake**
  （`jpov_text3d_depth_alpha_gold_test`）外全绿。

### 10.6 残留

- ~~仍为**平移近似**~~ → **已由 §11 的枢轴式（mode=1）落实**（真旋转、保长、接头焊死）；本节描述的是 `mode=0` 平移式基线。
- 树干网格上极少数微边（~1.5mm）在接枝处拉伸较大（绝对量 <0.1 单位），肉眼不可见。


---

## 11. 枢轴式（Pivot Painter 式）+ 平动湍流（2026-10-16，`WindCommand.mode = 1`）

> 本章是**当前推荐**的实现。§3/§10 的「平移式」保留为 `mode = 0`（A/B 对照与基线，逐字节零回归）。

### 11.0 一句话

**Pivot Painter 式逐枝枢轴旋转 + 朝向继承度（硬↔软）+ 链外平动湍流**；关节表由 Blender 自产。
行业对照：UE Pivot Painter 2.0 / SpeedTree per-bone wind（flexibility·gravity）/ GPU Gems 3 ch.16。

### 11.1 数据模型（相对 §1 的增量）

| 载体 | 内容 | 备注 |
|---|---|---|
| **关节表**（资产 node `extras.jpov_wind_joints`）| 每关节 **7 float**：`pivot.xyz, parent, level, phase, soft` | `parent = -1` 表示根 |
| GPU 纹理（每 mesh 一张）| RGBA32F **2N×1**：`texel(2j)=(pivot.xyz,parent)`、`texel(2j+1)=(level,phase,soft,0)` | 占 `kTexUnitWindJointTable = 12`（**必须留在 0..15**）|
| 顶点 `loc3 aWindLink`（TEXCOORD_2）| `(joint_id, link_w)` | `link_w` 当前**未使用**（见 11.5）|
| 顶点 `loc6 aFlex`（TEXCOORD_1）| `(softness, phase)` | 仅平移式使用；平动湍流复用其 `phase` |

**逐关节量必须只依赖关节自身**（`level/phase/soft` 都编在表里）——否则同一关节对不同顶点给出不同 θ ⇒ 段内不再刚性。

### 11.2 烘焙（`tools/vegetation/wind_bake.py` / `build_tree.py`）

- 折线**抽稀**成关节（**按枝粗细分级**：level ≤ 1 用 1.5 单位/≤6 个；level ≥ 2 用 0.5 单位/≤8 个）⇒ 细柳条有足够关节去弯。
- **合并同位置关节**（Sapling 把一条枝拆成多条**续接**样条，续接处会出现两个同坐标关节 ⇒ 必须并入同一个，否则同处叠两次铰链 + 相邻顶点分属不同链 ⇒ 撕裂）。
- 顶点归属 = **沿弧长的段**（floor-by-arc，不是最近点）+ ε 容差内优先更靠根的链 + **同位置顶点强制同链**。
- `soft`（朝向继承度）判据：**根部 z 高于尖部 z（下垂）且管半径中位数 < 0.01** ⇒ `s_spline = clamp(droop/0.5,0,1)`，沿枝渐变 `s_k = s_spline·(弧长/枝全长)`（接头硬、梢部软）。实测：柳条 103/183；树皮顶点 `s>0.25` 占 25%。
- 关节表随资产走：写进 trunk 对象的自定义属性 + `export_extras=True`（**枢轴要转成导出坐标系**：Blender Z-up → glTF Y-up，`(x,z,-y)`）。

### 11.3 链算法（VS，根→梢递推）

```
q_j = q_parent + R(A_parent)·(pivot_j − pivot_parent)      // 位置：**完整**继承
A_j = θ_j + (1 − s_j)·A_parent                             // 朝向：按 (1−s) 打折
p'  = q_own + R(A_own)·(p − pivot_own)                     // 顶点
θ_j = A · kWindPivotGain · (0.15·level) · sin(2π·f0(1+1.6·level)·(t+phase_j))
```
- **每段仍是刚体 ⇒ 保长**；`q_child` 与"父链对 `pivot_child` 的映射"逐点一致 ⇒ **接头焊死**。
- `s=0` 硬（朝向全继承，木质梁）；`s=1` 软（只随自己的 θ，保持自身下垂姿态）。**根部 level 0 ⇒ 增益 0 ⇒ 固定端不动**（固定支承；这是最初的一个设计错误）。
- 实现要点：由顶点关节沿 `parent` 收集链（≤ `kMaxWindChain = 16`）后**反向遍历**；父关节解码必须 `int(floor(w+0.5))`（`-1` 哨兵：GLSL `int()` 向零截断会把 `int(-0.5)` 变成 `0` ⇒ 环回 joint 0 ⇒ 整株被扳倒）。

### 11.4 平动湍流（**链外**，逐顶点）

```glsl
p += tdir · ( amp · sin(2π·f_turb·(t + phase)) )
amp  = uWindTurbAmp · softness · (0.5 + 0.5·level) · gust      // 属性耦合；两个滑条独立
f_turb = uWindTurbFreq · (1 + kTurbLevF·level)
gust = mix(1, pow(max(0, 0.5+0.5·sin(2π·f_gust·(t+phase))), kTurbGustP), uWindGustStr)
tdir = 固定的"斜上"体轴（不落在风向平面）
```
- **为什么逐顶点而不是逐关节常量**：纯平移**不能改变朝向**；逐关节常量会在每个接头拉出 `|δ|` 的**台阶**。用连续 `phase` 场驱动 ⇒ 位移场连续 ⇒ 不撕裂（空间波长 ~3.5 单位 ⇒ 分支尺度上梯度只有毫米级）。
- **相位必须含 `uWindTime`**（踩过：漏了它 ⇒ 湍流成静态位移，且"频率"退化成**空间**频率 ⇒ 高频时位移场梯度爆炸 ⇒ 关节被扯开）。

### 11.5 参数总表

| 面板滑条 | 默认 | 作用 |
|---|---|---|
| 风向方位角 / 风速 | 35° / 1.6 | 风向（弯曲平面 + 噪声扫过方向）；**风速不决定"歪多少"** |
| 振幅 | 0.6 | 结构角总闸（`θ ∝ 振幅`）|
| 频率 Hz | 0.9 | 结构基频（各 level 按 `1+1.6·level` 分档）|
| 湍流（平移式） | 0.5 | **仅平移式**的逐顶点细节颤 |
| 平动湍流幅度 | 0.15 | **独立**：平动湍流幅度（0 = 关）|
| 平动湍流频率 Hz | 2.5 | **独立**：抖动快慢（纯时间频率）|
| 阵风频率 Hz | 0.6 | 多久来一阵 |
| 阵风强度 0=关 | 0.7 | 起伏深度（0 = 关起伏，湍流恒开）|
| 枢轴式风（勾选） | 关 | `mode` 0/1 |

常数（shader，集中在 `instanced_object_renderer.h`）：`kWindPivotGain=0.4`、`kMaxWindChain=16`、
`kTurbLevF=0.5`、`kTurbGustP=4.0`。

### 11.6 验证（本地，GL-free 复刻 + headless 出图）

**四条固定自检**（`output/jpov_wind_viewer/jpov_wind_viewer`，headless 参数齐备）：
1. **模式 A ≠ B**：枢轴 vs 平移同帧显著不同（`mean|d| ≈ 68`）——曾为 0（资产没同步 ⇒ 静默退回平移式）；
2. **零振幅（+ 湍流 0）== 静止**：逐像素相同（`mean|d| = 0.0003`）；
3. **湍流独立生效**：振幅=0 时改"平动湍流幅度"仍显著变化（`mean|d| ≈ 32`）；
4. **帧间在动**：湍流开启后相邻帧必须不同（冻结类 bug 只有它能抓——踩过）。

几何：叶片三角形面积比恒 **1.000**（逐卡刚体）；树干边（>15cm）最大伸长 ~1.19（a=0.6）。

### 11.7 已知边界 / 后续

- **只改了 instanced 路径**；`object3d` / 蒙皮路径仍只有平移式（两条 GLSL 是各自拷贝，改一处必须查另一处）。
- `link_w` 在枢轴模式下空闲；`aFlex=(softness,phase)` 也只在平移式 + 湍流相位里用 ⇒ **正好留给 `pos_lag`**（"每关节平移量"与平动湍流同管）。
- **与 shader 同源的边长/面积单测尚未落地**（本轮因"复刻与实现不同源"量错过两次）——**最高优先级**。
- 性能：链深 ≤16 × 2 texelFetch/环；本机 llvmpipe，真 GPU 需实测。
