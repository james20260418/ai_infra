// JPOV UI S2 自证测试
//
// 验证 S2 按钮 Button + 命中 Hit：
//   1. Hit：纯点-in-box 判定。点在 box 内(含边界)、box 外、面板位移偏移、
//      零尺寸 box。
//   2. Button 绘制：底色(hover 深色主色/圆角/边框) + 文本 居中；
//      悬停态鼠标进入 → fill 变 theme_.accent（亮色高亮，danis bug#15 修正）。
//   3. 点击返回 true 且仅为一次性事件：本帧左键 Click + 释放位置在按钮内
//      → 返回 true；下一帧(无 Click) → false。
//   4. 容错：越界/零尺寸按钮 → 0 指令且不命中。
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

// 构造一个左键按住（Hold）的输入：鼠标位于 (mx,my)。
// raw=-2 = Hold（按下但未移动）。用于验证 Button 按下态。
InputSnapshot MakeHoldInput(float mx, float my) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = -2;  // Hold
    return in;
}

// 构造一个左键按住且已移动（Drag）的输入：鼠标位于 (mx,my)。
// raw=-1 = Drag（按下且移动过）。用于验证按下态在鼠标飘出 box 后仍保持。
InputSnapshot MakeDragInput(float mx, float my) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = -1;  // Drag
    return in;
}

void CheckFill(const FillRect2DCommand& r, float px, float py, float w, float h,
               const Color& fill) {
    CHECK_EQ(r.pos.x(), px);
    CHECK_EQ(r.pos.y(), py);
    CHECK_EQ(r.size.x(), w);
    CHECK_EQ(r.size.y(), h);
    CHECK_EQ(r.fill_color.r, fill.r);
    CHECK_EQ(r.fill_color.g, fill.g);
    CHECK_EQ(r.fill_color.b, fill.b);
    CHECK_EQ(r.fill_color.a, fill.a);
}

}  // namespace

namespace jpov {

class UiS2Test {
public:
    // Hit：点在 box 内(含边界) true、box 外 false、零尺寸 false。
    static void TestHitPointInBox() {
        const UiRect box{{10.0f, 20.0f}, {100.0f, 40.0f}};
        // 内部
        CHECK(Ui::Hit(box, 0.0f, 0.0f, MakeInput(60.0f, 40.0f)))
            << "box 内部应命中";
        // 左/上边界（含入）
        CHECK(Ui::Hit(box, 0.0f, 0.0f, MakeInput(10.0f, 20.0f)))
            << "左/上边界应命中";
        // 右/下边界（含入）
        CHECK(Ui::Hit(box, 0.0f, 0.0f, MakeInput(110.0f, 60.0f)))
            << "右/下边界应命中";
        // 右侧外部
        CHECK(!Ui::Hit(box, 0.0f, 0.0f, MakeInput(111.0f, 40.0f)))
            << "右侧外不应命中";
        // 上方外部
        CHECK(!Ui::Hit(box, 0.0f, 0.0f, MakeInput(60.0f, 19.0f)))
            << "上方外不应命中";
        // 零尺寸 box
        CHECK(!Ui::Hit(UiRect{{1.0f, 1.0f}, {0.0f, 0.0f}}, 0.0f, 0.0f,
                       MakeInput(1.0f, 1.0f)))
            << "零尺寸 box 不应命中";
        LOG(INFO) << "[PASS] Hit 点-in-box 判定（内部/边界/外部/零尺寸）";
    }

