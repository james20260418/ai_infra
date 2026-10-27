# sp_ui —— SDF 板材支撑的即时模式 UI

> 位置：`tools/jpov/sp_ui/`　命名空间：`jpov::sp_ui`　主类：`SpUi`
> 依赖：只有 `interface:render_command`（GPU 侧在 `src/plate2d`）。本库本身是纯 CPU 指令层。
>
> **和 `interface/ui`（旧版）的关系：完全隔离。** 本目录是从 `ui.cc` 复制出来的独立实现，
> 交互/布局/跨帧状态逐行相同，只把控件底板从「CPU 三角化的内外环」换成 `Plate2DCommand`
> （SDF）。改一边不会误伤另一边；`test/ui/` 那 10 个测试仍然是旧版的契约。

---

## 60 秒上手

```cpp
#include "tools/jpov/sp_ui/sp_ui.h"

class MyApp : public JPOV {
    jpov::sp_ui::SpUi ui_;                       // 跨帧持有（焦点/拖动/下拉展开在里面）
    jpov::sp_ui::SpDemoState…;                    // 你自己的状态（控件状态一律外置）

    void OneIteration(int64_t frame, const jpov::InputSnapshot& in,
                      const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = 1280.0f;    // 渲染分辨率（2D 坐标的基准）
        cmds->camera.fbo_3d_height_ = 720.0f;

        // 1) 主题：颜色 + 板材（材质 / 边框剖面）。改这一处 = 全 UI 换皮。
        jpov::sp_ui::SpTheme theme = jpov::sp_ui::SpTheme::Default(16.0f);
        theme.font_alias        = "cjk";                       // 见下方“字体”
        theme.plate_texture_id  = marble_tex_;                 // 0 = 纯色
        theme.plate_material_px = 200.0f;                      // 一格图集 = 200 屏幕像素
        theme.plate_flipbook_fps        = 6.0f;                // >0 才翻书
        theme.plate_tiles_x = theme.plate_tiles_y = 4;         // 图集 4x4
        theme.plate_border_texture_id   = marble_tex_;          // 边框层可另用一张
        theme.plate_border_flipbook_fps = 3.0f;                 // 两层速度可以不同
        theme.plate_border_bump = 1.0f;                         // 0=平面边 1=圆管剖面

        // 2) 每帧：Begin → 画控件 → End → Emit
        ui_.Begin(in, theme, 1280.0f, 720.0f, /*frame_dt_ms=*/33.3f,
                  /*time_seconds=*/elapsed_s_);                 // 时间零点由**你**定
        if (ui_.Button("重置", {{20, 40}, {120, 30}})) { DoReset(); }
        ui_.SliderFloat("速度", &speed_, {{20, 80}, {300, 26}}, 0.0f, 100.0f, 1);
        ui_.End();

        // 3) Emit 一次性把本帧所有 2D 指令追加进 cmds
        ui_.Emit(cmds);
    }
};
```

要点：
- **控件状态全部外置**（`bool*` / `float*` / `char*` / `int*`），`SpUi` 只记「谁被按住 / 谁有焦点 / 哪个下拉展开」。
- **每帧从零重建**：Begin 清空内部缓冲，Emit 输出后清空。不要跨帧持有指令。
- **`Begin` 每帧只调一次**；`End()` 可选，`Emit` 会隐式结束。

---

## 概念（只有 5 个）

| 概念 | 是什么 | 谁管 |
|---|---|---|
| `SpTheme` | 视觉参数唯一入口：颜色、字号、圆角、边框宽，以及**板材**（材质 + 剖面） | 你，每帧传给 `Begin` |
| `Plate2DCommand` | 一块板材：SDF 圆角矩形 + 填充层 + 边框层，一块 = 一次 draw call | `SpUi` 内部产出 |
| 填充层 / 边框层 | 两块**独立**材质（各自的图集、平铺、帧率），共用一条时间轴 | 主题或 `SpPlateSkin` |
| `FlipbookDesc` | 图集格式描述（网格 + 帧率）+ 时间→帧号换算，唯一权威在 `interface/flipbook.h` | 主题字段 / 你直接传 |
| `ui_scale` | 布局坐标 → 设备像素的单一倍率（字号/内边距/圆角/线宽/材质密度一起变） | `SetUiScale` |

### 板材长什么样

```
┌──────────────────────────┐  ← 边框带：border_width 像素，自带材质
│ ╭──────────────────────╮ │     剖面 bump=1 时是一根环管（上亮下暗）
│ │  填充区：fill 材质    │ │     渐变带 fade>0 时向内淡出
│ ╰──────────────────────╯ │
└──────────────────────────┘
```

