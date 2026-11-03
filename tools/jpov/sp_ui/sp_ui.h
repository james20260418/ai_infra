// JPOV UI — 即时模式控件（Immediate-Mode UI）
//
// 定位：面向"Agent 与设计者共同操作"的运行期调试面板/参数台。
//
// ============================= 怎么用（3 步） =============================
//   一个 SpUi 对象对应一块面板，跨帧持有。每帧：
//
//   jpov::SpUi ui;                                  // 跨帧持有（面板级）
//   ui.Begin(input, theme, 1280, 720);            // 每帧开始：注入输入/主题/视口
//   ui.SliderFloat("速度", &st.speed, SpRect{{20,0},{388,24}}, 0, 100);
//   if (ui.Button("重置", SpRect{{20,468},{120,32}})) { DoReset(); }
//   ui.End();                                     // 可选
//   RenderCommandList cmd;
//   ui.Emit(&cmd);                                // 本帧全部 2D 指令追加到 cmd
//
//   复用一组子面板 = 写一个函数 + for 循环，每帧传不同状态：
//   void DrawMotor(SpUi& ui, MotorParam& st) {
//       ui.SliderFloat("速度", &st.speed, SpRect{{92,0},{388,24}}, 0, 100);
//       ui.Checkbox("反转", &st.inverted, SpRect{{20,32},{96,24}});
//   }
//       DrawMotor(ui, console.motor_a);   // 复用 1
//       DrawMotor(ui, console.motor_b);   // 复用 2
//
// =============== 坐标系：SpRect 是【面板局部坐标】，不是屏幕坐标 ===============
//   - 所有 SpRect 的 pos 都是相对【本面板左上角 (0,0)】的偏移（像素）。
//   - 面板本身可放在窗口任意位置（面板位移由调用方负责，本接口不做）。
//   - 控件 box 只描述面板内部布局；面板整体平移【不会】改变控件 box 值。
//   - 长度均为像素。font_size 仅作文本行高/内容感知尺寸参照，不用于布局。
//
// ============ 跨帧状态会因【控件 box 值变化】而重置（重要） ============
//   即时模式不跨帧保存控件状态；只有少数显式状态需跨帧保持
//   （InputText 焦点 / Combo 展开 / 滑条拖动 / 按钮按下）。
//   这些状态的同一性判定，用控件当前的 SpRect（pos+size，精确等于）识别。
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
// ============== 跨帧状态的【所有权】（隔离规则，重要） ==============
//   上面的四个显式状态都是【独占】状态：一个面板内同一时刻只可能有一个
//   文本框聚焦 / 一个下拉展开 / 一条滑条被拖 / 一个按钮被按。所有权由
//   StateSlot 表达，写槽位只有两条合法路径：
//     1. Acquire(box)：本控件本帧成为该状态的发起者
//        （点框聚焦 / 点开下拉 / 起始拖动 / 起始按下）。
//     2. Release(box)：本控件正是当前持有者，且本帧状态结束
//        （点框外 / 回车失焦 / 收起下拉 / 松开左键）。
//   其余任何情况——尤其是"本控件本帧不持有该状态"——一律【不写槽位】。
//   这就是隔离：控件之间绝不互相踩状态，且与控件调用顺序无关
//   （谁先画不影响谁持有；取消态不会被无关控件清掉）。
//   
//   ❌ 反例（2026-09-15 修复的 bug）：若每个控件每帧都把"自己的 box +
//   自己的展开/聚焦态"无条件写回单槽，则同屏第二个控件会每帧凭空清掉
//   第一个控件的状态——表现为"第二个下拉一存在，第一个下拉一点就收"
//   （StateSlot 的 Acquire/Release 从类型上封死这种写法）。
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
//   命中测试与绘制分层一致（独占覆盖区内的全部鼠标事件）：弹出层画在最上层，
//   其覆盖区域内发生的【全部鼠标事件】（点击 / 悬停 / 按下 / 拖动起始）也只归
//   弹层拥有者，其它（被遮挡的）控件一律当作鼠标不在其上——修复"点下拉选项
//   时鼠标事件击穿到后方控件"（如同时聚焦了下方输入框 / 触发了下方按钮 /
//   拖动了下方滑条）。
//   几何来源：展开态由 combo_open_ 跨帧持有，故本帧用【上一帧】画出的
//   下拉矩形做独占判断（弹层展开期间该矩形逐帧不变）；拥有者（当前展开的
//   那个 Combo）不受此限制，照常处理其列表内的鼠标事件。
//
//   状态外置：控件状态（滑条值/勾选/选中项/文本）由调用方持有并传指针
//   写回；每个控件不跨帧记忆"值"本身（只记忆少数字面跨帧态如焦点/展开）。
//   可自测：控件产出纯 2D 指令，不直接接触渲染器，可做 gold 比对。

#ifndef JPOV_SP_UI_H_
#define JPOV_SP_UI_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "geom/common/vec.h"

#include "tools/jpov/interface/flipbook.h"
#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/pbr_material.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov::sp_ui {

// ==================== 主题 ====================

// sp_ui 主题：视觉参数唯一入口（Danis：就是这个「UI 配色的基座」）。
//
// 两层：
//   ① 颜色/间距（继承自旧 ui 的 UiTheme，字段一一对应，调用点不用改）；
//   ② 板材参数（材质 + 边框剖面）：所有控件底板都从这一处取，改一处全 UI 换皮。
//
// 布局仍是像素 box；主题不持有布局，只持有「一块板材该长什么样」。
struct SpTheme {
    float font_size;  // 字体像素高度。文本行高参照。

