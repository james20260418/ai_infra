// JPOV UI — 即时模式控件（Immediate-Mode UI）
//
// 定位：面向"Agent 与设计者共同操作"的运行期调试面板/参数台。
//
// ============================= 怎么用（3 步） =============================
//   一个 Ui 对象对应一块面板，跨帧持有。每帧：
//
//   jpov::Ui ui;                                  // 跨帧持有（面板级）
//   ui.Begin(input, theme, 1280, 720);            // 每帧开始：注入输入/主题/视口
//   ui.SliderFloat("速度", &st.speed, UiRect{{20,0},{388,24}}, 0, 100);
//   if (ui.Button("重置", UiRect{{20,468},{120,32}})) { DoReset(); }
//   ui.End();                                     // 可选
//   RenderCommandList cmd;
//   ui.Emit(&cmd);                                // 本帧全部 2D 指令追加到 cmd
//
//   复用一组子面板 = 写一个函数 + for 循环，每帧传不同状态：
//   void DrawMotor(Ui& ui, MotorParam& st) {
//       ui.SliderFloat("速度", &st.speed, UiRect{{92,0},{388,24}}, 0, 100);
//       ui.Checkbox("反转", &st.inverted, UiRect{{20,32},{96,24}});
//   }
//       DrawMotor(ui, console.motor_a);   // 复用 1
//       DrawMotor(ui, console.motor_b);   // 复用 2
//
// =============== 坐标系：UiRect 是【面板局部坐标】，不是屏幕坐标 ===============
//   - 所有 UiRect 的 pos 都是相对【本面板左上角 (0,0)】的偏移（像素）。
//   - 面板本身可放在窗口任意位置（面板位移由调用方负责，本接口不做）。
//   - 控件 box 只描述面板内部布局；面板整体平移【不会】改变控件 box 值。
//   - 长度均为像素。font_size 仅作文本行高/内容感知尺寸参照，不用于布局。
//
// ============ 跨帧状态会因【控件 box 值变化】而重置（重要） ============
//   即时模式不跨帧保存控件状态；只有少数显式状态需跨帧保持
//   （InputText 焦点 / Combo 展开 / 滑条拖动 / 按钮按下）。
//   这些状态的同一性判定，用控件当前的 UiRect（pos+size，精确等于）识别。
//   → 若某控件的 box 值在两帧间变化（你 resize / 重排布局），
//     该控件会被当作"新控件"，其跨帧状态被重置：
//     - InputText 立即失焦（光标消失、停止接收键入）
//     - Combo 下拉立即收起
//     - 正在拖动的滑条中断
//     - 按下的按钮弹回
//   [设计决策，非 bug] 面板整体【平移】（所有控件 box 都不变）不影响任何状态；
//   只有"控件相对面板的 box 改变"（重排/resize 单个控件）才触发重置。
//   想保状态就别在交互进行中改该控件的 box。
//
// ===================== 布局：stretch_w / stretch_h =====================
//   每个控件给一个 box（可用区域）外加两个拉伸开关：
//     - stretch_w = true  → 控件宽度铺满 box 宽；
//                      false → 用"内容理想宽度"，在 box 内水平对齐。
//     - stretch_h = true  → 控件高度铺满 box 高；
//                      false → 用"内容理想高度"，在 box 内垂直对齐。
//   非拉伸时：用理想尺寸并在 box 内居中（水平 + 垂直）。
//   当前实现不提供贴左/贴右 anchor（如需贴边，调用方在 box 里预留位置即可）。
//   "理想尺寸"由每个控件按自身内容计算：
//     - 文本/按钮/复选框/色块：内容感知（文本宽、方形边长等），会收缩。
//     - 滑条/输入框/下拉：无"内容宽度"，宽度始终取 box 宽（不收缩），
//       非拉伸仅影响高度（收缩到行高）。存宽度型控件要横向空间，属设计取舍。
//   默认两个都 true = 控件铺满给定 box，行为与无布局引擎的裸 box 一致。
//
// ===================== 容错（box 过小 / 越界时） =====================
//   1. 绝不缩小字号：字形保持原尺寸、在 box 内居中，超出部分裁切（不画/不溢出）。
//   2. 圆角/内边距/句柄直径等先 clamp 到合法范围，绝不产生负尺寸或 NaN。
//   3. 真方形控件（Checkbox/ColorSwatch）按 min(宽,高) 取正方形、box 内居中。
//   4. 越界剔除：与视口 (0,0,width,height) 不相交的控件不画、不命中。
//   5. 不做面板范围裁剪（无 scissor），越界交用户负责。
//
// ===================== 弹出层（重叠控件时谁在上） =====================
//   有"必须浮在其它控件之上"的内容（如 Combo 下拉列表）时，
//   它们会单独放在弹出层，Emit 在全部普通控件之后追加 → 永远画在最上层。
//   普通控件之间按调用顺序后画覆盖先画（画家算法）。
//
//   状态外置：控件状态（滑条值/勾选/选中项/文本）由调用方持有并传指针
//   写回；每个控件不跨帧记忆"值"本身（只记忆少数字面跨帧态如焦点/展开）。
//   可自测：控件产出纯 2D 指令，不直接接触渲染器，可做 gold 比对。

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

