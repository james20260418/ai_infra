// JPOV UI — 即时模式控件（Immediate-Mode UI）
//
// 定位：面向"Agent 与设计者共同操作的运行期调试面板/参数台"。
//
// 架构选择（与需求方反复讨论后最终定稿）：
// - 不做自动布局（无行列容器/无布局引擎）。每个控件必须显式给出
//   像素级 UiRect（box），布局真相内聚在一个 struct，一眼可读，
//   理解成本远低于行列嵌套/布局引擎。
// - box 内默认 NEWS 全面覆盖（控件自动填满所在 box，母控件变宽则子跟着变宽），
//   可手动覆盖某个方向的拉伸，实现常用对齐。
// - 控件状态外置：用户提供参数指针（float*、bool*...），交互结果写回。
//   不跨帧记忆状态 → 每帧从零构建，符合"无状态渲染"。
// - 一个 Ui 对象对应一个 root 面板，用户跨帧持有。
// - 可自测：控件产出纯 2D 指令（可被 gold 图比对），不直接接触渲染器。
//
// 容错原则（box 过小 / 越界时的处理，实现时遵守）：
// - 控件绘制时"缩放贴紧 box"，但绝不缩小字号（字形保持原尺寸，垂直居中，
//   超出的字被裁切，不溢出）。
// - 任何圆角/内边距/句柄直径等先 clamp 到合法范围再算，保证不产生负尺寸/NaN
//   （圆角半径 ≤ min(宽,高)/2；滑条句柄直径 ≥ 最小像素等）。
// - 真方形控件（Checkbox/色块）box 不足时按 min(宽,高) 取正方形居中。
// - 越界剔除：绘制前与视口(0,0,width,height) 做 AABB 不相交测试，
//   完全在视口外的控件不画不命中；部分相交则照画（GPU 兑底）。
// - 不做 root panel 范围裁剪（不引入 scissor），越界交用户自行负责。

//
// 坐标系统一约定（沿用 JPOV 2D）：
// - 屏幕像素坐标，原点在渲染分辨率左上角（x→右，y→下）。
//
// 使用方式（即时模式）：
//   jpov::Ui ui;                                  // 跨帧持有
//   ui.Begin(input, theme, 1280, 720);             // 视口剔除需要分辨率
//   ui.Text("名字",   UiRect{{20,20},{80,24}});
//   ui.SliderFloat("灵敏度", &sens, UiRect{{105,20},{360,24}}, 0.1f, 10.0f);
//   if (ui.Button("应用", UiRect{{20,60},{120,32}})) { DoApply(); }
//   ui.End();
//   RenderCommandList cmd;
//   ui.Emit(&cmd);

#ifndef JPOV_UI_H_
#define JPOV_UI_H_