    // 字体别名：SpUi 发出的全部文本指令的默认字体。
    // 控件级未指定具体字体时（font_alias 为空），Emit 会用此值自动填充。
    // 空串 = 使用首个已注册字体（与 Renderer 空别名语义一致）。
    const char* font_alias = nullptr;

    // ---- 颜色 ----
    Color background;  // 面板底色
    Color foreground;  // 文本/图标默认色
    Color accent;      // 高亮（选中、活动状态、按钮主色、下拉悬停项）
    Color selected;    // "已选中/按下"的沉稳深色（下拉选中项、按钮按下态）
    // 文本选区底色（InputText 选中文字时垫在字形下的矩形）。
    // 建议带 alpha < 1（渲染层走预乘 alpha，半透明会与底下板材混合）；
    // alpha = 1 也能用（等于不透明色块，会盖住材质观感）。
    Color selection_background = {0.20f, 0.45f, 0.85f, 0.35f};
    Color hover;       // 悬停态填充（浅高亮，跟随鼠标）
    Color border;      // 边框
    Color disabled;    // 禁用态文字/图标

    // ---- 布局系数 ----
    float padding_px;        // 控件内容内边距（像素）
    float spacing_px;        // 控件间间距（像素，供手动排布参考）
    float corner_radius_px;  // 圆角半径（像素）
    float border_width_px;   // 边框宽度（像素）

    // ---- 板材：材质（所有控件底板共用；底色靠 fill_color 的 tint 区分状态）----
    // 0 = 纯色（不采样纹理），此时所有控件回到旧 ui 的观感。
    // 非 0 时必须是已注册的纹理 id；调色靠各控件的 fill_color（tint）撑。
    uint32_t plate_texture_id = 0;
    Vec2f plate_tiling = {1.0f, 1.0f};  // 材质平铺次数（面板可 >1，小控件 1 即可）
    // 材质的隐式物理尺度（屏幕像素 / 一格图集 cell）。
    //   0  = 关闭（直接用 plate_tiling 的字面值，每块板材都采样整张 cell）；
    //   >0 = 逐块板按自身尺寸反算 tiling = size / plate_material_px。
    // 打开后**整屏纹理密度一致**（大面板看到多格、小控件看到少格，纹路尺度连续），
    // 而字面 tiling 会把同一张 cell 拉充满一块大面板 → 纹理被放大到糊。
    float plate_material_px = 0.0f;
    // flipbook 帧率：0 = 静止在第 0 帧（图集仍会裁 cell，不会整张显示）。
    // 与粒子同一套约定：帧号由 time_seconds × fps 算，时间由调用方给。
    float plate_flipbook_fps = 0.0f;
    int plate_tiles_x = 1;
    int plate_tiles_y = 1;

    // ---- 板材：**边框带**自己的材质（可与填充层完全不同）----
    // 0 = 纯色边框（不采样纹理）。非 0 时必须是已注册的纹理 id。
    // 为什么要分开：Danis 2026-10-27「box 核心一个 flipbook，box 边界另一个 flipbook」——
    // 边框想用另一条更慢的金属流、或纯色边框 + 纹理填充，都只需要改这里。
    uint32_t plate_border_texture_id = 0;
    // 边框层的密度（屏幕像素 / 一格 cell）。0 = 跟随 plate_material_px。
    float plate_border_material_px = 0.0f;
    // 边框层的翻书帧率（与填充层**独立**，所以两层的动画速度可以不同）。
    float plate_border_flipbook_fps = 0.0f;
    int plate_border_tiles_x = 1;
    int plate_border_tiles_y = 1;

    // ---- 板材：边框剖面（上一轮讨论的两个边框选项，是同一份数据的两端）----
    // 0 = 平面带（只有常量底）；1 = 完整圆管剖面（假 3D 银色边）。
    float plate_border_bump = 0.0f;
    // 渐变消失带宽（像素，[0, border_width_px]）：靠近内沿 alpha 收到 0。
    // 0 = 实心边。
    float plate_border_fade = 0.0f;

    // 便利构造：默认现代扁平主题（深色底）。
    static SpTheme Default(float font_size);
};

// ==================== 布局模型 ====================

// 一块矩形区域（像素单位，SpUi 面板局部坐标）。控件必须显式给出，
// 供命中测试与绘制定位。SpRect 是 SpUi 面板的局部坐标，不是窗口绝对坐标。
struct SpRect {
    Vec2f pos;   // 左上角（相对 SpUi root 面板原点的局部坐标，像素）
    Vec2f size;  // 宽高（像素）
};

