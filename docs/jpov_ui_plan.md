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
3. **布局 = 像素 box 显式定位**：每个控件必须给 `UiRect`，布局真相内聚于一个 struct，
   一眼可读。box 内默认 **NEWS 全面覆盖**（控件填满 box，母变宽子跟着变宽），
   可传 `Stretch` 掩码覆盖某方向拉伸实现对齐。
4. **控件状态外置**：用户提供参数指针（`float*`/`bool*`/`char*`/`int*`），交互写回。
   每帧从零构建，无状态渲染，符合 JPOV 自测理念。
5. **容错原则**（细节后续再议，本纲领定精神）：
   - box 过小时尽量 fallback 保全基本功能，**字号绝不缩小**（原尺寸 + 裁切）；
   - 圆角/内边距/句柄直径等先 clamp 到合法范围，绝无负尺寸/NaN；
   - 越界剔除：与视口 AABB 不相交则跳过；部分相交照画（GPU 兜底）。

### 接口形态（已定稿，见 `tools/jpov/interface/ui.h`）
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
- `UiRect{pos{vec2f}, size{vec2f}}`（像素，原点左上）
- `UiTheme`：颜色组 + 像素系数量（padding/spacing/corner_radius/border）
- `Stretch`：位掩码 `W/E/N/S`，默认 `NEWS` 全铺满

---

## 2. 子步骤拆解（每个组件独立测试 main，交互式验收）

> 验收前置：每个子步骤都含一个可运行的独立 demo/test main，
> 产出可在窗口交互操作（鼠标命中、滑动、点击），或用 gold 图比对像素。

### S0. 基础设施（先于一切）
- [ ] 在 `interface/BUILD` 新增 `ui` 库（`ui.h` + 待建 `ui.cc`），挂入 `jpov_interface`
- [ ] `ui.cc` 骨架：`Begin/End/Emit` 三基础方法 + 指令缓冲收集 + 视口剔除调用
- [ ] **验收**：空面板 emit 出 0 条指令；带一个矩形 emit 出 1 条 `FillRect2D`；gold 图空屏

### S1. 基础文本与矩形（Text + ColorSwatch + 背景）
- [ ] `Text` 绘制：字形原尺寸居中，box 过小裁切不缩字号、不溢出
- [ ] `ColorSwatch`：真方形控件，box 不足取 min(w,h) 正方形居中
- [ ] `SanitizeBox`：负尺寸 clamp 到 0；圆角 clamp ≤ min(宽,高)/2
- [ ] `OutsideViewport`：AABB 判交
- [ ] **验收**：demo main 摆若干 Text/Swatch，gold 图验证位置字号；鼠标悬停高亮

### S2. 按钮（Button + 命中 + 点击反馈）
- [ ] `Button` 绘制（底色/圆角/悬停态）；`Hit` 命中测试
- [ ] 点击返回 true（一次性事件）；悬停变色
- [ ] **验收**：demo main，鼠标点按验证返回值；可连点计数；gold 图普通态/悬停态

### S3. 复选框（Checkbox，状态外置）
- [ ] `Checkbox` 绘制（方框+勾）；点击翻转 `*value` 并返回 true
- [ ] **验收**：demo main，点击验证 `bool*` 翻转；gold 图勾选/未勾选

### S4. 水平滑条（SliderFloat，核心交互控件）
- [ ] `SliderFloat` 绘制（轨道+手柄+数值显示）
- [ ] 拖动：命中手柄/轨道 → 按横坐标比例映射到 `[min,max]` 写回 `*value`
- [ ] 容错：手柄直径 clamp 到最小像素（≥8px）；box 过窄轨道照画、手柄不缩到 0
- [ ] **验收**：demo main，拖动验证 `float*` 连续变化与边界；gold 图多档位

### S5. 文本输入框（InputText）
- [ ] `InputText` 绘制（底框+光标+占位符）
- [ ] 焦点：点击获得焦点；键盘输入写回 `char*` buffer（限容量）
- [ ] 超长水平滚动（内部 scroll 偏移），不溢出
- [ ] **验收**：demo main，点击聚焦→键盘打字→验证 buffer；越界不打

### S6. 下拉选择（Combo）
- [ ] `Combo` 绘制（当前项 + 下拉箭头）；点击展开选项列表
- [ ] 选择切换 `*selected` 并返回 true；点击外部关闭
- [ ] **验收**：demo main，展开/选择验证；gold 图展开态/收起态

### S7. 集成 demo（端到端，复杂面板）
- [ ] `jpov_ui_demo`：一个 1280×720 窗口，综合上述全部控件组成调试台
  - 标题、多行标签+输入框排列、滑条组、勾选、下拉、按钮行、色块
  - 用 `Stretch` 掩码演示"贴左不铺满"等对齐
- [ ] gold 图全貌 + 交互验证（鼠标点、拉、选、打字）
- [ ] **验收**：Danis 交互式验收（窗口可操作），gold 图入库

### S8. 文档 & 收尾
- [ ] `interface/ui.h` 顶部使用示例同步最终签名
- [ ] `README` / 计划书归档到 `docs/`（如适用）
- [ ] 全量回归：现有 jpov test 全绿，无破坏

---

## 3. 文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `tools/jpov/interface/ui.h` | 已新增（本 PR） | 接口定稿 |
| `tools/jpov/interface/ui.cc` | 待建 | 实现 |
| `tools/jpov/interface/BUILD` | 修改 | 加 `ui` 库并挂入 `jpov_interface` |
| `tools/jpov/test/ui/` | 待建 | 各组件独立测试/demo main + gold 图 |
| `docs/jpov_ui_plan.md` 或归档 | 待定 | 本纲领（若做） |

---

## 4. 验收标准（总体）

1. 每个子步骤有**独立可运行的 demo/test main**，可交互式验收（鼠标操作产出可见反馈）。
2. 每个绘制型组件有一张 **gold 图**做像素级比对（符合 JPOV 自测理念）。
3. 交互型组件（按钮/滑条/输入/下拉）通过**返回值 + state 外置指针**可被程序验证。
4. 全量 jpov 现有 test 全绿，无回归。
5. 容错规则落地：字号永不缩小；无负尺寸/NaN；越界剔除生效。

---

## 5. PR 计划

- 本 PR：#（待开），仅合入**计划纲领 + ui.h 接口定稿 + BUILD 里 ui.h 占位**。
- 目的：锁定接口，让 bulletin worker 可并行按 S0→S8 推进开发与验收。
- 后续：S0~S8 由 bulletin worker（任务书模式）逐项执行，每项可交互验收。
