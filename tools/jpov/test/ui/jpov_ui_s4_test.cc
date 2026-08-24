// JPOV UI S4 自证测试
//
// 验证 S4 滑条 SliderFloat（核心交互）：
//   1. 绘制：轨道（横铺 box、垂直居中、厚 30% 高）+ 句柄（方形、
//      直径 clamp ≥ 8px）+ 选中行程(accent) + 数值文本。
//   2. 点击跳转：本帧左键 Click 且释放位置在 box 内 → 横坐标比例映射
//      [min,max] 写回 *value，返回 true。一次性事件。
//   3. 拖动连续写回：左键 Drag/Hold 且鼠标同时在 box 横向+竖向范围内 →
//      每帧跟随鼠标横坐标映射写回（多档位验证 *value 随鼠标位置正确变化）。
//      竖向范围外拖动不响应（danis bug#14 回归：若从未在 box 内开始过 drag）。
//   3b. 一次 drag 语义（danis bug#4 回归）：左键在 box 内按下开始 drag 后，
//      后续帧左键仍按住时即使鼠标飘出 box 竖直范围也持续映射写值，直到
//      左键释放才结束（一般 UI 行为：drag 一旦开始，判定区不再作数）。
//   4. 边界：拖/点到最左→min，最右→max；越界值夹到 [min,max]。
//   5. 容错：句柄直径 < 8px 的矮 box → 句柄不缩到 0（clamp 8px）；
//      过窄 box 轨道照画、句柄不缩 0；越界/零尺寸 → 0 指令、不写值。
//
// 本测试是纯 CPU 的指令层比对（gold 指令），不渲染、无窗口（headless）。

#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::ClickEvent;
using jpov::Color;
using jpov::FillRect2DCommand;
using jpov::InputSnapshot;
using jpov::RenderCommandList;
using jpov::Text2DCommand;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;

// 构造一个鼠标位于 (mx,my) 的输入（左键无交互）。
InputSnapshot MakeInput(float mx, float my) {
    InputSnapshot in{};
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

// 构造一个左键单击（count 次 Click），全部点击释放位置均在 (cx,cy) 的输入。
InputSnapshot MakeClickInput(float mx, float my, float cx, float cy,
                             int count = 1) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = static_cast<int8_t>(count);
    for (int i = 0; i < count; ++i) {
        in.left_clicks[i] = ClickEvent{cx, cy, 1.0f};
    }
    return in;
}

// 构造一个左键 Drag（按下且移动过）的输入，鼠标位于 (mx,my)。
InputSnapshot MakeDragInput(float mx, float my) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = -1;  // Drag
    return in;
}

// 构造一个左键 Hold（按下未移动）的输入，鼠标位于 (mx,my)。
InputSnapshot MakeHoldInput(float mx, float my) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = -2;  // Hold
    return in;
}

// 断言一条 FillRect2D 的命令值与期望一致。
void CheckFill(const FillRect2DCommand& r, float px, float py, float w, float h,
               const Color& fill) {
    CHECK_NEAR(r.pos.x(), px, 0.01f);
    CHECK_NEAR(r.pos.y(), py, 0.01f);
    CHECK_NEAR(r.size.x(), w, 0.01f);
    CHECK_NEAR(r.size.y(), h, 0.01f);
    CHECK_EQ(r.fill_color.r, fill.r);
    CHECK_EQ(r.fill_color.g, fill.g);
    CHECK_EQ(r.fill_color.b, fill.b);
    CHECK_EQ(r.fill_color.a, fill.a);
}