// ==================== 按钮增强皮肤 ====================
//
// 给 `SpUi::Button` 的可选增强：底板换成**一条 flipbook 材质**，normal 态持续
// 翻书（按钮「活着」的样子）；hover / pressed **不换材质、只换 tint**（同一张图集
// 上乘不同颜色）—— 所以状态切换不会跳帧、不会换图，只是同一块石头亮起来/沉下去。
//
// hover 的 tint 不是瞬切：`normal_to_hover_delay` = 从 normal 到 100% hover tint
// 所需的**时间（秒）**，反向（hover → normal）用同一个时长。中间量是一个跨帧的
// `hover_ratio`（[0,1]），每帧按 dt/delay 线性推进：
//     hover 且未按下      → ratio += dt / delay
//     未悬停 **或** 按下  → ratio -= dt / delay
// 按下时直接用 `pressed_tint`（不参与渐变），同时 ratio 在后台继续衰减；
// 松开且鼠标仍在按钮上时 ratio 从当前值继续往上爬（不会先归零再爬）。
//
// 退化为「无时钟」时（Begin 传 frame_dt_ms = 0）：无从计时 ⇒ 直接切到目标值
// （悬停 = 1，否则 = 0），即 delay 被忽略。delay = 0 也走这条立即分支。
//
// Pre-condition: texture_id == 0（= 不启用）或已注册；
//                tiles_x >= 1、tiles_y >= 1（且 tiles_x * tiles_y 不得溢出 int）、
//                fps >= 0、material_px >= 0、normal_to_hover_delay >= 0。
// 注意：hover_ratio 按**按钮 box**（与其它跨帧状态槽同一套识别规则）归属，
//       两个同 box 同屏的按钮会被当成同一个（共享同一份 ratio，且同一帧里
//       只推进一次 —— 不会因为画了两遍而加速渐变）。真正的「同位置两颗不同
//       按钮」请让 box 至少差 1px，否则它们本来就是同一个交互区域。
struct SpButtonSkin {
    // 按钮的两种外观（Danis 2026-11-03：就这两种）。
    enum class Style {
        // 旧模式：主题的圆角板材（圆角/边框/剖面都是主题的），flipbook 只当**核心填充**。
        // 适合「无缝平铺材质」（大理石、拉丝等），边框仍由主题银边提供。
        kPlate,
        // 新模式：**整块按钮就是这张 flipbook** —— 不画主题边框、不用主题填充色，
        // 图集得自己把形状/倒角/高光画好（背景全铺满，四角是图里的像素）。
        // 轮廓仍按主题的 `corner_radius_px` 做 SDF 裁剪（想要直角就把主题半径设 0）。
        // 适合「用户 DIY 的整键皮」（一块完整的按钮背景图序列）。
        kFlipbook,
    };
    Style style = Style::kPlate;

    // 底板图集纹理 id。0 = 不启用材质（纯色 fill），但 hover/pressed 的 tint
    // 渐变仍然生效（tint 就是纯色体本身）。
    uint32_t texture_id = 0;
    int tiles_x = 1;           // 图集列数
    int tiles_y = 1;           // 图集行数
    float fps = 0.0f;          // normal 态翻书帧率；0 = 静止在第 0 帧
    float material_px = 0.0f;  // 一格图集对应的屏幕像素（布局单位，随 ui_scale
                               // 一起放大）；0 = 整张图铺满一块按钮
                               // （kFlipbook 模式**应当**保持 0：整键皮不该被平铺）

    // 三种状态的 tint（乘在图集上 / 无材质时即本体色）。
    // 默认全 1 = 只翻书、状态不变色 —— 状态反馈是美术决定，本库不猜。
    Color normal_tint = {1.0f, 1.0f, 1.0f, 1.0f};
    Color hover_tint = {1.0f, 1.0f, 1.0f, 1.0f};
    Color pressed_tint = {1.0f, 1.0f, 1.0f, 1.0f};

    // normal ↔ hover 的 tint 渐变时长（秒）。0 = 立即切换。
    float normal_to_hover_delay = 0.2f;
};

// ==================== 控件 ====================

// 即时模式 UI 主入口。
// 生命周期：每帧 Begin → 若干控件 → End → Emit。
// 不跨界持有控件状态（从零构建）；SpUi 对象本身可跨帧持有。
//
// 布局：无自动布局。每个控件必须给 SpRect（像素 box），加上
// stretch_w / stretch_h 两个开关控制是否铺满 box（见文件顶注释）。
class SpUi {
public:
    // 每帧开始。input 为窗口层帧级输入；theme 为本帧主题；
    // width/height 为渲染分辨率（像素），用于越界剔除
    // （与本视口不相交的控件跳过，不画不命中）。
    // frame_dt_ms：本帧时长（毫秒），供键盘 hold 重复的 150ms 阈值计时用。
    //   传真实帧时长时：按键被持续按住累计超过 kKeyHoldRepeatDelayMs 才会
    //   开始重复，之后每累计 kKeyHoldRepeatIntervalMs 再触发一次（验收bug#11）。
    //   传 0（缺省，如无时钟的 CPU gold 测试）：hold 不产生额外重复字符
    //   （无时间信息则无法做阈值判断，仅单次 Click 立即输入，行为保守）。
    // time_seconds：板材材质翻书的时间轴（秒）。**零点由调用方自定**（帧号只用于
    //   取样，不参与交互），引擎不取自己的时钟、也不会自己递增 —— 目的是让 gold
    //   可复现：传同一个 time_seconds 就得到同一张图。
    //   静态材质（plate_flipbook_fps == 0）下本参数无意义。
    // 本帧控件会先记录，Emit 时一次性追加到 RenderCommandList。
    // 每帧只允许一次 Begin。
    void Begin(const InputSnapshot& input, const SpTheme& theme,
               float width, float height, float frame_dt_ms = 0.0f,
               float time_seconds = 0.0f);

    // 每帧结束（可选）。Emit 会隐式结束。
    void End();

    // 每帧结束。把本帧所有 2D 指令追加到 cmd（不清空已有内容）。
    void Emit(RenderCommandList* cmd /*output*/);

    // ---- 基础控件（默认铺满给定 box）----
    // 文本显示。
    void Text(const char* label, const SpRect& box,
              bool stretch_w = true, bool stretch_h = true);

    // 按钮。返回 true = 本帧被点击（一次性事件）。
    bool Button(const char* label, const SpRect& box,
                bool stretch_w = true, bool stretch_h = true);

    // 增强按钮：底板用 skin 的 flipbook（normal 态持续翻书），
    // hover / pressed 只改 tint（渐变时长 = skin.normal_to_hover_delay）。
    // 返回值语义与上面**完全一致**（本帧是否被点击）；hover 渐变状态由 SpUi
    // 跨帧持有（调用方不需要存任何东西）。
    bool Button(const char* label, const SpRect& box, const SpButtonSkin& skin,
                bool stretch_w = true, bool stretch_h = true);