#include <cstddef>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "geom/common/vec.h"

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/pbr_material.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {

// ==================== 主题 ====================

// UI 主题：视觉参数唯一入口。纯色系。
// 布局用像素（box），主题只持有颜色、圆角、边框宽度。
struct UiTheme {
    float font_size;  // 字体像素高度。文本行高参照。

    // ---- 颜色 ----
    Color background;  // 面板底色
    Color foreground;  // 文本/图标默认色
    Color accent;      // 高亮（选中、活动状态、按钮主色）
    Color hover;       // 悬停态填充
    Color border;      // 边框
    Color disabled;    // 禁用态文字/图标

    // ---- 布局系数 ----
    float padding_px;        // 控件内容内边距（像素）
    float spacing_px;        // 控件间间距（像素，供手动排布参考）
    float corner_radius_px;  // 圆角半径（像素）
    float border_width_px;   // 边框宽度（像素）

    // 便利构造：默认现代扁平主题（深色底）。
    static UiTheme Default(float font_size);
};

// ==================== 布局模型 ====================

// 一块矩形区域（像素坐标）。控件必须显式给出，供命中测试与绘制定位。
struct UiRect {
    Vec2f pos;   // 左上角
    Vec2f size;  // 宽高
};

// 拉伸方向位掩码（默认 NEWS = 全面覆盖，控件自动填满 box）。
// box 内控件默认"铺满"；给某个方向置 0 则只在 box 内对齐（不拉伸）。
enum Stretch : uint8_t {
    Stretch_None  = 0,
    Stretch_W     = 1 << 0,  // 贴左
    Stretch_E     = 1 << 1,  // 贴右（与 W 同置 = 水平铺满）
    Stretch_N     = 1 << 2,  // 贴上
    Stretch_S     = 1 << 3,  // 贴下（与 N 同置 = 垂直铺满）
    Stretch_NEWS  = Stretch_W | Stretch_E | Stretch_N | Stretch_S,  // 默认全面覆盖
};

// ==================== 控件 ====================

// 即时模式 UI 主入口。
// 生命周期：每帧 Begin → 若干控件 → End → Emit。
// 不跨界持有控件状态（从零构建）；Ui 对象本身可跨帧持有。
//
// 布局：无自动布局。每个控件必须给 UiRect（像素 box），默认 NEWS 铺满 box，
// 可传 Stretch 掩码覆盖某方向的拉伸（实现 box 内对齐/非铺满）。
class Ui {
public:
    // 每帧开始。input 为窗口层帧级输入；theme 为本帧主题；
    // width/height 为渲染分辨率（像素），用于越界剔除
    // （与本视口不相交的控件跳过，不画不命中）。
    // 本帧控件会先记录，Emit 时一次性追加到 RenderCommandList。
    // 每帧只允许一次 Begin。
    void Begin(const InputSnapshot& input, const UiTheme& theme,
               float width, float height);

    // 每帧结束（可选）。Emit 会隐式结束。
    void End();

    // 每帧结束。把本帧所有 2D 指令追加到 cmd（不清空已有内容）。
    void Emit(RenderCommandList* cmd /*output*/);

    // ---- 基础控件（默认 box 内 NEWS 铺满）----
    // 文本显示。
    void Text(const char* label, const UiRect& box,
              Stretch stretch = Stretch_NEWS);

    // 按钮。返回 true = 本帧被点击（一次性事件）。
    bool Button(const char* label, const UiRect& box,
                Stretch stretch = Stretch_NEWS);

    // 复选框。value 为 in/out；返回 true = 本帧被点击（值已翻转）。
    bool Checkbox(const char* label, bool* value /*inout*/, const UiRect& box,
                  Stretch stretch = Stretch_NEWS);

    // 水平滑条。value 为 in/out，范围 [min, max]（要求 min < max）。
    bool SliderFloat(const char* label, float* value /*inout*/,
                     const UiRect& box, float min, float max,
                     Stretch stretch = Stretch_NEWS);

    // 文本输入框。buffer 为 in/out（C 字符串 + 容量上限 buffer_size）。
    bool InputText(const char* label, char* buffer /*inout*/,
                   size_t buffer_size, const UiRect& box,
                   Stretch stretch = Stretch_NEWS);

    // 下拉选择。selected 为 in/out，合法范围 [0, items.size()-1]。
    bool Combo(const char* label, int* selected /*inout*/,
               const std::vector<const char*>& items, const UiRect& box,
               Stretch stretch = Stretch_NEWS);

    // 颜色显示块：仅展示色值（不可编辑）。
    void ColorSwatch(const char* label, const Color& color, const UiRect& box,
                     Stretch stretch = Stretch_NEWS);

    // ---- 查询 ----
    static bool Hit(const UiRect& r, float x, float y, const InputSnapshot& in);

    // ---- 容错辅助 ----
    // 规格化 box：宽/高负值 clamp 到 0；圆角半径 clamp 到 ≤ min(宽,高)/2。
    static UiRect SanitizeBox(const UiRect& r);

    // 判定 box 是否完全落在视口外（越界剔除用）。
    bool OutsideViewport(const UiRect& r) const;

private:
    // 内容感知行高：文本 + 内边距推算控件高度。
    // 首版固定近似（glyph 平均高度 + 内边距），后续接入字体度量 API。
    float RowHeight() const;

    // 控件绘制入内部指令缓冲（不是直接写 RenderCommandList）。
    void PushFillRect(const UiRect& r, Color fill, Color border, float radius);
    void PushText(const char* s, const UiRect& box);

    const InputSnapshot* input_ = nullptr;  // 本帧输入（Begin 设置）
    UiTheme theme_;         // 本帧主题拷贝
    float width_ = 0;       // 视口宽（Begin 设置，越界剔除用）
    float height_ = 0;      // 视口高（Begin 设置，越界剔除用）
    // 本帧指令暂存（Begin 起收集，Emit 追加到外部 RenderCommandList）
    std::vector<FillRect2DCommand> fill_rects_;
    std::vector<Text2DCommand> texts_;
};

}  // namespace jpov

#endif  // JPOV_UI_H_
