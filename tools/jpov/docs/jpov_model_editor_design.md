# JPOV 模型编辑器 — 设计说明

> 定位：retarget 流程的**配准工具**。目标是把一个 glTF/GLB 资产用人手微调到与
> 目标骨架（`Mixamo23Skeleton`）对齐的姿态——缩放、平移、旋转。
>
> **本 PR 只做"摆"，不做资产保存**；摆放数学产出的 `(center, up, front, scale)`
> 即后续 PR 反推"顶点调整量 / 骨长比例"的输入。

## 1. 与查看器（jpov_model_viewer）的分工

| 维度 | jpov_model_viewer | jpov_model_editor（本工具） |
|------|-------------------|------------------------------|
| 目的 | 光照标定（滑条调 elev/turb/season） | 模型摆位（缩放/平移/旋转） |
| 面板 | 太阳仰角/浊度/季节R/地面/模型缩放 | 缩放/平移XYZ/地面（**无旋转滑条**） |
| 左键 | 不消费 | **横向 drag = 模型旋转**（拖面板滑条时不旋转，见 §6） |
| 右键 | 相机环绕（`ApplyInput`） | 同左（需求："依然服从相机右键拖动"） |
| 光照 | 滑条可调 | **固定 sunny day 同款** |
| 地面 | [-3, +3] 可调 | [-3, 0] 可调（需求定档） |
| headless | `--four_views` / `--round_video` | 无（交互工具；冒烟自检另有 smoke 程序） |

**为什么是两个 App 而不是一个带开关的 App**：两者手感与面板语义本就不同，
合并的代价是面板布局 + 输入消费全绕 `show_editor_` 分支（SOUL 忌讳的 demo 分叉
温床）。真正**必须**共享的是：

- `demo/editor/model_placement.h` —— 放置数学（本工具专属，可单测）
- `demo/view_config.h` —— 场景构造（光照/地面/相机，两边共用，未改动）

判据（Danis 定调）：**"一片代码不能单测，就不要为共享而共享"**——放置数学已抽到
独立头文件并被 `model_placement_test` 覆盖，`EditorApp` 只留"接线 + 布局"。

## 2. 变换语义（与渲染后端严格对齐）

后端的 `BuildModelMatrix`（`src/object3d/object3d_renderer.cc`）语义为：

```
局部坐标轴： +X = left = normalize(cross(up, front))， +Y = up， +Z = front
顶点变换：   v_world = T(center) · R(up, front) · (scale · v_local)
```

即**缩放 → 旋转 → 平移**（经典顺序）。`BuildModelMatrix` 用 `(up, front)` 重建
正交基（`right = cross(up,front)`），因此本工具只需提供：

| 输出 | 来源 |
|------|------|
| `scale` | 滑条值 [0.1, 10] |
| `center` | `(tx, ty, tz)`，各 [-3, +3] |
| `up` | 状态里的 `up` 矢量（局部 +Y 的世界方向） |
| `front` | 状态里的 `front` 矢量（局部 +Z 的世界方向） |

`up`/`front` 均为单位向量且互相垂直（由绕世界轴的增量旋转保持，见 §3.2），
故后端内部的归一化是恒等操作。
**后端零改动**——本 PR 的全部编辑能力就是喂给既有 `DrawGltfObject` 的四个入参。

## 3. 交互映射

### 3.1 左键横向 drag → 旋转（需求逐条落实）

| 按键 | 轴 | 右滑方向 |
|------|----|----------|
| 无 Ctrl | 绕**世界 X** 轴（俯仰） | 逆时针 |
| 按住左 Ctrl | 绕**世界 Y** 轴（偏航） | 逆时针 |
| 纵向 drag 分量 | **忽略** | — |

**"右滑逆时针"的符号推导**（右手系，从被说到的正轴一侧朝原点看）：