    // 复选框。value 为 in/out（状态外置，本接口不跨帧记忆）；
    // 返回 true = 本帧被点击（此时 *value 已翻转）。
    // 绘制：box 内取真方形（min(宽,高) 居中）画方框；勾选态底色=accent
    // 且内画勾（2 段折线）；未勾选底色=background。标签文本在方框右侧。
    bool Checkbox(const char* label, bool* value /*inout*/, const SpRect& box,
                  bool stretch_w = true, bool stretch_h = true);

    // 水平滑条。value 为 in/out，范围 [min, max]（要求 min < max）。
    // decimal_places：数值文本显示的小数位数（0=整数，1=一位小数，…）。
    //   默认 0：显示为整数（兼容旧行为）。仅影响显示，不影响拖到的浮点值。
    bool SliderFloat(const char* label, float* value /*inout*/,
                     const SpRect& box, float min, float max,
                     int decimal_places = 0, bool stretch_w = true,
                     bool stretch_h = true);

    // 文本输入框。buffer 为 in/out（C 字符串 + 容量上限 buffer_size）。
    // buffer 必须是**合法 UTF-8**（纯 ASCII 也是合法 UTF-8）；本控件按码点编辑，
    // 不会把一个多字节字符切成半个（中文/emoji 可安全退格、粘贴、选区删除）。
    //
    // 编辑模型（2026-11-03 增加选区与剪贴板）：
    //   - 光标可在**任意字符间**定位（不是只能在末尾）：Left/Right 移动一位，
    //     Home/End 移到行首/行尾；光标为**跨帧状态**（input_caret_，码点索引）。
    //   - 字符**插入在光标处**，Backspace 删光标左侧、Delete 删光标右侧。
    //   - 左键**按下**即聚焦（不要求松开）：按下点决定光标位置；随后拖动 = 拉选区
    //     （锚点取按下点，所以低帧率下选区起点也不漂）。松开后选区保留。
    //   - 有选区时：Backspace/Delete/输入可打印字符都**先删掉选区**再执行；
    //     Left 折叠到选区头、Right 折叠到选区尾（只折叠，不再多走一格；
    //     hold 的第 2 次起才开始正常移动）；Home/End 清选区后跳转。
    //   - 在框内任意位置**再按一次左键** → 光标落到按下点、选区取消。
    //   - Ctrl+A 全选 / Ctrl+C 复制 / Ctrl+X 剪切 / Ctrl+V 粘贴。
    //     剪贴板通过 SetClipboard 注入（见下）；未注入时退化为**进程内剪贴板**，
    //     所以 Ctrl+C/V 在无窗口的 CPU 测试里也能用。粘贴会剔除/替换换行、Tab 等
    //     控制字符（单行控件），并按整码点截断到 buffer 容量（不会切出半个字）。
    //   - Ctrl/Alt/Super 按住时**不插入字母**（否则 Ctrl+C 会同时插入一个 'c'）。
    //   - Enter/Escape → 失焦（不改变 buffer），选区一并清掉。
    //   - 键盘 hold 重复（150ms 阈值）对可打印字符/Backspace/Delete/方向键均生效。
    // 返回：本帧**帧末**是否聚焦（用于感知回车/焦点丧失；字符改动经 buffer 体现）。
    bool InputText(const char* label, char* buffer /*inout*/,
                   size_t buffer_size, const SpRect& box,
                   bool stretch_w = true, bool stretch_h = true);

    // 下拉选择。selected 为 in/out，合法范围 [0, items.size()-1]。
    // （下拉列表属弹出层，Emit 后画，盖住下方其它控件。）
    bool Combo(const char* label, int* selected /*inout*/,
               const std::vector<const char*>& items, const SpRect& box,
               bool stretch_w = true, bool stretch_h = true);

    // 折叠面板（Accordion）的**标题栏**。
    //
    // 用法：折叠面板 = 标题栏（本控件）+ 展开时才绘制的若干内容行。调用方负责
    // 面板底板与「折叠时只有标题栏、展开时标题栏 + 内容」的高度/布局；本控件只画
    // 标题栏本身（点击切换展开态）。
    //
    // open 为 in/out（状态外置，同 Checkbox）：点击标题栏 → 翻转 *open。
    // 返回：本帧（可能已翻转的）*open，即「本帧是否展开」——调用方据此决定是否
    // 绘制内容行。
    // 绘制：整行填充（悬停=accent 高亮，否则=hover 深色）+ 边框 + 左侧标题文本 +
    // 右侧展开指示三角（展开 = 朝下 ▾，折叠 = 朝右 ▸，纯几何、不依赖字形）。
    // 点击属一次性事件（不跨帧记忆）；被已展开弹层独占的点击不处理（同其它控件）。
    // Pre-condition: open != nullptr。
    bool AccordionHeader(const char* label, bool* open /*inout*/,
                         const SpRect& box, bool stretch_w = true,
                         bool stretch_h = true);

    // 颜色显示块：仅展示色值（不可编辑）。
    void ColorSwatch(const char* label, const Color& color, const SpRect& box,
                     bool stretch_w = true, bool stretch_h = true);

    // ---- 装饰板材 ----
    // 用**本帧主题的材质/剖面**画一块底板，不参与命中、不参与任何状态。
    // 用于面板底 / 卡片底 / Log 底框这类声明式装饰矩形（旧版这些位置是直接
    // 调 `cmd->DrawFillRect(...)` 绕开 Ui 的）。
    //
    // 顺序：与控件同一缓冲、按调用顺序输出 ⇒ 先调本函数再调控件，背景就被盖在下面。
    // 要让装饰底也长出大理石/银边，不需要给面板传材质 —— 改主题一处即可（同源）。
    // Pre-condition: box 已由调用方保证非负尺寸（内部会 Sanitize + 越界剔除）
    // textured: false = 这块不用材质（纯色填充），用于大面积背景 ——
    //   背景不需要纹理，而且大面板上必然出现平铺重复（材质格是周期的）;
    //   静下来反而让控件上的纹路更突出，也更不抢文字。
    void Plate(const SpRect& box, Color fill, float radius, bool textured = true);