- 轮廓由**片元里的解析距离场**求 ⇒ 任意尺寸/圆角都是 1px 解析抗锯齿，放大不会看到多边形面。
- 填充区与边框带是同一块片元的两个不相交区域（覆盖率相减）⇒ 交接处不会裂、不会双 alpha 变暗。
- **两层材质互相独立**：`plate_texture_id` + `plate_flipbook_fps` 管填充，
  `plate_border_texture_id` + `plate_border_flipbook_fps` 管边框。想让边框用另一条更慢的金属流，
  只改那几个字段 —— 不需要第二条渲染路径。

---

## 控件（8 个 + 2 个辅助）

```cpp
void Text(label, box, stretch_w=true, stretch_h=true);
bool Button(label, box, ...);                      // true = 本帧被点
bool Checkbox(label, bool* value, box, ...);       // true = 本帧被点（value 已翻转）
bool SliderFloat(label, float* v, box, min, max, decimal_places=0, ...);
bool InputText(label, char* buf, size_t cap, box, ...);   // 返回本帧末尾是否聚焦
bool Combo(label, int* sel, items, box, ...);      // 下拉是弹出层，永远画在最上层
bool AccordionHeader(label, bool* open, box, ...); // 返回本帧的展开态
void ColorSwatch(label, color, box, ...);          // 纯色填充 + 纹理边框
```

两个辅助：

```cpp
// 装饰板材：用主题的材质画一块底（面板底/卡片底/Log 框）。
// textured=false 关掉这一块的材质（大面积背景常这么用），边框随之也变纯色。
void Plate(box, fill, radius, textured = true);

// 多纹理板材：核心与边界各一套材质，可以各自动自己的 flipbook。不参与命中。
SpPlateSkin skin;
skin.fill   = ui.MakeLayerFor(box, tex_ripple, /*material_px=*/160, {4,4, 4.0f});
skin.border = ui.MakeLayerFor(box, tex_sine,   /*material_px=*/ 90, {4,4, 9.0f});
skin.fill_tint = {0.34f, 0.36f, 0.40f, 1.0f};
skin.border_tint = {0.86f, 0.87f, 0.90f, 1.0f};
skin.radius = 10.0f; skin.border_width = 6.0f; skin.border_bump = 1.0f;
ui.MultiPlate(box, skin);
```

---

## 我该怎么写自己的面板

1. **状态放结构体，不放 UI**。每个面板一个 `struct`，控件写回它。
2. **布局用像素 box**（`SpRect{pos, size}`），没有自动布局。要复用就写函数 + for 循环
   （见 `demo/sp_ui_demo_panel.h` 的 `DrawMotorCard`）。
3. **主题每帧传**（可以先算好存起来）。想换皮就换 theme，不要改控件。
4. **时间由你给**：`Begin(..., time_seconds)`。想暂停就把秒数停住（不是回 0）。
5. **字体必须显式注册**：`Config::fonts = {{"fonts/xxx.ttc", 0, jpov::kFontBuiltinCJK}, ...}`，
   `theme.font_alias` 指向它。JPOV 没有隐式默认字体，缺了会 crash。

### 缩放

```cpp
ui.SetUiScale(1.5f);                                  // 布局坐标 → 设备像素 ×1.5
ui.Begin(in, theme, 1280.0f * 1.5f, 720.0f * 1.5f, dt, t);   // 视口也要 ×1.5
```
box / 字号 / 内边距 / 圆角 / 边框宽 / 材质密度 / `Plate()` 的 radius 全部同步放大
（命中测试用的是缩放后的 box，与绘制同源 ⇒ 不会点歪）。

---

## 材质与 flipbook

### 材质密度：`plate_material_px`

- `0`（默认）= 关闭，改用 `plate_tiling` 的字面值（每块板都采样整张图）。
- `> 0` = **一格图集对应多少屏幕像素**，逐块按尺寸反算 `tiling = size / material_px`，
  并把 UV **锚到屏幕原点**（`scroll = −pos / material_px`）。

锚定是必须的：不锚定的话每块板材都从自己左上角重新铺，3 个同尺寸卡片会长成三张一样的石头；
锚定之后整屏是一整块连续石板。

### flipbook 格式（唯一权威：`interface/flipbook.h`）