- **绕 +X 看**：`+Y → +Z`。向右滑即沿 +X 转轴方向看是逆时针 → 角速度 `+dx·k`
  → **`rx += dx·k`**。
- **绕 +Y 看**：世界 XZ 平面里 `+Z → +X` 才是逆时针（+Y 从上看时 X 为右、Z 为
  朝观察者下方；直接朝 +Z 看是顺时针）→ 角速度 `-dx·k` → **`ry -= dx·k`**。

两个符号差异内聚在 `ApplyRotateDrag`，调用方只传"是否按 Ctrl"。

**像素 → 角度**：拖满一屏宽 = 360°（与查看器相机"一屏一圈"手感一致）。

**旋转轴在 drag 起点冻结**：按下左键那一刻的 Ctrl 状态决定本次 drag 用哪个轴；
drag 中途切换 Ctrl **不换轴**（否则手心打滑）。由 `rotate_mode_latched_` 实现。

### 3.2 ⭐ 朝向状态是 up/front 矢量，不是欧拉角

**这是本模块最重要的设计决策**（经 Danis 指正后才定下来）。

朝向的状态量就是 **(up, front) 这对矢量本身**，不是任何两个 float 角度：

| | 两个 float 角度（旧/错误） | (up, front) 矢量（现行） |
|---|---|---|
| 自由度 | 2，且第二次旋转的轴是"第一次转完后的坐标系轴" | 完整朝向，可任意累积 |
| 任意顺序的世界轴累积 | ✗ 做不到（固定轴序参数化） | ✓ 每步作用在当前矢量上 |
| 跨帧状态语义 | 存了参数化的中间量 | 存姿态本身（无损） |

**交互入口只有两个增量旋转函数**（都作用于当前矢量、不重新参数化）：

```cpp
void ApplyYawDelta(ModelPlacement* p, float deg);    // 绕世界 Y（up/front 同施）
void ApplyPitchDelta(ModelPlacement* p, float deg);  // 绕世界 X（up/front 同施）
```

例：先绕世界 Y 转 90° 再绕世界 X 转 90°：
- 矢量累积：`up` 最终 `(0,0,1)`、`front` `(1,0,0)`
- 若用 `R = Ry(ry)·Rx(rx)` 重算：`up` 得 `(1,0,0)` —— **不同**

这个差异由 `TestWorldAxisAccumulationAnyOrder` 锁死（把实现改回欧拉角参数化会 FAIL）。

**正交归一不变量**：两次增量都绕世界轴、初始正交归一，故 up/front 在整个累积
过程中保持单位长且垂直 → `BuildModelMatrix` 内部的归一化是恒等操作。
由 `TestOrthonormalUnderAccumulation`（多轴非平凡序列）守卫。

**因此没有旋转滑条**：滑条的状态是一个 float，与 "朝向 = 两个矢量" 的语义
不匹配（存不下任意朝向）。旋转**只能**通过左键横向拖动完成。

### 3.3 UI 布局（**随窗口尺寸自适应**）

| 元素 | 位置 | 样式 |
|---|---|---|
| 操作说明（4 行） | **左上角** | **黑色文字、真左对齐** |
| 滑条（5 条：缩放 / 平移XYZ / 地面高度） | **左下角** | 宽度 = 半屏（比例固定） |

**关键：布局全部从「本帧窗口尺寸」推算，不用编译期常量。**

2D 指令的坐标空间是**每帧渲染分辨率**（= 当帧窗口尺寸，见
`render_command.h` 对 `Rect2D` 的说明），所以：

```cpp
// OneIteration：
const float fw = winfo.width, fh = winfo.height;   // 每帧随窗口变
cmds->camera.fbo_3d_width_  = (int)fw;             // 渲染分辨率跟着窗口
cmds->camera.fbo_3d_height_ = (int)fh;

PanelLayout L = MakeLayout(fw, fh);                // 布局也跟着窗口
```