    // ---- 多纹理板材（演示/自定义控件用）----
    //
    // 核心与边界各自一套材质，**可以各自动自己的 flipbook**（不同图集/帧率/密度）。
    // 不参与命中、不写任何状态 —— 要交互请用现成控件。
    struct SpPlateSkin {
        PlateLayer fill;            // 核心层
        PlateLayer border;          // 边界层
        // 默认白色（恒等乘）。用字面量而不是 jpov::kColorWhite：后者定义在
        // src/renderer.cc，引用它会把纯 CPU 的 sp_ui 拖去链接整个渲染器。
        Color fill_tint = {1.0f, 1.0f, 1.0f, 1.0f};
        Color border_tint = {1.0f, 1.0f, 1.0f, 1.0f};
        float radius = 0.0f;
        float border_width = 0.0f;
        float border_bump = 0.0f;
        float border_fade = 0.0f;
    };

    // 按主题的「密度 + 屏幕锚定」规则造一层材质（供上面的 SpPlateSkin 用）。
    // tex: 已注册纹理 id（0 = 纯色）；material_px: 屏幕像素/格（0 = 用 tiling 字面值）；
    // fb: 图集格式（见 interface/flipbook.h）。
    // box: **布局坐标**（内部会缩放到设备像素后再反算密度）。
    PlateLayer MakeLayerFor(const SpRect& box, uint32_t tex, float material_px,
                            const FlipbookDesc& fb) const;

    // 画一块多纹理板材（不参与命中）。时间轴用本帧的 time_seconds。
    void MultiPlate(const SpRect& box, const SpPlateSkin& skin);

    // 本帧主题。供面板/装饰自己构造板材时取同一套配色与材质，
    // 避免把颜色硬编码进面板（那样换主题只换了一半）。
    //
    // ⚠️ 返回的是**已缩放**的主题：`font_size / padding_px / spacing_px /
    // corner_radius_px / border_width_px / plate_border_fade / plate_material_px(*)`
    // 都已经是**设备像素**（Begin 里乘过 ui_scale_）。
    //   * 拿颜色 → 随便用；
    //   * 拿长度喂给公开 API（`Plate(box, fill, radius)` 之类**会再乘 ui_scale_**）
    //     ⇒ 会二次缩放。要么自己除回 ui_scale_，要么别用这些字段当布局长度。
    // （`plate_material_px` 例外地仍是「屏幕像素/格」的语义：设备像素，正常用。）
    const SpTheme& theme() const { return theme_; }

    // ---- 全局 UI 缩放 ----
    // 所有控件的 box、字号、内边距、圆角、边框带宽、材质尺度 **同步** 乘这个系数：
    // 调用方仍用原来的布局坐标写面板，只是整体变大/变小（线框跟着变粗）。
    // 命中测试用的是缩放后的 box，与绘制同源 ⇒ 缩放不会错位。
    // 视口尺寸（Begin 的 width/height）也需要调用方乘同一个系数。
    // Pre-condition: scale > 0
    void SetUiScale(float scale);
    float ui_scale() const { return ui_scale_; }

    // ---- 查询 ----
    static bool Hit(const SpRect& r, float x, float y, const InputSnapshot& in);

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
    // userdata：透传给回调（SpUi 不解读）。
    // Pre-condition: fn != nullptr（未设回调时保持 nullptr）
    void SetTextMeasure(float (*fn)(const char* text, float font_size,
                                    const char* font_alias, void* userdata),
                        void* userdata = nullptr) {
        measure_text_ = fn;
        measure_userdata_ = userdata;
    }

    // ---- 剪贴板回调（可选） ----
    //
    // InputText 的 Ctrl+C/X/V 需要读写外部剪贴板。sp_ui 是纯 CPU 指令层，不链接
    // 窗口库，所以用回调注入（与 SetTextMeasure 同一套路）：
    //   - 窗口程序：接 JPOV::GetClipboardText / SetClipboardText（UTF-8）。
    //   - 无窗口的 CPU 测试：两个回调都给 nullptr 即可 —— 控件退化为
    //     **进程内剪贴板**（实例成员 local_clipboard_），Ctrl+C/V 依旧可用。
    // 两个回调可独立为空：为空的那一侧走进程内剪贴板。
    //
    // get：返回当前剪贴板文本（UTF-8，nullptr = 无内容）。指针只需在本帧内有效，
    //      SpUi 不保留它。
    // set：写入剪贴板（UTF-8，保证非空）。由调用方负责拷贝/持有。
    struct SpClipboard {
        const char* (*get)(void* userdata) = nullptr;
        void (*set)(const char* text, void* userdata) = nullptr;
        void* userdata = nullptr;
    };
    // Pre-condition: get/set 为 nullptr 或其 userdata 可用；本函数不做非空校验。
    void SetClipboard(const SpClipboard& clipboard) {
        clipboard_ = clipboard;
    }

    // ---- 容错辅助 ----
    // 规格化 box：宽/高负值 clamp 到 0；圆角半径 clamp 到 ≤ min(宽,高)/2。
    static SpRect SanitizeBox(const SpRect& r);