// S0 骨架自证测试类（本仓库 tools/jpov/test/ui/，jpov 命名空间）前向声明，
// 供 Ui 的 friend 访问私有原语验证指令缓冲。
class UiS0Test;

// ==================== 主题 ====================

// UI 主题：视觉参数唯一入口。纯色系。
// 布局用像素（box），主题只持有颜色、圆角、边框宽度。
struct UiTheme {
    float font_size;  // 字体像素高度。文本行高参照。

    // 字体别名：Ui 发出的全部文本指令的默认字体。
    // 控件级未指定具体字体时（font_alias 为空），Emit 会用此值自动填充。
    // 空串 = 使用首个已注册字体（与 Renderer 空别名语义一致）。
    const char* font_alias = nullptr;

    // ---- 颜色 ----
    Color background;  // 面板底色
    Color foreground;  // 文本/图标默认色
    Color accent;      // 高亮（选中、活动状态、按钮主色、下拉悬停项）
    Color selected;    // "已选中/按下"的沉稳深色（下拉选中项、按钮按下态）
    Color hover;       // 悬停态填充（浅高亮，跟随鼠标）
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

// 一块矩形区域（像素单位，Ui 面板局部坐标）。控件必须显式给出，
// 供命中测试与绘制定位。UiRect 是 Ui 面板的局部坐标，不是窗口绝对坐标。
struct UiRect {
    Vec2f pos;   // 左上角（相对 Ui root 面板原点的局部坐标，像素）
    Vec2f size;  // 宽高（像素）
};

// ==================== 控件 ====================

// 即时模式 UI 主入口。
// 生命周期：每帧 Begin → 若干控件 → End → Emit。
// 不跨界持有控件状态（从零构建）；Ui 对象本身可跨帧持有。
//
// 布局：无自动布局。每个控件必须给 UiRect（像素 box），加上
// stretch_w / stretch_h 两个开关控制是否铺满 box（见文件顶注释）。
class Ui {
public:
    // 每帧开始。input 为窗口层帧级输入；theme 为本帧主题；
    // width/height 为渲染分辨率（像素），用于越界剔除
    // （与本视口不相交的控件跳过，不画不命中）。
    // frame_dt_ms：本帧时长（毫秒），供键盘 hold 重复的 150ms 阈值计时用。
    //   传真实帧时长时：按键被持续按住累计超过 kKeyHoldRepeatDelayMs 才会
    //   开始重复，之后每累计 kKeyHoldRepeatIntervalMs 再触发一次（验收bug#11）。
    //   传 0（缺省，如无时钟的 CPU gold 测试）：hold 不产生额外重复字符
    //   （无时间信息则无法做阈值判断，仅单次 Click 立即输入，行为保守）。
    // 本帧控件会先记录，Emit 时一次性追加到 RenderCommandList。
    // 每帧只允许一次 Begin。
    void Begin(const InputSnapshot& input, const UiTheme& theme,
               float width, float height, float frame_dt_ms = 0.0f);

    // 每帧结束（可选）。Emit 会隐式结束。
    void End();

    // 每帧结束。把本帧所有 2D 指令追加到 cmd（不清空已有内容）。
    void Emit(RenderCommandList* cmd /*output*/);

    // ---- 基础控件（默认铺满给定 box）----
    // 文本显示。
    void Text(const char* label, const UiRect& box,
              bool stretch_w = true, bool stretch_h = true);

    // 按钮。返回 true = 本帧被点击（一次性事件）。
    bool Button(const char* label, const UiRect& box,
                bool stretch_w = true, bool stretch_h = true);

    // 复选框。value 为 in/out（状态外置，本接口不跨帧记忆）；
    // 返回 true = 本帧被点击（此时 *value 已翻转）。
    // 绘制：box 内取真方形（min(宽,高) 居中）画方框；勾选态底色=accent
    // 且内画勾（2 段折线）；未勾选底色=background。标签文本在方框右侧。
    bool Checkbox(const char* label, bool* value /*inout*/, const UiRect& box,
                  bool stretch_w = true, bool stretch_h = true);

