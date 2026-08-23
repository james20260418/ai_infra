# JPOV UI 轻量级即时模式控件 — 开发计划纲领

> 状态：纲领定稿，待合入后交由 bulletin worker 按此推进
> 分支：`feature/20260822-lightweight-ui`
> 类型：计划书（先定纲领 → 提 PR 合入 → 子步骤逐项开发验收）

## 0. 目标（一句话）

给 JPOV 提供一套**轻量级即时模式 UI**：Agent 与设计者共同操作的运行期调试面板/参数台，
无布局引擎（像素 box 显式定位）、状态外置、可自测（gold 图比对）。

### 非目标（明确不做）
- 不做自动布局 / 行列容器 / 布局引擎
- 不做窗口行为（拖动、resize、焦点、遮挡）
- 不做复杂表单 / 表格 / 网格
- 不做跨帧状态记忆（状态一律外置）

---

## 1. 架构决策（已定稿，不再讨论）

1. **即时模式流水账**（ImGui 路线），而非可复用 Panel 布局树。
   - 复用靠 **函数包装 + for 循环 + 每帧传不同 state** 获得；
   - 换来：免去布局树数据结构 / 偏移绑定 / 生命周期管理的全部复杂度。
2. **一个 Ui 对象 = 一个 root 面板**，用户跨帧持有 Ui。
3. **布局 = 像素 box 显式定位**：每个控件必须给 `UiRect`，布局真相内聚于一个 struct。
   box 内默认 **NEWS 全面覆盖**，可传 `Stretch` 掩码覆盖某方向拉伸实现对齐。
4. **控件状态外置**：用户提供参数指针（`float*`/`bool*`/`char*`/`int*`），交互写回。
   每帧从零构建，无状态渲染。
5. **容错原则**（已写入 ui.h 注释，实现时遵守）：
   - box 过小 fallback 保全功能，**字号绝不缩小**（原尺寸 + 裁切）；
   - 圆角/内边距/句柄直径 clamp 到合法范围，绝无负尺寸/NaN；
   - 越界剔除：与视口 AABB 不相交则跳过；部分相交照画（GPU 兜底）。

### 接口形态（定稿，见 `tools/jpov/interface/ui.h`——**细节一切以 ui.h 注释为准**）
```cpp
class Ui {
  void Begin(const InputSnapshot&, const UiTheme&, float width, float height);
  void End();
  void Emit(RenderCommandList*);
  void Text(const char*, const UiRect&, Stretch = NEWS);
  bool Button(const char*, const UiRect&, Stretch = NEWS);
  bool Checkbox(const char*, bool*, const UiRect&, Stretch = NEWS);
  bool SliderFloat(const char*, float*, const UiRect&, float, float, Stretch = NEWS);
  bool InputText(const char*, char*, size_t, const UiRect&, Stretch = NEWS);
  bool Combo(const char*, int*, const std::vector<const char*>&, const UiRect&, Stretch = NEWS);
  void ColorSwatch(const char*, const Color&, const UiRect&, Stretch = NEWS);
  static bool Hit(const UiRect&, float, float, const InputSnapshot&);
  static UiRect SanitizeBox(const UiRect&);
  bool OutsideViewport(const UiRect&) const;
};
```
- `UiRect{pos{vec2f}, size{vec2f}}`：**Ui 面板局部坐标、像素单位**（非窗口绝对坐标）
- `UiTheme`：颜色组 + 像素系数量（padding/spacing/corner/border）
- `Stretch`：位掩码 `W/E/N/S`，默认 `NEWS` 全铺满

---

## 2. 子步骤拆解

> **两种验收形态（贯穿每个子步骤）：**
> - **交互式 demo main**：可运行窗口，鼠标可操作，实时可见反馈（点/拉/选/打字）。
> - **单帧渲染自证测试（jpov 特有链路）**：不经窗口，直接 `Ui.Begin→控件→Emit`
>   拿到 `RenderCommandList`，与**黄金指令（gold）比对**，验证像素级输出正确。
>   每张 gold 图/指令集都是"单帧自证"，确保无回归。
> - 每个子步骤**先写单帧自证测试**（快、稳、可被 CI），**再补交互 demo**（给人看）。

### S0. 基础设施（Ui 骨架）
- [ ] S0.1 `interface/BUILD` 新增 `ui` 库（`ui.h` + 待建 `ui.cc`），挂入 `jpov_interface`
- [ ] S0.2 `ui.cc`：`Begin/End/Emit` 三方法 + 指令缓冲收集
- [ ] S0.3 视口宽高记录 + 空面板行为
- [ ] **验收**：
  - 自证：空面板 `Emit` → 0 条指令；单矩形 → 1 条 `FillRect2D`；gold 指令比对
  - demo（可选）：空屏窗口，无控件不崩溃

### S1. 文本 Text + 色块 ColorSwatch + 规格化
- [ ] S1.1 `Text` 绘制：字形原尺寸垂直居中；box 过小裁切不缩字号、不溢出
- [ ] S1.2 `ColorSwatch`：真方形，box 不足取 min(w,h) 居中
- [ ] S1.3 `SanitizeBox`：负尺寸 clamp 0；圆角 clamp ≤ min(宽,高)/2
- [ ] S1.4 `OutsideViewport`：AABB 判交
- [ ] **验收**：
  - 自证：几个不同 box 的 Text/Swatch → gold 指令；负尺寸 box → 规格化后无 NaN；
    越界 box → 被剔除（指令 0 条）
  - demo：摆若干文本/色块，悬停/展示