    // 判定 box 是否完全落在视口外（越界剔除用）。
    bool OutsideViewport(const SpRect& r) const;

private:
    // 把调用方坐标（布局空间）换成设备像素：box × ui_scale_。
    // 所有公开控件的 box 入口都只过这一个函数，保证“绘制与命中共源”。
    SpRect Scaled(const SpRect& r) const;

    // 内容感知行高：文本 + 内边距推算控件高度。
    // 首版固定近似（glyph 平均高度 + 内边距），后续接入字体度量 API。
    float RowHeight() const;

    // 控件绘制入内部指令缓冲（不是直接写 RenderCommandList）。
    void PushPlate(const SpRect& r, Color fill, Color border, float radius);
    // PushPlate 的实际实现：textured=false 时这次不采样材质（纯色填充）。
    // 走参数而不是临时改主题 —— 隐式改成员状态会让后面所有控件也跟着变。
    // fill_layer 非空时**直接用这层**填填充区（忽略 fill_textured）——
    // 给增强按钮用：它的图集/帧率/密度是逐按钮的，不来自主题。
    void PushPlateInternal(const SpRect& r, Color fill, Color border,
                           float radius, bool fill_textured,
                           bool border_textured,
                           const PlateLayer* fill_layer = nullptr,
                           bool draw_border = true);
    // 由主题字段造一层材质（fill / border 共用同一套规则）。
    // textured = false 时该层退回纯色（不采样纹理；图集/帧率仍保留，不影响其它层）。
    PlateLayer MakeLayer(const SpRect& box_device, bool is_border,
                         bool textured) const;

    // ---- 增强按钮（SpButtonSkin）----
    // 两个公开 Button 重载共用的实现；skin == nullptr = 老路径（主题三态色）。
    bool ButtonImpl(const char* label, const SpRect& box,
                    const SpButtonSkin* skin, bool stretch_w, bool stretch_h);
    // 用 skin 造填充层材质（密度规则与 MakeLayerFor 一致；box_device 已是设备像素）。
    PlateLayer MakeButtonLayer(const SpRect& box_device,
                               const SpButtonSkin& skin) const;
    // 推进某个按钮的 hover_ratio（跨帧，按 key 归属）并返回本帧的 ratio。
    // hovering = 本帧悬停且**未**按下（按下走 pressed_tint，同时 ratio 衰减）。
    float AdvanceHoverRatio(const SpRect& key, bool hovering, float delay_s);
    void PushText(const char* s, const SpRect& box);
    // 与公开的 Text() 同语义，但 box 已是**设备像素**（不再过 Scaled）。
    // 控件内部把 box 缩过一次后必须调这个：调公开的 Text() 会二次缩放
    // （表现：文字位置飞到右下，倍率 = scale²）。
    void TextInternal(const char* label, const SpRect& box_scaled,
                      bool stretch_w, bool stretch_h);
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
    // 未注入时回退到按码点估算（ASCII = 0.6em，非 ASCII = 1.0em，供无字体的
    // CPU gold 测试保持确定性）。
    // return：文本绘制后 pen 落到的水平终点（像素，相对文本左缘）。
    float MeasureTextWidth(const char* text, float font_size) const;

    // 量文本**前 count 个码点**（前缀）的绘制宽度（像素）。用于光标/选区定位：
    // 光标在第 caret 个码点边界时，其相对文本左缘的 X = 前 caret 个码点的宽度。
    // （真实字体的前缀宽 ≠ 单字宽×count，故必须按前缀实测，不能简单按字符数乘。）
    // Pre-condition: text 非空且码点数 >= count（内部按 UTF-8 算字节偏移）。
    float MeasureTextPrefixWidth(const char* text, int count,
                                 float font_size) const;

    // 给定点击的相对文本左缘 X（像素），返回光标应落的**码点索引**（0..len）：
    // 逐码点累加前缀宽度，取与 x 最接近的码点边界。用于“点哪插哪”。
    // len 是码点数（不是字节数）。
    int CaretIndexForX(const char* text, int len, float x) const;

    // 剪贴板读写：回调为空的一侧退化为进程内剪贴板（local_clipboard_）。
    const char* ClipboardText() const;
    void WriteClipboard(const char* text);

    // 布局解析（C1）：根据 stretch 开关把给定 box 解析成控件实际占用的矩形。    //   - stretch 方向非拉伸时，用 ideal（控件理想尺寸）在该方向收缩。
    //     ideal.w/h 为负表示该方向不可收缩（始终保持 box 尺寸）。
    //   - 非拉伸方向在 box 内居中（水平 + 垂直）；不提供贴左/贴右 anchor。
    // return：控件实际绘制的 SpRect（已 clamp 到 box 内）。
    static SpRect ResolveBox(const SpRect& box, float ideal_w, float ideal_h,
                             bool stretch_w, bool stretch_h);

    // 把文本压进“弹出层”缓冲（Combo 下拉等需浮在其它控件之上的内容）。
    // 弹出层在 Emit 时于全部普通指令之后追加 → 永远画在最上层。
    void PushPopupPlate(const SpRect& r, Color fill, Color border,
                           float radius);
    void PushPopupLabelText(const char* s, float left, float cy, Color color);

    // 弹层独占查询：本帧是否有【已展开的 Combo 下拉列表】覆盖点 (x,y)。
    // 弹层覆盖区域内发生的全部鼠标事件（点击/悬停/按下/拖动起始）只归拥有
    // 者 Combo，其它（被遮挡的）控件据此当作鼠标不在其上（修复"鼠标事件
    // 击穿到后方控件"）。几何取上一帧画出的展开列表矩形（见 active_popup_rect_）。
    bool PopupCoversPoint(float x, float y) const;

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
    SpTheme theme_;         // 本帧主题拷贝
    float width_ = 0;       // 视口宽（Begin 设置，越界剔除用）
    float height_ = 0;      // 视口高（Begin 设置，越界剔除用）