    // 水平滑条。value 为 in/out，范围 [min, max]（要求 min < max）。
    // decimal_places：数值文本显示的小数位数（0=整数，1=一位小数，…）。
    //   默认 0：显示为整数（兼容旧行为）。仅影响显示，不影响拖到的浮点值。
    bool SliderFloat(const char* label, float* value /*inout*/,
                     const UiRect& box, float min, float max,
                     int decimal_places = 0, bool stretch_w = true,
                     bool stretch_h = true);

    // 文本输入框。buffer 为 in/out（C 字符串 + 容量上限 buffer_size）。
    bool InputText(const char* label, char* buffer /*inout*/,
                   size_t buffer_size, const UiRect& box,
                   bool stretch_w = true, bool stretch_h = true);

    // 下拉选择。selected 为 in/out，合法范围 [0, items.size()-1]。
    // （下拉列表属弹出层，Emit 后画，盖住下方其它控件。）
    bool Combo(const char* label, int* selected /*inout*/,
               const std::vector<const char*>& items, const UiRect& box,
               bool stretch_w = true, bool stretch_h = true);

    // 颜色显示块：仅展示色值（不可编辑）。
    void ColorSwatch(const char* label, const Color& color, const UiRect& box,
                     bool stretch_w = true, bool stretch_h = true);

    // ---- 查询 ----
    static bool Hit(const UiRect& r, float x, float y, const InputSnapshot& in);

    // ---- 文本宽度测量回调（可选） ----
    //
    // InputText 光标需要用与渲染层完全一致的文本宽度来定位。渲染层有真实
    // 字体度量（各字形 advance），而本 UI 类是纯 CPU 指令层、无字体句柄。
    // 故通过注入一个测量回调让调用方（通常是无窗口渲染层/演示程序）提供
    // 真实文本宽度：
    //   - 设置后：InputText 光标用回调返回的宽度（pen 水平终点）定位，
    //     混合 Latin/CJK 也能精确贴合文本末尾（视觉上不再漂到 1.5~2x）。
    //   - 未设置：回退到脚本内 0.6*font_size/字符 的等宽估计（仅用于
    //     无字体的 CPU gold 测试，保持确定性）。
    //
    // text：要测量的文本；font_size：字号（像素）；
    // font_alias：字体别名（可为空 = 首个字体）；返回宽度（像素，>
    // 文本绘制后 pen 落到的最右 X，从文本左缘算起）。
    // userdata：透传给回调（Ui 不解读）。
    // Pre-condition: fn != nullptr（未设回调时保持 nullptr）
    void SetTextMeasure(float (*fn)(const char* text, float font_size,
                                    const char* font_alias, void* userdata),
                        void* userdata = nullptr) {
        measure_text_ = fn;
        measure_userdata_ = userdata;
    }

    // ---- 容错辅助 ----
    // 规格化 box：宽/高负值 clamp 到 0；圆角半径 clamp 到 ≤ min(宽,高)/2。
    static UiRect SanitizeBox(const UiRect& r);

    // 判定 box 是否完全落在视口外（越界剔除用）。
    bool OutsideViewport(const UiRect& r) const;

    // S0 骨架自证测试（本仓库 test/ui/，jpov 命名空间）需直接验证内部
    // 指令缓冲的收集→追加行为，故向该测试类开放私有原语访问。
    friend class UiS0Test;

private:
    // 内容感知行高：文本 + 内边距推算控件高度。
    // 首版固定近似（glyph 平均高度 + 内边距），后续接入字体度量 API。
    float RowHeight() const;

    // 控件绘制入内部指令缓冲（不是直接写 RenderCommandList）。
    void PushFillRect(const UiRect& r, Color fill, Color border, float radius);
    void PushText(const char* s, const UiRect& box);
    // 2D 折线（勾选标记等），首帧折线缓冲，Emit 时追加到外部列表。
    void PushPolyline(const std::vector<Vec2f>& vertices, Color color,
                      float line_width);
    // 2D 实心三角形条带（如 Combo 实心下箭头）：4 顶点、某侧顶点重合的
    // 退化 strip 画实心三角（不依赖字体字形，gold 可确定性比对）。
    // 与 PushPolyline 一样入普通层缓冲，Emit 时追加。
    void PushStrip(const std::vector<Vec2f>& vertices, Color color);
    // 左对齐、垂直居中的文本（pos=左缘中点，kMidLeft），用于需要精确左对齐
    // 垂直居中的控件（滑条数值/输入框正文/Combo 当前项与选项行）。
    // 相比 PushText（仅 kTopLeft）能正确对齐垂直中心；跳过空串/越界/零尺寸。
    void PushLabelText(const char* s, float left, float cy, Color color);