    // Hit：面板偏移 (x,y)。Ui root 面板在窗口 (offset_x, offset_y) 时，
    // mouse 为窗口绝对坐标，需减偏移后再判点。
    static void TestHitPanelOffset() {
        const UiRect box{{10.0f, 20.0f}, {100.0f, 40.0f}};
        // 面板原点在窗口 (200, 100)。box 局部 (10,20) → 窗口 (210,120)。
        const float ox = 200.0f, oy = 100.0f;
        CHECK(Ui::Hit(box, ox, oy, MakeInput(210.0f, 120.0f)))
            << "考虑面板偏移后内部应命中";
        CHECK(!Ui::Hit(box, ox, oy, MakeInput(209.0f, 119.0f)))
            << "考虑面板偏移后外部不应命中";
        // 不用偏移则同一窗口点落在了错误局部坐标 → 判 false。
        CHECK(!Ui::Hit(box, 0.0f, 0.0f, MakeInput(210.0f, 120.0f)))
            << "漏掉面板偏移不应命中";
        LOG(INFO) << "[PASS] Hit 面板位移偏移判点";
    }

    // Button 普通态：鼠标在外，fill=hover 深色 + 边框，文本居中，返回 false。
    static void TestButtonNormalDraw() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};

        ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
        const bool clicked = ui.Button("GO", box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!clicked) << "鼠标在外不应触发点击";
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "按钮应产出一条背景 FillRect2D";
        CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                  box.size.y(), theme.hover);
        // 文本原字号、居中（同 Text 语义，pos=box 中心）。
        CHECK_EQ(cmd.text2d.size(), 1u) << "按钮应产出标签 Text2D";
        CHECK_EQ(cmd.text2d[0].text, "GO");
        CHECK_EQ(cmd.text2d[0].pos.x(),
                 box.pos.x() + box.size.x() * 0.5f);
        CHECK_EQ(cmd.text2d[0].pos.y(),
                 box.pos.y() + box.size.y() * 0.5f);
        LOG(INFO) << "[PASS] Button 普通态：hover 深色底色 + 居中文本 + 不点击";
    }

    // Button 悬停态：鼠标进入 box → fill 变 theme_.accent（亮色高亮）。
    static void TestButtonHover() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
        // 鼠标在按钮中心。
        const float mx = box.pos.x() + box.size.x() * 0.5f;
        const float my = box.pos.y() + box.size.y() * 0.5f;

        ui.Begin(MakeInput(mx, my), theme, 640.0f, 360.0f);
        const bool clicked = ui.Button("GO", box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!clicked) << "仅悬停(无 Click)不应触发点击";
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "悬停态应产出背景 FillRect2D";
        CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                  box.size.y(), theme.accent);
        LOG(INFO) << "[PASS] Button 悬停态：fill=accent 亮色变色";
    }

    // 点击：本帧左键 Click + 释放位置在按钮内 → 返回 true（一次性）。
    static void TestButtonClickOnce() {
        // 帧 1：按钮中心释放点击 → true。
        {
            Ui ui;
            RenderCommandList cmd;
            const UiTheme theme = UiTheme::Default(16.0f);
            const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
            const float cx = box.pos.x() + box.size.x() * 0.5f;
            const float cy = box.pos.y() + box.size.y() * 0.5f;

            ui.Begin(MakeClickInput(cx, cy, cx, cy), theme, 640.0f, 360.0f);
            const bool clicked = ui.Button("GO", box);
            ui.End();

            CHECK(clicked) << "按钮内点击应返回 true";
        }
        // 帧 2：同一按钮、无 Click（None）→ false。证明"仅当帧"一次性。
        {
            Ui ui;
            RenderCommandList cmd;
            const UiTheme theme = UiTheme::Default(16.0f);
            const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
            const float cx = box.pos.x() + box.size.x() * 0.5f;
            const float cy = box.pos.y() + box.size.y() * 0.5f;

            // 鼠标仍在按钮内，但本帧无左键交互。
            ui.Begin(MakeInput(cx, cy), theme, 640.0f, 360.0f);
            const bool clicked = ui.Button("GO", box);
            ui.End();

            CHECK(!clicked) << "仅悬停(无 Click)不触发点击，确保一次性事件";
        }
        LOG(INFO) << "[PASS] Button 点击返回 true 且仅当帧（一次性）";
    }

    // 点击但释放位置在按钮外 → 不触发。
    static void TestClickOutsideNotFired() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
        // 鼠标此刻在按钮内（会悬停变色），但 Click 释放位置在按钮外远处。
        const float cx = 500.0f, cy = 300.0f;

        ui.Begin(MakeClickInput(60.0f, 48.0f, cx, cy), theme, 640.0f, 360.0f);
        const bool clicked = ui.Button("GO", box);
        ui.End();

        CHECK(!clicked) << "Click 释放位置在按钮外不应触发 Button";
        LOG(INFO) << "[PASS] Button 释放位置在按钮外不触发";
    }

    // 容错：越界/零尺寸按钮 → 0 指令且不命中。
    static void TestOffscreenButton() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);

        ui.Begin(MakeClickInput(700.0f, 400.0f, 700.0f, 400.0f), theme,
                 640.0f, 360.0f);
        // 完全在视口右侧外。
        const bool c1 = ui.Button("off", UiRect{{700.0f, 10.0f}, {100.0f, 30.0f}});
        // 零尺寸。
        const bool c2 = ui.Button("zero", UiRect{{10.0f, 10.0f}, {0.0f, 0.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK(!c1 && !c2) << "越界/零尺寸按钮不应命中";
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界/零尺寸按钮不应画底色";
        CHECK_EQ(cmd.text2d.size(), 0u) << "越界/零尺寸按钮不应画文本";
        LOG(INFO) << "[PASS] Button 越界/零尺寸 → 0 指令、不命中";
    }

    // 按下态：左键在按钮内按住（Hold）→ fill 变 theme_.pressed（Bug#5）。
    // gold 校验：按下帧产出 pressed 填充色；返回 false（仅按住不触发 Click）。
    static void TestButtonPressed() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
        // 鼠标在按钮中心且左键按住（Hold）。
        const float mx = box.pos.x() + box.size.x() * 0.5f;
        const float my = box.pos.y() + box.size.y() * 0.5f;

        ui.Begin(MakeHoldInput(mx, my), theme, 640.0f, 360.0f);
        const bool clicked = ui.Button("GO", box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!clicked) << "按住(无 Click)不应触发点击";
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "按下态应产出背景 FillRect2D";
        CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                  box.size.y(), theme.pressed);
        LOG(INFO) << "[PASS] Button 按下态：hold → fill=pressed 变色";
    }

    // 按下态持续：左键在按钮内按下开始后，鼠标飘出按钮（Drag）仍保持按下色，
    // 直到左键释放才恢复（与 SliderFloat 一次 drag 语义一致，Bug#5）。
    // 跨帧状态在同一个 Ui 实例内保持（同 S4 跨帧测试约定）。
    static void TestPressedHoldsWhenMouseLeaves() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
        const float mx = box.pos.x() + box.size.x() * 0.5f;
        const float my = box.pos.y() + box.size.y() * 0.5f;
        Ui ui;  // 跨帧持有同一 Ui（按下状态在其中）。

        // 帧 1：按钮内按下（Hold）→ pressed。
        {
            RenderCommandList cmd;
            ui.Begin(MakeHoldInput(mx, my), theme, 640.0f, 360.0f);
            ui.Button("GO", box);
            ui.End();
            ui.Emit(&cmd);
            CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                      box.size.y(), theme.pressed);
        }
        // 帧 2：鼠标飘出按钮（Drag，左键仍按住）→ 仍 pressed。
        {
            RenderCommandList cmd;
            ui.Begin(MakeDragInput(500.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.Button("GO", box);
            ui.End();
            ui.Emit(&cmd);
            CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                      box.size.y(), theme.pressed);
        }
        LOG(INFO) << "[PASS] Button 按下态：鼠标飘出 box 仍保持，直到释放";
    }

    // 按下态释放：左键松开后恢复默认/hover 色（不再 pressed）。
    // 帧 1 按下（Hold）→ 帧 2 左键松开（None，鼠标仍悬停）→ hover 亮色。
    static void TestPressedReleasesOnMouseUp() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {140.0f, 36.0f}};
        const float mx = box.pos.x() + box.size.x() * 0.5f;
        const float my = box.pos.y() + box.size.y() * 0.5f;
        Ui ui;  // 跨帧持有同一 Ui。

        // 帧 1：按下（Hold）→ pressed。
        {
            RenderCommandList cmd;
            ui.Begin(MakeHoldInput(mx, my), theme, 640.0f, 360.0f);
            ui.Button("GO", box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(cmd.fillrect2d[0].fill_color.r, theme.pressed.r)
                << "帧1 未按下？";
        }
        // 帧 2：左键已松开（None），鼠标仍悬停在按钮内 → 恢复 hover 亮色。
        {
            RenderCommandList cmd;
            ui.Begin(MakeInput(mx, my), theme, 640.0f, 360.0f);
            ui.Button("GO", box);
            ui.End();
            ui.Emit(&cmd);
            CheckFill(cmd.fillrect2d[0], box.pos.x(), box.pos.y(), box.size.x(),
                      box.size.y(), theme.accent);
        }
        LOG(INFO) << "[PASS] Button 按下态：松开后恢复 hover 色";
    }

    // 按下态所有权：A 按钮被按住时鼠标飘越另一个按钮 B，B 不误抢按下态
    // （被按的 A 保持 pressed，B 保持 hover/default；与 Slider drag 不互抢一致）。
    static void TestPressedOwnershipNotStolen() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect boxA{{20.0f, 30.0f}, {140.0f, 36.0f}};
        const UiRect boxB{{200.0f, 30.0f}, {140.0f, 36.0f}};
        Ui ui;  // 跨帧持有同一 Ui。

        // 帧 1：A 内按下（Hold）→ A pressed、B 默认深色。
        {
            RenderCommandList cmd;
            ui.Begin(MakeHoldInput(60.0f, 48.0f), theme, 640.0f, 360.0f);
            ui.Button("A", boxA);
            ui.Button("B", boxB);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(cmd.fillrect2d[0].fill_color.r, theme.pressed.r)
                << "帧1 A 应按下";
            CHECK_EQ(cmd.fillrect2d[1].fill_color.r, theme.hover.r)
                << "帧1 B 未被按，应为默认深色";
        }
        // 帧 2：鼠标飘到 B 上方（Drag，左键仍按住）→ A 仍 pressed，B 不抢。
        {
            RenderCommandList cmd;
            ui.Begin(MakeDragInput(250.0f, 48.0f), theme, 640.0f, 360.0f);
            ui.Button("A", boxA);
            ui.Button("B", boxB);
            ui.End();
            ui.Emit(&cmd);
            // 指令顺序与控件调用顺序一致：A 在前、B 在后。
            CheckFill(cmd.fillrect2d[0], boxA.pos.x(), boxA.pos.y(),
                      boxA.size.x(), boxA.size.y(), theme.pressed);
            CheckFill(cmd.fillrect2d[1], boxB.pos.x(), boxB.pos.y(),
                      boxB.size.x(), boxB.size.y(), theme.accent);
        }
        LOG(INFO) << "[PASS] Button 按下态所有权：被按 A 不被 B 抢";
    }

    static void RunAll() {
        TestHitPointInBox();
        TestHitPanelOffset();
        TestButtonNormalDraw();
        TestButtonHover();
        TestButtonClickOnce();
        TestClickOutsideNotFired();
        TestOffscreenButton();
        TestButtonPressed();
        TestPressedHoldsWhenMouseLeaves();
        TestPressedReleasesOnMouseUp();
        TestPressedOwnershipNotStolen();
        LOG(INFO) << "===== UI S2 (Button+Hit) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s2_test");
    jpov::UiS2Test::RunAll();
    return 0;
}