    // 文本宽度测量回调（可选，SetTextMeasure 注入）。
    // 非空时 InputText 光标用真实字体进宽定位；为空回退 0.6em 等宽估计。
    float (*measure_text_)(const char*, float, const char*, void*) = nullptr;
    void* measure_userdata_ = nullptr;
    // 本帧指令暂存（Begin 起收集，Emit 追加到外部 RenderCommandList）
    std::vector<Plate2DCommand> fill_rects_;
    std::vector<Text2DCommand> texts_;
    std::vector<Polyline2DCommand> polylines_;
    std::vector<Strip2DCommand> strips_;

    // 弹出层指令暂存（Combo 下拉等）。Emit 在普通指令全部追加后，
    // 再把本层追加到外部 RenderCommandList（含 order），保证画在最上层。
    // 见文件顶“弹出层”注释。
    std::vector<Plate2DCommand> popup_fill_rects_;
    std::vector<Text2DCommand> popup_texts_;

    // ---- 弹层独占（全部鼠标事件，几何跨帧）----
    // Combo 下拉浮在上层，其覆盖的点击只归拥有者。展开态跨帧稳定，故本帧
    // 用上一帧记录的展开列表矩形做判断：
    //   active_popup_rect_ = 本帧生效的独占矩形（= 上一帧的 open_popup_rect_）
    //   open_popup_rect_   = 本帧 Combo 画出的展开列表矩形（供下一帧生效）
    // 空矩形（size<=0，默认 SpRect{}）表示"无弹层"。
    SpRect active_popup_rect_{};
    SpRect open_popup_rect_{};

    // ---- 跨帧状态槽：独占状态的"唯一持有者"（隔离规则的落地点）----
    // 见文件顶《跨帧状态的所有权》。每个独占状态一个槽位，同一时刻最多
    // 一个控件持有（用 box 精确相等识别同一控件）。写槽位只有 Acquire /
    // Release 两条合法路径，其余控件只读——从类型上封死"无条件写回"
    // 导致的互相踩状态（同屏多控件场景）。
    //
    // 边界（本次未扩大范围，行为与历史一致）：槽位是"唯一持有者"，不是
    // "自动回收"——持有者不再被绘制（控件被删 / 其 box 变了）时槽位不会
    // 自己释放（只在 Acquire/Release 时变）。对焦点/下拉无实际影响（它们
    // 本就靠 Acquire 顶替）；但以 IsFree() 作起始门槛的滑条/按钮，会因此
    // 暂时开不了新的拖动/按下（面板重建即恢复）。
    struct StateSlot {
        // 本控件（box 判定）是否持有该状态。
        bool IsHeldBy(const SpRect& box) const {
            return is_held && held_box.pos == box.pos &&
                   held_box.size == box.size;
        }
        // 槽位空闲（无人持有）→ 允许本控件发起一个新状态。
        bool IsFree() const { return !is_held; }
        // 取得所有权（本帧成为发起者时调用）。会顶掉原持有者 = 状态转移
        // （如：点另一个输入框 → 焦点从旧框转给新框）。
        void Acquire(const SpRect& box) {
            is_held = true;
            held_box = box;
        }
        // 释放所有权：仅当本控件正是持有者时生效；否则【无操作】
        // （绝不踩别人的槽位——隔离的关键）。
        void Release(const SpRect& box) {
            if (IsHeldBy(box)) {
                is_held = false;
                held_box = SpRect{};
            }
        }
        // 无条件释放（跨帧兜底专用）：不看持有者是谁。调用点必须能自己保证
        // “这个槽位在本帧已经不可能有意义”（例如左键已经松开）—— 见 SpUi::Begin。
        void ReleaseAny() {
            is_held = false;
            held_box = SpRect{};
        }

    private:
        // 字段私有：只有 SpUi 的成员函数能改，外部无法绕过 Acquire/Release
        // 直接写槽位 → 不会重新引入"互相踩状态"的写法（隔离由类型保证）。
        friend class SpUi;

        bool is_held = false;  // 槽位是否有人持有
        SpRect held_box{};     // 持有者 box（is_held 为 true 时有效）
    };

    // ---- 跨帧状态（InputText 焦点 + 水平滚动）----
    // 聚焦中的文本框：跨帧记忆"哪个文本框聚焦"（焦点/光标可见属显式状态
    // 语义；光标绘制为静态不闪烁，以便单帧自证 gold 可确定性比对）。
    StateSlot text_focus_;
    // 水平滚动偏移（像素）：文本超出 box 宽时，光标跟随内部滚动，保证
    // 光标不越出 box 右缘（S5.3 内部 scroll，不溢出）。跨帧保持以免重绘闪烁。
    // 归属上面聚焦的那个文本框（面板内同时至多一个聚焦框）。
    float input_scroll_px_ = 0.0f;

    // 输入框光标位置（**码点索引**，0..文本码点数，插在 buffer 的第 caret 个
    // 码点之前）。跨帧保持；按下框内时按按下点 x 定位光标（见实现）。
    // 为什么是码点而不是字节：buffer 是 UTF-8，按字节移动/删除会把中文切成
    // 半个字符（粘贴/退格必现）。码点索引是唯一不破坏编码的编辑单位。
    // 与 text_focus_ 同属一个聚焦框。
    int input_caret_ = 0;

    // 选区锚点（码点索引，与 input_caret_ 同一所有权）。
    // 不变量：无选区时 select_anchor_ == input_caret_；
    //         有选区时选区 = [min(anchor,caret), max(anchor,caret))。
    // 拖拽选择时锚点固定、光标跟随鼠标；方向键折叠时两者一起走到边界。
    int select_anchor_ = 0;