// 数值文本命令匹配 helper：检查是否存在包含前缀的 Text2D 命令。
bool HasTextPrefix(const RenderCommandList& cmd, const char* prefix) {
    for (const Text2DCommand& t : cmd.text2d) {
        if (t.text.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace jpov {

class UiS4Test {
public:
    // 多档位拖动映射：滑条 [0,100]，box 横向铺满 → 拖动鼠标到不同 X，
    // *value 应随时间位置正确映射，且返回 true。
    static void TestDragMapsMultipleStops() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 200px 宽 box，黄金比句柄：handle_h=20, handle_w=20/φ=12.36, half=6.18；
        // track_left=56.18, track_len=187.64。
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        const float min_ = 0.0f, max_ = 100.0f;

        // 档位 A：鼠标在轨道 25% 处 → 期望 ~25。
        {
            Ui ui;
            float value = 0.0f;
            const float x = 56.18f + 187.64f * 0.25f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            const bool changed = ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK(changed) << "拖动应返回 true（值被改变）";
            CHECK_NEAR(value, 25.0f, 2.0f);  // 25% 位置应映射到 ~25
        }
        // 档位 B：鼠标在轨道 50% 处 → 期望 ~50。
        {
            Ui ui;
            float value = 0.0f;
            const float x = 56.18f + 187.64f * 0.50f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK_NEAR(value, 50.0f, 2.0f);  // 50% 位置应映射到 ~50
        }
        // 档位 C：鼠标在轨道 75% 处 → 期望 ~75。
        {
            Ui ui;
            float value = 0.0f;
            const float x = 56.18f + 187.64f * 0.75f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK_NEAR(value, 75.0f, 2.0f);  // 75% 位置应映射到 ~75
        }
        // 档位 D：Hold（按下未移动）也应持续反映当前值（不做跳变）。
        {
            Ui ui;
            float value = 30.0f;
            const float x = 56.18f + 187.64f * 0.40f;
            ui.Begin(MakeHoldInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK_NEAR(value, 40.0f, 2.0f);  // Hold 在 40% 处应反映 ~40
        }
        LOG(INFO) << "[PASS] SliderFloat 多档位拖动映射 [0,100] 正确";
    }

    // 边界 min/max：拖到最左→min，最右→max；越界值被夹断。
    static void TestBoundaryMinMax() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        // 最左（轨道起点处）→ min。
        {
            Ui ui;
            float value = 50.0f;
            const float x = 56.18f;  // track_left
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK_NEAR(value, 0.0f, 1.0f);  // 最左应映射到 min=0
        }
        // 超过最右 → 夹到 max。
        {
            Ui ui;
            float value = 50.0f;
            const float x = 50.0f + 200.0f - 1.0f;  // 接近右缘
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK_NEAR(value, 100.0f, 1.0f);  // 右缘应映射到 max=100
        }
        // 调用方传入越界初值 → 夹到合法范围。
        {
            Ui ui;
            float value = 999.0f;
            ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK_NEAR(value, 100.0f, 1.0f);  // 越界初值应夹到 max
        }
        LOG(INFO) << "[PASS] SliderFloat 边界 min/max 夹断正确";
    }

    // 点击跳转（一次性事件）：Click 释放位置在 box 内 → 映射写回。
    static void TestClickJumps() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        {
            Ui ui;
            float value = 0.0f;
            // 点击位置在轨道 50% 处。
            const float cx = 56.18f + 187.64f * 0.50f;
            ui.Begin(MakeClickInput(cx, 110.0f, cx, 110.0f), theme,
                     640.0f, 360.0f);
            const bool changed =
                ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK(changed) << "点击应返回 true";
            CHECK_NEAR(value, 50.0f, 2.0f);  // 点击 50% 处应映射到 ~50
        }
        // 点击位置在 box 外 → 不写值、返回 false。
        {
            Ui ui;
            float value = 10.0f;
            const float cx = 500.0f, cy = 300.0f;  // 框外远处。
            ui.Begin(MakeClickInput(60.0f, 110.0f, cx, cy), theme,
                     640.0f, 360.0f);
            const bool changed =
                ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK(!changed) << "框外点击不应写值";
            CHECK_NEAR(value, 10.0f, 0.01f);  // 框外点击不应改变 *value
        }
        LOG(INFO) << "[PASS] SliderFloat 点击跳转（框内写/框外忽略）";
    }

    // 拖动竖直范围校验：鼠标 y 在 box 竖直范围外时拖动不应写值（danis bug#14）。
    // box={{50,100},{200,20}} → y 有效范围 [100,120]；x 在横向范围内但 y 远离。
    static void TestDragOutsideYNoChange() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        // 鼠标 x 在轨道 50% 处（横向命中），但 y=300 远在 box 下方 → 不应响应拖动。
        const float x = 56.18f + 187.64f * 0.50f;
        {
            Ui ui;
            float value = 0.0f;
            ui.Begin(MakeDragInput(x, 300.0f), theme, 640.0f, 360.0f);
            const bool changed =
                ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK(!changed) << "y 在 box 竖直范围外拖动不应返回 true";
            CHECK((value < 0.01f))
                << "y 在 box 竖直范围外拖动不应改变 *value, got " << value;
        }
        // 对称：鼠标 y 在 box 正上方远处（y=-100）同样不应响应。
        {
            Ui ui;
            float value = 60.0f;
            ui.Begin(MakeDragInput(x, -100.0f), theme, 640.0f, 360.0f);
            const bool changed =
                ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK(!changed) << "y 在 box 上方远处拖动不应返回 true";
            CHECK((value > 59.99f))
                << "y 在 box 上方远处拖动不应改变 *value, got " << value;
        }
        // 反证：鼠标 y 回到 box 竖直范围内（y=110）→ 应正常响应。
        {
            Ui ui;
            float value = 0.0f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            const bool changed =
                ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            CHECK(changed) << "y 在 box 竖直范围内拖动应返回 true";
            CHECK((value > 48.0f && value < 52.0f))
                << "应映射到 50%, got " << value;
        }
        LOG(INFO) << "[PASS] SliderFloat 竖直范围外拖动不响应（danis bug#14）";
    }

    // 一次 drag 语义（danis bug#4 回归）：左键在 box 内按下开始 drag 后，
    // 后续帧左键仍按住时，即使鼠标飘出 box 竖直范围也持续映射写值，直到
    // 左键释放才结束。跨帧用 box 识别同一滑条（同一个 Ui 跨帧持有）。
    static void TestDragContinuesOutsideBox() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 200px 宽 box：黄金比句柄 handle_h=20, handle_w=12.36, half=6.18；
        // track_left=56.18, track_right=243.82, track_len=187.64。有效 y 范围 [100,120]。
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        Ui ui;               // 跨帧持有同一 Ui（drag 状态在其中）。
        float value = 0.0f;
        // 轨道 25% 处鼠标 x = 56.18 + 187.64*0.25 = 103.09。
        const float x = 103.09f;

        // 帧 1：左键在 box 内（y=110）按下开始 drag → 写 25%。
        ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
        ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK_NEAR(value, 25.0f, 2.0f);

        // 帧 2：继续按住，鼠标仍飘到 box 竖直范围外（y=300，远在下方）→
        // drag 不中断，仍持续写值（映射到 50%）。
        ui.Begin(MakeDragInput(x + 187.64f * 0.25f, 300.0f), theme,
                 640.0f, 360.0f);
        const bool ch2 =
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK(ch2) << "drag 开始后鼠标飘出竖直范围仍应返回 true";
        CHECK_NEAR(value, 50.0f, 2.0f);

        // 帧 3：鼠标飘到 box 下方更远（y=500），仍按住 → 继续写（75%）。
        ui.Begin(MakeDragInput(x + 187.64f * 0.50f, 500.0f), theme,
                 640.0f, 360.0f);
        ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK_NEAR(value, 75.0f, 2.0f);

        // 帧 4：左键释放（None）→ drag 结束，不再写值。
        ui.Begin(MakeInput(x + 187.64f * 0.75f, 500.0f), theme,
                 640.0f, 360.0f);
        const bool ch4 =
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK(!ch4) << "左键释放后不应再返回 true";
        CHECK_NEAR(value, 75.0f, 2.0f);

        // 帧 5：左键释放后、未再按下，鼠标在 box 内移动也不应写值。
        ui.Begin(MakeInput(x, 110.0f), theme, 640.0f, 360.0f);
        const bool ch5 =
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK(!ch5) << "释放后未按住，鼠标在 box 内也不应写值";
        CHECK_NEAR(value, 75.0f, 2.0f);

        // 帧 6：重新在 box 内按下（新的 drag）→ 恢复响应，写 25%。
        ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
        ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
        ui.End();
        CHECK_NEAR(value, 25.0f, 2.0f);

        LOG(INFO) << "[PASS] SliderFloat 二次 drag 持续漂移写值（danis bug#4）";
    }

    // 跨滑条 drag 归属：A 正在 drag 且鼠标飘到 B 的竖直范围，B 不应抢走 drag
    // （无其它滑条持有 drag 时 B 才能新起始；A 释放后 B 可正常接管）。
    static void TestDragOwnershipNotStolen() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 两个竖直堆叠的滑条：A 在上 (y 100-120)，B 在下 (y 200-220)。
        const UiRect boxA{{50.0f, 100.0f}, {200.0f, 20.0f}};
        const UiRect boxB{{50.0f, 200.0f}, {200.0f, 20.0f}};
        Ui ui;
        float va = 0.0f, vb = 0.0f;
        const float x = 103.09f;  // 轨道 25%

        // 帧 1：在 A 内按下开始 drag。
        ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
        ui.SliderFloat("a", &va, boxA, 0.0f, 100.0f);
        ui.End();
        CHECK_NEAR(va, 25.0f, 2.0f);
        // 帧 2：仍按住，鼠标飘到 B 的竖直范围内（y=210）→ A 应继续 drag
        // （写 50%），B 不应被误判新按下而抢走 drag（vb 不被写）。
        ui.Begin(MakeDragInput(x + 187.64f * 0.25f, 210.0f), theme,
                 640.0f, 360.0f);
        const bool chA =
            ui.SliderFloat("a", &va, boxA, 0.0f, 100.0f);
        const bool chB =
            ui.SliderFloat("b", &vb, boxB, 0.0f, 100.0f);
        ui.End();
        CHECK(chA) << "A 仍在 drag，应继续写值";
        CHECK_NEAR(va, 50.0f, 2.0f);
        CHECK(!chB) << "B 不应在本帧抢走 drag（无新按下）";
        CHECK_NEAR(vb, 0.0f, 0.01f);

        // 帧 3：左键释放 → A drag 结束。
        ui.Begin(MakeInput(x, 110.0f), theme, 640.0f, 360.0f);
        ui.SliderFloat("a", &va, boxA, 0.0f, 100.0f);
        ui.SliderFloat("b", &vb, boxB, 0.0f, 100.0f);
        ui.End();
        // 帧 4：重新在 B 内按下 → B 可正常开始 drag（此时无其它滑条持有）。
        ui.Begin(MakeDragInput(x, 210.0f), theme, 640.0f, 360.0f);
        const bool chB4 =
            ui.SliderFloat("b", &vb, boxB, 0.0f, 100.0f);
        ui.End();
        CHECK(chB4) << "无其它滑条持有 drag 时 B 可新起始";
        CHECK_NEAR(vb, 25.0f, 2.0f);

        LOG(INFO) << "[PASS] SliderFloat 跨滑条 drag 归属不互抢（bug#4 衍生）";
    }

    // gold 指令：多档位（含句柄位置随 value 变化）指令比对。
    // 轨道 + 选中行程 + 句柄 3 条 FillRect；标签非空时另有数值文本。
    static void TestGoldCommands() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        // handle_h=20, handle_w=12.36, half=6.18 → track_left=56.18, track_len=187.64。
        // value=25 → 句柄中心 x = 60 + 0.25*180 = 105。
        {
            Ui ui;
            RenderCommandList cmd;
            float value = 25.0f;
            ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, 0.0f, 100.0f);
            ui.End();
            ui.Emit(&cmd);

            // 3 条 FillRect：轨道(背景)、选中行程(accent, 宽=hx-track_left)、句柄。
            CHECK_EQ(cmd.fillrect2d.size(), 3u)
                << "未交互帧应产轨道+行程+句柄 3 条 FillRect";
            // 黄金比句柄：handle_h=20, handle_w=20/φ=12.36, half=6.18；
            // track_left=50+6.18=56.18, track_right=250-6.18=243.82, track_len=187.64,
            // thickness=max(4,6)=6 → 轨道 y=100+(20-6)/2=107。
            // 轨道：x=56.18, y=107, 宽=187.64, 厚=6。
            CheckFill(cmd.fillrect2d[0], 56.18f, 107.0f, 187.64f, 6.0f,
                      theme.background);
            // 选中行程：x=56.18, 宽=hx-56.18；value=25 → hx=56.18+0.25*187.64=103.09 → 宽=46.91。
            CheckFill(cmd.fillrect2d[1], 56.18f, 107.0f, 46.91f, 6.0f,
                      theme.accent);
            // 句柄：黄金比矩形，中心(103.09,110)，高=20（参考尺寸），x 窄 → 宽=12.36。
            // x = 103.09-6.18 = 96.91, y = 100, w = 12.36, h = 20。
            // danis bug#9：句柄用深色 hover（不再用浅色 foreground，与文本/轨道难辨）。
            // danis bug#10：由方形改黄金比矩形——宽:高=1:φ≈0.618，x 窄 y 高。
            CheckFill(cmd.fillrect2d[2], 96.91f, 100.0f, 12.36f, 20.0f,
                      theme.hover);
            // 数值文本：含 "speed: " 前缀。
            CHECK(HasTextPrefix(cmd, "speed: ")) << "应产出数值文本";
        }
        LOG(INFO) << "[PASS] SliderFloat gold 指令（轨道+行程+句柄+数值）";
    }

    // 容错：矮 box（高 < 8px）句柄直径 clamp 到 8px，不缩 0。
    static void TestThinBoxHandleClamp() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 高 4px 的 box：handle_d clamp 到 8px（> box 高）。
        const UiRect box{{50.0f, 100.0f}, {200.0f, 4.0f}};
        {
            Ui ui;
            RenderCommandList cmd;
            float value = 50.0f;
            ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            ui.Emit(&cmd);

            CHECK_EQ(cmd.fillrect2d.size(), 3u) << "矮 box 也应照画 3 条 FillRect";
            // 句柄为最后一条：高 8px（不缩 0），宽为该高的黄金比（x 窄）。
            const FillRect2DCommand& handle = cmd.fillrect2d[2];
            CHECK_NEAR(handle.size.y(), 8.0f, 0.01f);  // 句柄高 clamp 到 8px
            // 句柄宽 = 高/φ ≈ 8/1.618 = 4.94（黄金比矩形，x 轴窄）。
            CHECK_NEAR(handle.size.x(), 4.94f, 0.01f);
        }
        LOG(INFO) << "[PASS] SliderFloat 矮 box 句柄 clamp ≥ 8px、不缩 0";
    }

    // 容错：过窄 box（宽 < 句柄直径）轨道照画、句柄不缩 0、不越界。
    static void TestNarrowBoxTrackStillDrawn() {
        const UiTheme theme = UiTheme::Default(16.0f);
        // 宽 5px、高 20px 的 box：handle_d=clamp(20,8,5)=5（不超 box 宽）。
        const UiRect box{{50.0f, 100.0f}, {5.0f, 20.0f}};
        {
            Ui ui;
            RenderCommandList cmd;
            float value = 50.0f;
            ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("s", &value, box, 0.0f, 100.0f);
            ui.End();
            ui.Emit(&cmd);

            // 轨道与句柄都照画（track_len=0 时轨道退化为一条竖线窄矩形，仍画）。
            CHECK_EQ(cmd.fillrect2d.size(), 3u)
                << "过窄 box 轨道应照画，不丢弃";
            // 句柄直径 ≤ box 宽，不越界。
            const FillRect2DCommand& handle = cmd.fillrect2d[2];
            CHECK_GE(handle.size.x(), 0.0f);
            CHECK_LE(handle.size.x(), 5.0f);
        }
        LOG(INFO) << "[PASS] SliderFloat 过窄 box 轨道照画、句柄不缩 0";
    }

    // 容错：越界/零尺寸 → 0 指令、不写值。
    static void TestOffscreenSlider() {
        const UiTheme theme = UiTheme::Default(16.0f);
        Ui ui;
        RenderCommandList cmd;
        float value = 50.0f;

        ui.Begin(MakeClickInput(700.0f, 400.0f, 700.0f, 400.0f), theme,
                 640.0f, 360.0f);
        // 完全在视口右侧外。
        const bool c1 =
            ui.SliderFloat("off", &value, UiRect{{700.0f, 10.0f}, {100.0f, 20.0f}},
                           0.0f, 100.0f);
        // 零尺寸。
        const bool c2 =
            ui.SliderFloat("zero", &value, UiRect{{10.0f, 10.0f}, {0.0f, 0.0f}},
                           0.0f, 100.0f);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!c1 && !c2) << "越界/零尺寸滑条不应写值";
        CHECK_NEAR(value, 50.0f, 0.01f);  // 越界/零尺寸不应改变 *value
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界/零尺寸不应画指令";
        CHECK_EQ(cmd.text2d.size(), 0u) << "越界/零尺寸不应画文本";
        LOG(INFO) << "[PASS] SliderFloat 越界/零尺寸 → 0 指令、不写值";
    }

    // 小数位（增强1）：decimal_places 控制数值文本显示精度，默认 0=整数。
    static void TestDecimalPlaces() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 20.0f}};
        {
            // decimal_places=2 → 显示 0.25（保留两位）。
            Ui ui;
            RenderCommandList cmd;
            float value = 0.25f;
            ui.Begin(MakeInput(100.0f, 100.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, 0.0f, 1.0f,
                           /*decimal_places=*/2);
            ui.End();
            ui.Emit(&cmd);
            CHECK(HasTextPrefix(cmd, "speed: 0.25"))
                << "decimal_places=2 应显示 speed: 0.25";
        }
        {
            // 默认 decimal_places=0 → 显示整数 0（% .0f 四舍五入）。
            Ui ui;
            RenderCommandList cmd;
            float value = 0.25f;
            ui.Begin(MakeInput(100.0f, 100.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, 0.0f, 1.0f);
            ui.End();
            ui.Emit(&cmd);
            CHECK(HasTextPrefix(cmd, "speed: 0"));
        }
        {
            // decimal_places=1 → 0.26 四舍五入显示 0.3（一位小数）。
            Ui ui;
            RenderCommandList cmd;
            float value = 0.26f;
            ui.Begin(MakeInput(100.0f, 100.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, 0.0f, 1.0f,
                           /*decimal_places=*/1);
            ui.End();
            ui.Emit(&cmd);
            CHECK(HasTextPrefix(cmd, "speed: 0.3"));
        }
        LOG(INFO) << "[PASS] SliderFloat decimal_places 控制小数显示";
    }

    static void RunAll() {
        TestDragMapsMultipleStops();
        TestBoundaryMinMax();
        TestClickJumps();
        TestDragOutsideYNoChange();
        TestDragContinuesOutsideBox();
        TestDragOwnershipNotStolen();
        TestGoldCommands();
        TestThinBoxHandleClamp();
        TestNarrowBoxTrackStillDrawn();
        TestOffscreenSlider();
        TestDecimalPlaces();
        LOG(INFO) << "===== UI S4 (SliderFloat) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s4_test");
    jpov::UiS4Test::RunAll();
    return 0;
}
