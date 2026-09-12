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
| 面板 | 太阳仰角/浊度/季节R/地面/模型缩放 | 缩放/平移XYZ/旋转RX·RY/地面 |
| 左键 | 不消费 | **横向 drag = 模型旋转** |
| 右键 | 相机环绕（`ApplyInput`） | 同左（需求："依然服从相机右键拖动"） |
| 光照 | 滑条可调 | **固定 sunny day 同款** |
| 地面 | [-3, +3] 可调 | [-3, 0] 可调（需求定档） |
| headless | `--four_views` / `--round_video` | 无（交互工具；冒烟自检另有 smoke 程序） |

**为什么是两个 App 而不是一个带开关的 App**：两者手感与面板语义本就不同，
合并的代价是面板布局 + 输入消费全绕 `show_editor_` 分支（SOUL 忌讳的 demo 分叉
温床）。真正**必须**共享的是：

- `demo/model_placement.h` —— 放置数学（本工具新增，可单测）
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

| 输出 | 计算 |
|------|------|
| `scale` | 滑条值 [0.1, 10] |
| `center` | `(tx, ty, tz)`，各 [-3, +3] |
| `up` | `R · (0,1,0)` |
| `front` | `R · (0,0,1)` |

其中 `R = Ry(ry) · Rx(rx)`，两个旋转轴**固定在世界系**（不随模型自身姿态滚动，
避免"转着转着轴跑歪"）：

```
Ry = [ cos  0  sin ]      Rx = [ 1   0    0  ]
     [  0   1   0  ]           [ 0  cos -sin ]
     [-sin  0  cos ]           [ 0  sin  cos ]
```

`up`/`front` 均为单位向量（旋转保长），故后端内部的归一化是恒等操作。
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

### 3.2 角度用"折叠"而非"clamp"

旋转是**连续累积**量（拖满一屏 = 360°）。若用 `std::clamp(-180, 180)`，角度会
卡死在 ±180（拖过头反而"弹不回"）。故用 `WrapDegToHalfOpen` 折叠到半开区间
`[-180, 180)`——角度的等价类本就是 `mod 360°`。

> 缩放/平移仍用 `clamp`（非周期量）。这个区别是刻意的，且被
> `TestWrapRotation` / `TestClampScaleAndTranslate` 分别锁死。

## 4. 文件与构建

| 文件 | 职责 |
|------|------|
| `demo/model_placement.h` | ★ 放置数学（纯函数、header-only、可单测、与渲染零耦合） |
| `demo/editor_app.h` | 渲染核心 App（场景 + 放置面板 + 左键旋转消费） |
| `demo/jpov_model_editor.cc` | 主程序（装配 + 交互事件循环） |
| `demo/jpov_model_editor_smoke.cc` | headless 冒烟渲染（开发自检，非交付产物） |
| `demo/model_placement_test.cc` | 放置数学单测（11 个用例） |
| `build_jpov_model_editor.sh` | sh 包裹编译 → `output/jpov_model_editor/`（含字体拷贝） |

```bash
# 编译
./tools/jpov/build_jpov_model_editor.sh
# 运行
output/jpov_model_editor/jpov_model_editor /absolute/path/to/model.glb
# 单测
bazel test //tools/jpov:model_placement_test
```

## 5. 已知边界（非缺陷，是设计取舍）

1. **平移滑条范围 ±3m 是"人尺度"假设**。若资产本身只有 ~18cm（如仓库内
   `pliers.gltf`），±3m 位移相当于把它甩到相机视锥之外。这是**正确行为**
   （位移就是位移），但用小手模型时请配合"缩放"滑条或滚轮拉远相机。
   冒烟程序 `jpov_model_editor_smoke` 里对此有注释说明。
2. **相机 R 只按未缩放的包围盒自适应一次**（初始化时）。把缩放拉到 2 以上可能
   让模型出框——用滚轮 zoom 即可，本 PR 不做"跟随缩放自动重新取景"。
3. **不做资产保存**（显式非目标）。摆放结果目前仅供人眼配准与后续 PR 反推。

## 6. 自检记录（开发时实测）

- `model_placement_test`：11 用例全绿；**5 项负向验证**均正确 FAIL
  （组合顺序写反 / rx 符号写反 / ry 符号写反 / 折叠换成 clamp / 缩放不 clamp）。
- `jpov_model_editor_smoke`：渲染 6 张对比图（恒等 / 缩放2 / 平移X / rx45 /
  ry45 / 组合），人工核对确认四项能力均**可见生效**，且六图 MD5 互不相同。
  - ⚠️ 首轮冒烟曾出现"组合图与平移图字节相同"的假象，根因是**平移量按人尺度
    假设给了 1.5m，而该资产只有 18cm** → 模型整体出框成空场景（两张图都只剩
    背景，故相同）。这暴露出滑条量纲与资产量纲不匹配的可用性问题，
    已在 §5.1 记录，并把冒烟位移改为按资产尺度取 0.05m。
- 全量回归：`bazel test //... --jobs=1` → **66/66 通过**，gold 图零回归
  （`tools/jpov` 侧未触碰任何既有 gold 或渲染路径）。