### S2. 按钮 Button + 命中 Hit
- [ ] S2.1 `Button` 绘制（底色/圆角/文本）
- [ ] S2.2 `Hit` 命中测试（点在 box 内）
- [ ] S2.3 悬停态（鼠标进入变色）
- [ ] S2.4 点击返回 true（一次性事件）
- [ ] **验收**：
  - 自证：模拟点击 → 返回 true 且仅当帧；gold 指令普通态/悬停态
  - demo：连点计数显示，点按实时反馈

### S3. 复选框 Checkbox（状态外置）
- [ ] S3.1 `Checkbox` 绘制（方框 + 勾）
- [ ] S3.2 点击翻转 `*bool` 并返回 true
- [ ] **验收**：
  - 自证：点击后 `*value` 翻转；gold 勾选/未勾选两态
  - demo：点击勾选，旁边文本同步显示 ON/OFF

### S4. 滑条 SliderFloat（核心交互）
- [ ] S4.1 绘制（轨道 + 句柄 + 数值）
- [ ] S4.2 拖动：命中 → 横坐标比例映射 `[min,max]` 写回 `*float`
- [ ] S4.3 容错：句柄直径 clamp ≥ 8px；box 过窄轨道照画、句柄不缩 0
- [ ] **验收**：
  - 自证：多档位拖动 → `*value` 正确映射；边界（min/max）；gold 多档位指令
  - demo：拖动滑条，旁边数值实时跟变

### S5. 文本输入框 InputText
- [ ] S5.1 绘制（底框 + 光标 + 占位符）
- [ ] S5.2 焦点：点击获得；键盘输入写回 `char*`（限容量）
- [ ] S5.3 超长水平滚动（内部 scroll），不溢出
- [ ] **验收**：
  - 自证：模拟按键 → buffer 内容正确、超容量截断；gold 光标/文本态
  - demo：点击聚焦 → 打字 → 实时显示 buffer 内容

### S6. 下拉 Combo
- [ ] S6.1 绘制（当前项 + 下拉箭头）
- [ ] S6.2 点击展开选项列表；选择写回 `*int` 返回 true；点外部关闭
- [ ] **验收**：
  - 自证：选项切换 → `*selected` 正确；gold 展开/收起两态
  - demo：展开 → 选择 → 当前项实时更新

### S7. 集成 demo（端到端，完整调试台）
> ⚠️ 集成 demo 要求：**包含全部控件** + **一个实时 text 输出框**反馈"发生了什么"。
- [ ] S7.1 `jpov_ui_demo`：1280×720 窗口，综合全部控件组成调试台：
  - 标题、多行标签/输入、滑条组、勾选、下拉、按钮行、色块
  - 用 `Stretch` 掩码演示"贴左不铺满"等对齐
- [ ] S7.2 **实时 Log 输出框**：一个专用 Text/textbox 区域，把所有交互事件
  （按钮按下、滑条变更、勾选翻转、下拉切换、输入变化）以时间戳文本实时追加显示，
  供验收者确认每次操作都被正确捕获。
- [ ] S7.3 函数包装复用演示：for 循环画 2~3 份同一子面板（如多电机），各自独立 state
- [ ] **验收**：
  - 自证：整台一次 Emit → gold 指令全貌比对
  - demo（**Danis 交互式验收**）：窗口可操作，每个动作在 log 框实时反映，直观确认

### S8. 回归 & 文档
- [ ] S8.1 `ui.h` 使用示例与最终签名完全同步（已含复用伪代码）
- [ ] S8.2 全量 jpov 现有 test 全绿，无回归
- [ ] S8.3 需要时把本纲领归档到 docs/

---

## 3. 文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `tools/jpov/interface/ui.h` | 已新增（本 PR） | 接口定稿 + 全部细节注释 |
| `tools/jpov/interface/ui.cc` | 待建 | 实现 |
| `tools/jpov/interface/BUILD` | 修改 | 加 `ui` 库并挂入 `jpov_interface`（已完成） |
| `tools/jpov/test/ui/` | 待建 | 各组件单帧自证测试 + demo main + gold 指令 |
| `docs/jpov_ui_plan.md` | 本文件 | 计划纲领 |

---

## 4. 验收标准（总体）

1. 每个子步骤有**单帧自证测试**（gold 指令比对）+ **交互式 demo main** 两种验收。
2. 绘制组件有 gold 指令比对；交互组件通过**返回值 + state 外置指针**可程序验证。
3. 集成 demo **包含全部控件** + **实时 log 输出框**，可交互验收。
4. 容错落地：字号永不缩小；无负尺寸/NaN；越界剔除生效。
5. 全量 jpov 现有 test 全绿，无回归。

---

## 5. PR 计划

- 本 PR：#63（待合入），含**计划纲领 + ui.h 接口定稿 + BUILD 挂载**。
- 目的：锁定接口，让 bulletin worker 可并行按 S0→S8 推进开发与验收。
- 后续：S0~S8 由 bulletin worker 逐项执行，每项可交互验收 + gold 自证。