    // 文本宽度测量：优先用调用方注入的回调（真实字体进宽，光标贴合文本末尾）；
    // 未注入时回退到 0.6*font_size/字符 的等宽估计（供无字体的 CPU gold 测试）。
    // return：文本绘制后 pen 落到的水平终点（像素，相对文本左缘）。
    float MeasureTextWidth(const char* text, float font_size) const;

    // 布局解析（C1）：根据 stretch 开关把给定 box 解析成控件实际占用的矩形。
    //   - stretch 方向非拉伸时，用 ideal（控件理想尺寸）在该方向收缩。
    //     ideal.w/h 为负表示该方向不可收缩（始终保持 box 尺寸）。
    //   - 非拉伸方向在 box 内居中（水平 + 垂直）；不提供贴左/贴右 anchor。
    // return：控件实际绘制的 UiRect（已 clamp 到 box 内）。
    static UiRect ResolveBox(const UiRect& box, float ideal_w, float ideal_h,
                             bool stretch_w, bool stretch_h);

    // 把文本压进“弹出层”缓冲（Combo 下拉等需浮在其它控件之上的内容）。
    // 弹出层在 Emit 时于全部普通指令之后追加 → 永远画在最上层。
    void PushPopupFillRect(const UiRect& r, Color fill, Color border,
                           float radius);
    void PushPopupLabelText(const char* s, float left, float cy, Color color);

    // 键盘 hold 重复的累计计时（150ms 阈值两态，验收 bug#11）。
    // 输入框消费按键时调用：更新某 key 的跨帧 hold 时长累计，
    // 返回本帧应触发的“重复”动作次数（不包含 Click 帧的首次输入）：
    //   - key 处于 Hold：hold_ms_[key] += frame_dt_ms_；
    //     累计 > kKeyHoldRepeatDelayMs 后，每满 kKeyHoldRepeatIntervalMs
    //     触发 1 次（返回本帧新增长的部分，跨帧不重复发）。
    //   - key 非 Hold（本帧 None/Click，即已释放）：清零 hold_ms_[key]，返回 0。
    // 返回值为>=0 的整数动作次数；frame_dt_ms_<=0 时（无时钟）恒返回 0。
    int AdvanceKeyHold(KeyCode key);

    const InputSnapshot* input_ = nullptr;  // 本帧输入（Begin 设置）
    UiTheme theme_;         // 本帧主题拷贝
    float width_ = 0;       // 视口宽（Begin 设置，越界剔除用）
    float height_ = 0;      // 视口高（Begin 设置，越界剔除用）

    // 文本宽度测量回调（可选，SetTextMeasure 注入）。
    // 非空时 InputText 光标用真实字体进宽定位；为空回退 0.6em 等宽估计。
    float (*measure_text_)(const char*, float, const char*, void*) = nullptr;
    void* measure_userdata_ = nullptr;
    // 本帧指令暂存（Begin 起收集，Emit 追加到外部 RenderCommandList）
    std::vector<FillRect2DCommand> fill_rects_;
    std::vector<Text2DCommand> texts_;
    std::vector<Polyline2DCommand> polylines_;
    std::vector<Strip2DCommand> strips_;

    // 弹出层指令暂存（Combo 下拉等）。Emit 在普通指令全部追加后，
    // 再把本层追加到外部 RenderCommandList（含 order），保证画在最上层。
    // 见文件顶“弹出层”注释。
    std::vector<FillRect2DCommand> popup_fill_rects_;
    std::vector<Text2DCommand> popup_texts_;

    // ---- 跨帧状态（仅 InputText 焦点/水平滚动需要；其余控件一律无状态）----
    // 焦点文本框 box 的 Ui 面板局部坐标。跨帧记忆当前哪个文本框聚焦
    // （焦点/光标可见属于显式状态语义，需跨帧保持；光标本身绘制为静态不闪烁，
    // 以便单帧自证 gold 指令可确定性比对）。用 box 位置+尺寸识别同一文本框；
    // 本帧绘制时若 box 与之相等则视为聚焦。
    bool input_focused_ = false;
    UiRect input_focus_box_{};  // 聚焦文本框的 box（input_focused_ 为 true 时有效）
    // 水平滚动偏移（像素）：文本超出 box 宽时，光标跟随内部滚动，保证
    // 光标不越出 box 右缘（S5.3 内部 scroll，不溢出）。跨帧保持以免重绘闪烁。
    float input_scroll_px_ = 0.0f;