`PanelLayout::Row(i)` / `Help()` 的几何即由此算出：滑条左缘贴边距、最后一行
底缘贴屏底（左下角），说明贴左上角。**尺寸固定、位置随窗口平移** —— 即
"尺寸可固定，但位置整体贴左下角"。

> ⚠️ 踩过的坑：早期版本用常量 `kEditorHeight=720` 算布局，窗口放大后滑条
> 飘在中间偏上。常量现已改名 `kEditorDefaultWidth/Height` 并注明**仅表示
> 开窗默认尺寸，不是布局常量**。`TestLayoutScalesWithWindow` 锁死这一点。

**说明文字为何不走 `Ui::Text`**：`Ui::Text` 内部写死 `kCenter` 对齐 +
写死 `theme_.foreground`（浅色），无法满足"左对齐 + 黑字"。故直接向
`cmds` 发 `DrawText(..., kTopLeft, alias)`。上一版用"估字符宽 + 居中"凑
左对齐，估宽不准就参差不齐（"排版怪"的根因）。

两处几何仍是**单一真相**且被 `editor_drag_ownership_test` 守卫：
- `PanelLayout::Row(i)` 同时供绘制和 `PointInPanel`（防"能画的滑条拖不动"）；
- `PanelLayout::Help()` 同时供绘制和 `PointInPanel`（说明区也计入面板，
  避免它成为模型旋转的**隐形触发区**）；测试断言它与滑条区不重叠。

### 3.4 缩放/平移用 clamp，朝向不 clamp

缩放/平移是非周期量 → `std::clamp` 到各自区间（超界夹断）。
朝向是单位矢量，无"越界"概念 → 不做 clamp（早期版本对角度做过 mod 360 折叠，
角度被移除后该逻辑一并删除）。

## 4. 文件与构建

**全部代码/测试自洽在 `tools/jpov/demo/editor/`（独立 Bazel 包）下**，不散落到上层：

| 文件 | 职责 |
|------|------|
| `demo/editor/model_placement.h` | ★ 放置数学（纯函数、header-only、可单测、与渲染零耦合） |
| `demo/editor/editor_app.h` | 渲染核心 App（场景 + 放置面板 + 左键旋转消费） |
| `demo/editor/jpov_model_editor.cc` | 主程序（装配 + 交互事件循环） |
| `demo/editor/model_placement_test.cc` | 放置数学单测（12 个用例） |
| `demo/editor/editor_drag_ownership_test.cc` | 左键 drag 归属回归（见 §6） |
| `demo/editor/BUILD` | 本工具的独立 Bazel 包 |
| `build_jpov_model_editor.sh` | sh 包裹编译 → `output/jpov_model_editor/`（留在上层与其他 build_*.sh 一致） |

仅与查看器共用 `demo/view_config.h`（场景构造，未改动）与
`//tools/jpov:view_config` 库。

```bash
# 编译
./tools/jpov/build_jpov_model_editor.sh
# 运行
output/jpov_model_editor/jpov_model_editor /absolute/path/to/model.glb
# 单测
bazel test //tools/jpov/demo/editor:all
```

## 5. 已知边界（非缺陷，是设计取舍）
1. **平移滑条范围 ±3m 是"人尺度"假设**。若资产本身只有 ~18cm（如仓库内
   `pliers.gltf`），±3m 位移相当于把它甩到相机视锥之外。这是**正确行为**
   （位移就是位移），但用小手模型时请配合"缩放"滑条或滚轮拉远相机。
2. **相机 R 只按未缩放的包围盒自适应一次**（初始化时）。把缩放拉到 2 以上可能
   让模型出框——用滚轮 zoom 即可，本 PR 不做"跟随缩放自动重新取景"。
3. **不做资产保存**（显式非目标）。摆放结果目前仅供人眼配准与后续 PR 反推。
4. **窗口可缩放**（`cfg.resizable = true`，与查看器的不可缩放不同）：布局已能
   随窗口自适应（见 §3.3），放大后滑条/说明会重新贴边。

