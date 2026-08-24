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
// 坐标系统一约定（沿用 JPOV 2D）：
// - 屏幕像素坐标，原点在渲染分辨率左上角（x→右，y→下）。
// - 所有 UiRect 均为【Ui 面板的局部坐标】。Ui 的 root 面板左上角即原点 (0,0)；
//   控件 box 坐标是相对 root 面板的偏移，不是窗口绝对坐标。
//   若面板不在窗口 (0,0)，需自行做一次位移（本接口不负责面板定位）。
// - 长度均以【像素】为单位（UiRect.size 的 x/y、UiTheme 的 padding/corner 等）。
//   em（字号倍数）不用于布局；font_size 仅作为文本行高与内容感知尺寸的参照。
//
// 容错原则（box 过小 / 越界时的处理，实现时遵守）：
// 1. 控件绘制时"缩放贴紧 box"，但绝不缩小字号——字形保持原尺寸，
//    在 box 内垂直居中，超出 box 的字被裁切（不画、不溢出）。
// 2. 任何圆角/内边距/句柄直径等，先 clamp 到【合法范围】再参与计算，
//    保证绝不产生负尺寸或 NaN：
//    - 圆角半径 clamp 到 ≤ min(box.宽, box.高)/2；
//    - 滑条句柄直径 clamp 到 ≥ 最小像素（≥8px）；
//    - box 宽/高为负 → SanitizeBox 先 clamp 到 0。
// 3. 真方形控件（Checkbox/ColorSwatch）box 不足时，按 min(宽,高) 取正方形，
//    在 box 内居中。
// 4. 越界剔除：绘制前与视口 (0,0,width,height) 做 AABB 不相交测试，
//    完全在视口外的控件【不画、不命中】；部分相交则照画（GPU 兜底）。
// 5. 不做 root 面板范围裁剪（不引入 scissor），越界交用户自行负责。
//
// sizing 语义：控件尺寸 = box 内按 Stretch 位掩码布局。
// - 默认 Stretch_NEWS → 控件填满整个 box（母控件变宽则子跟着变宽）。
// - 去掉某方向位 → 控件在该方向上用内容感知的理想尺寸、在 box 内对齐：
//   - 水平（W 或 E）：只留 W=贴左、只留 E=贴右、都不留=居中。
//   - 垂直（N 或 S）：只留 N=贴上、只留 S=贴下、都不留=居中。
// - 文本的"内容理想尺寸" = 度量后的文字宽高；其他控件见各自实现。
//
// 使用方式（即时模式 + 函数包装实现复用）：
//   // —— 子面板 = 函数 + for 循环 + 每帧传不同 state（复用）——
//   void DrawMotor(Ui& ui, MotorParam& st) {
//       ui.Text("速度", UiRect{{20,  0},{64,24}});
//       ui.SliderFloat("", &st.speed, UiRect{{92,0},{388,24}}, 0, 100);
//       ui.Checkbox("反转", &st.inverted, UiRect{{20,32},{96,24}});
//   }
//   // —— 帧循环：一个 Ui 严格对应一个 root 面板，跨帧持有 ——
//   jpov::Ui ui;                                // 跨帧持有
//   ui.Begin(input, theme, 1280, 720);          // 视口剔除需要分辨率
//   DrawMotor(ui, console.motor_a);             // 复用点 1
//   DrawMotor(ui, console.motor_b);             // 复用点 2
//   if (ui.Button("重置", UiRect{{20,468},{120,32}})) { DoReset(); }
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

// S0 骨架自证测试类（本仓库 tools/jpov/test/ui/，jpov 命名空间）前向声明，
// 供 Ui 的 friend 访问私有原语验证指令缓冲。
class UiS0Test;

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
    Color pressed;     // 按下态填充（左键按住不放时，比 hover 更深的反馈色）
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

    // 复选框。value 为 in/out（状态外置，本接口不跨帧记忆）；
    // 返回 true = 本帧被点击（此时 *value 已翻转）。
    // 绘制：box 内取真方形（min(宽,高) 居中）画方框；勾选态底色=accent
    // 且内画勾（2 段折线）；未勾选底色=background。标签文本在 box 中心。
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
    // 左对齐、垂直居中的文本（pos=左缘中点，kMidLeft），用于需要精确左对齐
    // 垂直居中的控件（滑条数值/输入框正文/Combo 当前项与选项行）。
    // 相比 PushText（仅 kTopLeft）能正确对齐垂直中心；跳过空串/越界/零尺寸。
    void PushLabelText(const char* s, float left, float cy, Color color);

    const InputSnapshot* input_ = nullptr;  // 本帧输入（Begin 设置）
    UiTheme theme_;         // 本帧主题拷贝
    float width_ = 0;       // 视口宽（Begin 设置，越界剔除用）
    float height_ = 0;      // 视口高（Begin 设置，越界剔除用）
    // 本帧指令暂存（Begin 起收集，Emit 追加到外部 RenderCommandList）
    std::vector<FillRect2DCommand> fill_rects_;
    std::vector<Text2DCommand> texts_;
    std::vector<Polyline2DCommand> polylines_;

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

    // ---- 跨帧状态（仅 Combo 下拉展开需要；其余控件一律无状态）----
    // 展开中的 Combo 框 box（combo_open_ 为 true 时有效）。下拉的展开/收起
    // 属于显式状态语义（需跨帧保持可见），故用与 InputText 焦点相同的模式：
    // 跨帧记忆展开中的 Combo，用 box 位置+尺寸识别同一 Combo；本帧绘制时若
    // box 与之相等则视为展开态。
    bool combo_open_ = false;
    UiRect combo_open_box_{};  // 展开中的 Combo 框 box（combo_open_ 为 true 时有效）

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
