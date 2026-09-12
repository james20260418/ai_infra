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

## 7. 自检记录（开发时实测）

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
- 全量回归：`bazel test //... --jobs=1` → **67/67 通过**（含新增的 drag 归属测试），
  gold 图零回归（`tools/jpov` 侧未触碰任何既有 gold 或渲染路径）。