## 6. 左键 drag 归属（面板 vs 视口）—— 曾出过 bug，已修

左键同时承担两个职责：拖滑条（面板）与转模型（3D 视口）。二者靠**按下起点**区分：

| 按下位置 | 本次按住期间的归属 |
|---|---|
| 底部面板矩形内 | 面板 —— 旋转逻辑完全不插手（交给 Ui 滑条） |
| 3D 视口区域 | 视口 —— 正常旋转（无 Ctrl 绕世界 X / Ctrl 绕世界 Y） |

归属在**按下起点冻结**，中途拖出/拖入面板不改归属（与 Ui 滑条"drag 一旦
开始判定区不作数"的语义对称）。

> **首版 bug（已修）**：原实现无条件消费任何左键 drag，导致拖"平移 X"滑条时
> 同一个 `mouse_dx` 被滑条与旋转逻辑各吃一次 → tx 与 rx 同时变，表现为
> "滑条串行"。归属判定 + 该回归由 `//tools/jpov:editor_drag_ownership_test`
> 锁死（5 条路径 + 4 项负向验证）。

两个容易再犯的坑：
1. **归属必须在左键按下的那一帧记下**，不能等 `IsDrag()` 转真才判 ——
   按下后先 Hold（原地不动）再移动时，IsDrag 转真那帧鼠标已在别处，
   用那时坐标会把"起于面板"误判成"起于视口"。
2. **面板几何必须单一真相**：`PanelLeft/PanelTop/PanelRow` 同时供"画控件的 box"
   与"判定归属的面板矩形"使用，杜绝两处各写一份几何。

## 7. 世界坐标架（2026-09-14 追加）

编辑器场景里摆一个 **1m × 1m × 1m 的世界坐标架**，供用户目测模型朝向与尺度：

| 项 | 值 |
|---|---|
| 三条带 | 分别沿世界 **+X / +Y / +Z** 正方向 |
| 长度 | **1m**（需求定档） |
| 宽度 | **5cm**（半宽 2.5cm），**无厚度**（单面条带） |
| 颜色 | **X=红 / Y=绿 / Z=蓝**（图形学通行约定） |
| 透明度 | **alpha 0.2**（"几乎透明"，不遮挡被编辑的模型） |
| 位置 | **世界原点 `(0,0,0)` 且跟随模型平移**（见 §7.1b） |
| 法线 | X 带朝 **+Z**；Y 带朝 **+X**；Z 带朝 **+X** |
| 可见性 | 每条带**正反各画一次** → 双面可见（见 §7.2） |

### 7.1 用 3D 条带，不动 Object3D 着色通路（Danis 定调）

需求原文说"用 make box 做直棱柱"。首版照做（`MeshData::MakeOrientedBox` +
每轴一个 mesh + `DrawObject3D`）—— 但 `DrawObject3D` 的片元 shader 此前
**写死 `FragColor.a = 1.0`**、没有任何 alpha 混合路径，为了画三根半透明辅助线
就得去动 **Object3D 的着色通路**（加 uniform、加混合状态、改深度写入…）。

Danis 定调：**不为辅助线动 Object3D 的 alpha 通路，减少影响面**。改用既有的
**3D 条带**（`RenderCommandList::DrawStrip3D`）：

- 它的 FS 是 `FragColor = uColor`（`primitives3d_renderer.h: kFs3d`）→
  **alpha 天然生效**，渲染后端**一行都不用改**（本 PR 现在对
  `src/object3d/`、`src/renderer.*`、`src/skeleton/` 零改动）；
- 无光照、无 PBR —— 正是辅助线要的"恒定鲜艳色"；
- 代价：无厚度、单面。对 1m 长的方向指示完全够用（用户只看朝向）。