    // 本帧时长（毫秒），Begin 设置；<=0 表示无时钟（不产生 hold 重复）。
    float frame_dt_ms_ = 0.0f;
    // 键盘 hold 重复跨帧累积：按键被持续按住的总时长（毫秒）。
    // 每帧 AdvanceKeyHold 更新，key 释放（None/Click）时清零。
    // 下标 = KeyCode 数值，与 InputSnapshot::keys 对齐。
    // 已发出的重复次数也由 hold_emitted_repeats_ 记录，避免跨帧重发。
    // 用空括号列表初始化（C++11 默认成员初始化），保证 Ui 对象构造时
    // 数组清零（本类无显式构造函数，跨帧持久状态不能被垃圾值污染）。
    float hold_ms_[kMaxKeyCode] = {};
    int hold_emitted_repeats_[kMaxKeyCode] = {};

    // ---- 跨帧状态（仅 Combo 下拉展开需要；其余控件一律无状态）----
    // 展开中的 Combo 框 box（combo_open_ 为 true 时有效）。下拉的展开/收起
    // 属于显式状态语义（需跨帧保持可见），故用与 InputText 焦点相同的模式：
    // 跨帧记忆展开中的 Combo，用 box 位置+尺寸识别同一 Combo；本帧绘制时若
    // box 与之相等则视为展开态。
    bool combo_open_ = false;
    UiRect combo_open_box_{};  // 展开中的 Combo 框 box（combo_open_ 为 true 时有效）

    // ---- 跨帧状态（仅 InputText 键盘 hold 重复需要；其余控件一律无状态）----
    // 键盘 hold 重复阈值（毫秒，验收 bug#11 两态）：
    //   累计 hold 时间 > kKeyHoldRepeatDelayMs 视为连续按下，开始重复；
    //   之后每累计 kKeyHoldRepeatIntervalMs 触发 1 次。
    static constexpr float kKeyHoldRepeatDelayMs = 150.0f;
    static constexpr float kKeyHoldRepeatIntervalMs = 150.0f;

    // ---- 跨帧状态（仅 SliderFloat 拖动需要；其余控件一律无状态）----
    // 正在被拖动的滑条框 box（slider_drag_active_ 为 true 时有效）。滑条
    // 拖动属于显式状态语义：左键在 box 内按下开始 drag 后，只要左键仍按住
    // 就持续写值，即使鼠标飘出 box 竖直范围也不再校验（符合一般 UI：drag
    // 一旦开始，判定区不作数，直到左键释放才结束）。跨帧用 box 位置+尺寸
    // 识别同一滑条（与 InputText 焦点 / Combo 展开同一模式）。
    bool slider_drag_active_ = false;
    UiRect slider_drag_box_{};  // 正在拖动的滑条框 box（active 时有效）

    // ---- 跨帧状态（仅 Button 按下态需要；其余控件一律无状态）----
    // 正处于按下态的按钮框 box（button_pressed_active_ 为 true 时有效）。
    // 按钮按下属于显式状态语义：左键在 box 内按下（Drag/Hold）开始后，只要
    // 左键仍按住就持续保持按下色，即使鼠标飘出 box 也不再校验（符合一般 UI，
    // hold 一旦开始判定区不作数，直到左键释放才恢复）；起始判据仍保留（box 内
    // 才可开始）。跨帧用 box 位置+尺寸识别同一按钮（与 InputText 焦点 / Combo
    // 展开 / SliderFloat 拖动同一模式）；被按的按钮不被其他按钮抢走按下态（A
    // 飘越 B 时 B 不误抢）。
    bool button_pressed_active_ = false;
    UiRect button_pressed_box_{};  // 按下中的按钮框 box（active 时有效）
};

// 文本输入框 S5 实现的字符来源（KeyCode → 可编辑字符）辅助，供自证测试
// 合成按键时复用同一套映射，避免测试与实现分叉。
// 返回该 key 在本帧应写入 buffer 的字符；无法映射为可编辑字符（修饰键/
// 控制键）返回 '\0'，调用方忽略。大小写：InputSnapshot 未携带 Shift 修饰，
// 一律输出小写（可编辑字符全集：a-z / 0-9 / 空格）。
char UiInputCharForKey(KeyCode key);

}  // namespace jpov

#endif  // JPOV_UI_H_
