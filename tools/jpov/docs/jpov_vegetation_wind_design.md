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

- 本版**平移式弯曲**（无 pivot/层级）；真弯曲（保长、枝继承）另开。
- 阴影 pass 同步施加同一位移（否则树动影不动）——本 PR 一并做。
- 四季节/落叶：风权重随季节降低（后续，非本 PR）。