编辑器侧：`EditorApp::DrawAxisGizmo()` 每帧造顶点直接发 3 条带 × 2 次
（正反各一）`DrawStrip3D`（条带是 stream 数据，无需注册 GPU mesh；开销可忽略）。
位置见 §7.1b。说明文字里也加了一行
`三色坐标架（随模型）：红 = X 轴 · 绿 = Y 轴 · 蓝 = Z 轴（各 1m）`。

### 7.2 🔑 绕序（winding）是**功能性**的，不是审美

条带是单面几何，而 `Draw3DCommands` 入口开着 `GL_CULL_FACE` + CCW → **法线背对
相机的那一面会被剔除、整条消失**。实测踩过：三带统一用一种绕序时，X 带恰好
法线朝相机（可见），而 **Y / Z 带被剔除、画面上完全看不到**。

`MakeAxisStripVertices` 因此提供两个等价绕序（`normal_toward_side`），按
"法线 = side × axis" 推导逐带选择：

| 带 | axis | side | side × axis | 需要的法线 | 绕序 |
|---|---|---|---|---|---|
| X | +X | +Z | **+Y** | +Y | winding A |
| Y | +Y | +Z | **−X** | +X | 需翻 → winding B |
| Z | +Z | +X | **−Y** | +Y | 需翻 → winding B |

单测 `AxisGizmoStrips.NormalsMatchRequirementSigned` **断言带符号法线**
（不是 `fabs`）——因为"法线朝哪一侧"直接决定这条带可见还是消失。

### 7.3 历史教训（首版棱柱方案，已弃）

1. **长轴必须喂 `up`（局部 +Y），不是 `front`。** 喂错了 box 仍是一根长条，
   但躺在错误的轴上（肉眼像"转了 90°"）。
2. **半透明 + 纯漫反射会让颜色"看不见"。** 首版只给 `base_color`（纯色漫反射）：
   杆被太阳照时六面明暗不一，背光面只剩 `ambient(≈0.3) × base_color`，乘
   alpha 0.2 混到浅灰地面后**色差仅 3/255** —— 画面上是根"灰杆"，看不出红绿蓝。
   当时靠加 `emissive` 解决；改用条带后**问题自然消失**（条带 FS 无光照，
   颜色恒定）—— 这也是"少一条复杂通路就少一类坑"的又一例。

### 7.4 自检

- `interface/axis_gizmo_test`（**13 用例**）：轴向 / 起点贴原点 / 宽度 5cm /
  零厚度 / **带符号法线（三带朝向约定）** / **反转绕序必须反向法线** /
  顶点构成合法四边形（面积 = 长×宽）/ 颜色常量 / `AxisGizmoName`。
  **负向验证**均正确 FAIL（X 带绕序写反 / Z 带用旧宽度方向 / 反转绕序未生效 /
  带宽改 10 倍）。
- `demo/editor/axis_gizmo_render_test`（headless 像素自证）：
  ① 三带在地面区域各能找到"该色调占优"像素（margin 55/50/51，阈值 6）；
  ② 非主通道之和 ≥ 60（半透明生效；不依赖主通道绝对值）；
  ③ 三带最强像素"分离"仅作**警告**（同点发散的三条带在某些合理机位下会相邻，
     硬断言会把"换个机位"误判成回归；真正锁住不串位的是 ①②④）；
  ④ **直接对照**：同场景再渲一张 alpha=1.0，半透明必须更靠近背景色
  （实测 1881 vs 64178 / 1824 vs 65198 / 1894 vs 65198）；
  ⑤ **对面视角**三带仍可见（双面自证，实测两边都是 55/50/51）。
  地平线由图像自身检测（`DetectHorizonY`），**不写死行号** —— 写死的行号会随
  相机/分辨率改动**静默失效**。
  **负向验证**：alpha 常量改 1.0 → 正确 FAIL；**只画单面（去掉反向那次）**
  → 对面视角三带全消失、正确 FAIL。