    // ---- 跨帧状态（InputText 拖拽选择）----
    // 左键在框内按下后持有（按住期间），用按下起点定位锚点、帧末鼠标位置
    // 拖出选区。松开即释放（选区保留，供 Ctrl+C/X 使用）。
    // 与 button_press_/slider_drag_ 同一套语义：一旦开始就不校验鼠标是否还在
    // 框内（允许把选区拖出框外），只在左键松开时结束。
    StateSlot text_select_drag_;

    // 剪贴板回调（SetClipboard 注入；空 = 用进程内剪贴板）。
    SpClipboard clipboard_;
    // 进程内剪贴板：无回调时 Ctrl+C/V 的落点（也让 CPU gold 测试无需窗口）。
    std::string local_clipboard_;

    // 本帧时长（毫秒），Begin 设置；<=0 表示无时钟（不产生 hold 重复）。
    float frame_dt_ms_ = 0.0f;
    // 本帧材质翻书时间（秒），Begin 设置；零点由调用方自定（见 Begin 注释）。
    float time_seconds_ = 0.0f;
    // 全局 UI 缩放（SetUiScale 设置，默认 1）。布局坐标 → 设备像素的单一倍率。
    float ui_scale_ = 1.0f;
    // 键盘 hold 重复跨帧累积：按键被持续按住的总时长（毫秒）。
    // 每帧 AdvanceKeyHold 更新，key 释放（None/Click）时清零。
    // 下标 = KeyCode 数值，与 InputSnapshot::keys 对齐。
    // 已发出的重复次数也由 hold_emitted_repeats_ 记录，避免跨帧重发。
    // 用空括号列表初始化（C++11 默认成员初始化），保证 SpUi 对象构造时
    // 数组清零（本类无显式构造函数，跨帧持久状态不能被垃圾值污染）。
    float hold_ms_[kMaxKeyCode] = {};
    int hold_emitted_repeats_[kMaxKeyCode] = {};

    // ---- 跨帧状态（Combo 下拉展开）----
    // 展开中的 Combo（下拉列表可见，属显式状态语义需跨帧保持）。
    StateSlot combo_open_;

    // ---- 跨帧状态（仅 InputText 键盘 hold 重复需要；其余控件一律无状态）----
    // 键盘 hold 重复阈值（毫秒，验收 bug#11 两态）：
    //   累计 hold 时间 > kKeyHoldRepeatDelayMs 视为连续按下，开始重复；
    //   之后每累计 kKeyHoldRepeatIntervalMs 触发 1 次。
    static constexpr float kKeyHoldRepeatDelayMs = 150.0f;
    static constexpr float kKeyHoldRepeatIntervalMs = 150.0f;

    // ---- 跨帧状态（SliderFloat 拖动）----
    // 正在被拖动的滑条。滑条拖动属显式状态语义：左键在 box 内按下开始 drag
    // 后，只要左键仍按住就持续写值，即使鼠标飘出 box 竖直范围也不再校验
    // （符合一般 UI：drag 一旦开始，判定区不作数，直到左键释放才结束）。
    StateSlot slider_drag_;

    // ---- 跨帧状态（Button 按下态）----
    // 正处于按下态的按钮。按钮按下属显式状态语义：左键在 box 内按下
    //（Drag/Hold）开始后，只要左键仍按住就持续保持按下色，即使鼠标飘出
    // box 也不再校验（符合一般 UI，hold 一旦开始判定区不作数，直到左键
    // 释放才恢复）；起始判据仍保留（box 内才可开始）。被按的按钮不被其他
    // 按钮抢走按下态（A 飘越 B 时 B 不误抢，因起始需槽位空闲）。
    StateSlot button_press_;

    // ---- 跨帧状态（Button 增强皮肤的 hover 渐变）----
    // 每个「用过 SpButtonSkin 的按钮」一条轨道，按 box 识别（同 StateSlot 的
    // 规则：pos + size 精确相等 = 同一个按钮；**用调用方的布局坐标**，不含
    // ui_scale_ —— 运行时改缩放比不会把所有 ratio 重置）。有多条而不是单个槽位：
    // 鼠标从 A 移到 B 时，A 的 ratio 还要继续衰减到 0（只留一个槽位会让 A
    // 卡在半亮）。回收在 Begin 里做：条目由最后一次被绘制的帧号 last_frame
    // 决定，保留「last_frame ≥ 本帧 − 2」的条目 ⇒ 控件消失后还能再活 2 帧
    // （让最后一小段衰减有机会画完），第 3 帧开始清掉。
    // 同一帧内对同一 box 重复调用只会推进一次（last_frame 已是本帧就直接返回
    // 当前 ratio）—— 否则画两遍的按钮渐变速度会翻倍。
    struct HoverTrack {
        SpRect box{};         // 归属按钮（调用方给的 box，布局单位）
        float ratio = 0.0f;   // [0,1]
        int64_t last_frame = 0;
    };
    std::vector<HoverTrack> hover_tracks_;
    // 帧序号（Begin 自增，仅用于回收没再出现的 hover 轨道）。
    int64_t frame_index_ = 0;
};

// 键盘字符映射：KeyCode → 可编辑字符（供 InputText 写回 buffer，也供测试
// 合成按键时复用同一套映射，避免测试与实现分叉）。
//
// shift 决定字母大小写，以及标点/数字键的上档符号（如 Minus → '-' / '_'，
// Period → '.' / '>'，_1 → '!'）。不可编辑键（修饰键/控制键/方向键等）返回 '\0'。
char SpInputCharForKey(KeyCode key, bool shift);

}  // namespace jpov::sp_ui

#endif  // JPOV_SP_UI_H_