```cpp
struct FlipbookDesc { int tiles_x = 1, tiles_y = 1; float fps = 0.0f; };
bool ResolveFlipbookFrame(const FlipbookDesc&, float time_seconds, float* out_frame);
```

必须记住的几条（详见该文件头的 8 条约定）：

1. 图集 = `tiles_x × tiles_y` 网格、**行主序、帧 0 在图片左上角**。
2. **裁 cell 与「会不会动」是两件事**：`tiles > 1×1` ⇒ 裁 cell；`fps = 0` ⇒ 静止在第 0 帧
   （不是「整张图一帧」；要单张图就写 `tiles = 1×1`）。
3. 纹理注册用 `repeat = true`、**`mipmap = false`**（UI 是 1:1 像素；平铺接缝处导数断，开 mip 会糊一条线）。
4. 时间由调用方给（秒），引擎不取自己的时钟 ⇒ 同一命令 + 同一秒数 = 同一张图（gold 可复现）。
5. 循环闭合的判据：`t = N/fps` 与 `t = 0` **逐像素相同**。
   “最后一格和第一格长得一样”是**错的**（那是重复帧，会顿一下）。

---

## 坑（都真踩过）

| 现象 | 原因 | 怎么办 |
|---|---|---|
| 文字飞出按钮 / 位置偏到 2.25 倍 | 控件内部调了公开的 `Text()`，而 box 已缩放过一次 | 控件内部一律用 `TextInternal()`（设备像素）；**加缩放后务必回归按钮标签** |
| 装饰板圆角在 1.5 倍面板上还是原来的像素值 | `Plate(box, fill, radius)` 的 radius 漏乘 `ui_scale_` | 已修，且 gold 测试钉住 |
| flipbook 每到一个「行边界」就跳一下 | 图集行序与消费者 UV 的 v 方向没对齐（照抄了粒子那份 `vrow` 翻转） | 本 pass 的 `uv.y=0` 在板材顶部 ⇒ **不要翻 row** |
| 同尺寸控件花纹一模一样 | 材质 UV 锚在各自板材左上角 | 用 `plate_material_px`（自动锚到屏幕） |
| 边框看起来是纯白、没有材质 | 边框 tint 太亮 + 剖面把值推到 1.3 ⇒ 截白后材质调制被抹平 | 边框 tint 别超过 ~0.7 |
| 链接时报 `undefined reference to jpov::kColorWhite` | 该符号定义在 `src/renderer.cc`；默认成员初始化会 odr-use | 接口层的默认色用字面量（`{1,1,1,1}`） |

---

## 测试

```bash
bazel test //tools/jpov/test/sp_ui:sp_ui_gold_test      # 纯 CPU，无窗口
bazel run   //tools/jpov:jpov_sp_ui_demo                # 交互（需 DISPLAY / WSLg）
# 伪造输入 + 多帧输出（单帧仿真）：悬停→按下→松开点「重置」，每帧一张 PNG，
# 并打印每帧 log 条数 —— 看到 log 从 0 变 1 就说明点击真的进了业务逻辑。
./output/jpov_sp_ui_demo/jpov_sp_ui_demo --script --capture output/shots/sp_ui/script
./tools/jpov/build_jpov_sp_ui_demo.sh                   # 出 Linux + Windows 产物
./output/jpov_sp_ui_demo/jpov_sp_ui_demo \
    --capture output/shots/try --times 0,0.4 --wave sine   # headless 出图
```

gold 测试钉住的机理（每条都有负向验证）：文字落在自己控件上（scale 1.0/1.5）、
缩放处处同步、两层材质独立（含色样“纯色填充 + 纹理边框”形态）、屏幕锚定 UV、
时间即数据、`ResolveFlipbookFrame` 的 wrap、退化输入。

---

## 目前没做 / 边界

- **`ButtonDelux`**（用户自带整键烘焙皮）没做。真要做得先定 `kReplace` / `kOverlay` 枚举，
  且**不许开辟第二条渲染路径** —— 必须展开成 `Plate2DCommand` 的组合。
- 形状只有矩形 / 圆角矩形 / 胶囊（单一半径），没有每角独立半径、没有任意多边形。
- 没有外发光 / 内阴影 / 描边动画；没有布局系统、没有 scissor。
- 每帧每块板做 ~16 次 `glGetUniformLocation`（没走 `ShaderManager` 缓存）；30 块板量级无感，
  真要上几百块板再抽。
- `SpPlateSkin` 的 `tiling/scroll` 是按某块 box 算好的：**同一份 skin 换 box 复用会密度不对**。