- 全量回归：`bazel test //... --jobs=1` → **74/74 通过**（216 cases），
  所有 gold 图 `max-channel-mean-diff = 0`。
  Windows 交叉编译（`--config=windows`）通过（顺手修了 `editor_save.cc` 的
  MinGW `_USE_MATH_DEFINES` 顺序坑 —— 属保存 commit 的既有问题，见 §8）。

### 7.5 已知边界（非缺陷，P8 review 时核实）

1. **单面无厚度**：从背面看本会被裁剪，已用"正反各画一次"解决（见 §7.2b）
   —— 不关剔除（减少影响面），代价是 draw ×2。
2. **半透明物体投不透明阴影**：`DrawStrip3D` 不参与阴影 pass（那个 pass 只遍历
   `cmds.object3d` / `cmds.skinned_mesh`）→ 坐标架**根本不投影**，无此问题。
3. **顺序问题**：条带走全局 `GL_BLEND`（`Render()` 入口开启），经典
   `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` 非交换。三带从同一点发散、几乎不重叠，
   当前视觉无差别；将来如需真正的顺序无关，那是渲染管线级改动（独立议题）。
4. **颜色不做 sRGB 解码**：条带色直传 `uColor`。实测在编辑器浅灰地面上三色清晰，
   故不加。

## 8. 自检记录（开发时实测）

- `model_placement_test`：**12 用例全绿**（含世界轴任意顺序累积、多轴正交归一、
  整圈回位）；**3 项负向验证**均正确 FAIL（退回欧拉角参数化 / pitch 漏转 up /
  yaw 符号写反）。其中"退回欧拉角参数化"直接证明 §3.2 的设计动机成立。
- `editor_drag_ownership_test`：面板/视口归属 5 条路径 + 布局自洽 4 组 + 负向
  验证全过。含 `TestLayoutScalesWithWindow`（窗口放大到 1920×1080 后底缘仍贴
  新屏底、宽为新半屏、说明仍贴左上、归属判定仍正确）。
- 交互工具不提供 headless 冒烟程序（开发期曾有，验收时删除：editor 本身是
  交互工具，看效果直接开窗口，出图脚本对该工具无交付价值）。
  - ⚠️ 首轮冒烟曾出现"组合图与平移图字节相同"的假象，根因是**平移量按人尺度
    假设给了 1.5m，而该资产只有 18cm** → 模型整体出框成空场景（两张图都只剩
    背景，故相同）。这暴露出滑条量纲与资产量纲不匹配的可用性问题，
    已在 §5.1 记录，并把冒烟位移改为按资产尺度取 0.05m。
- 全量回归：`bazel test //... --jobs=1` → **74/74 通过（216 cases）**（含新增的
  drag 归属测试 + 坐标架两项测试），gold 图 `max-channel-mean-diff = 0`
  （`src/object3d` / `src/renderer.*` / `src/skeleton` 本次**零改动**，
  见 §7.1 —— 这是"改用条带"换来的最大收益）。

### 8.x MinGW `_USE_MATH_DEFINES` 顺序坑（本次顺手修）

`editor_save.cc` 在 Windows 交叉编译下报 `M_PI` / `M_PI_2` 未声明。
根因是老坑（PR #90 踩过）：**`_USE_MATH_DEFINES` 必须在首次 include `<cmath>`
之前定义**才在 MinGW 生效，而该文件经 `mesh_transform.h → geom/math_util.h`
用到 `M_PI`，系统头先把 `<cmath>` 拉进来了。

修法（与 `render_command.h` / `skeleton_manager.cc` 同款）：在 include 项目头之前
加 `#ifdef _WIN32 / #define _USE_MATH_DEFINES / #include <cmath> / #endif`。

注：这是**保存 commit 的既有问题**（不是我这次坐标架改动引入的），
但它让 `--config=windows` 建 `editor_app` 失败，所以在本次一并修掉。
