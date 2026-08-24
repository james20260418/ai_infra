// JPOV UI S1 自证测试
//
// 验证 S1 基础控件 Text / ColorSwatch 的绘制与容错：
//   1. Text：原字号、垂直居中（kCenter 对齐，pos=box 中心）、box 过小不缩字号。
//   2. ColorSwatch：真方形、box 不足时按 min(宽,高) 居中。
//   3. 容错：负尺寸 box → SanitizeBox 规格化后无 NaN；越界 box → 剔除 0 指令。
//
// 本测试是纯 CPU 的指令层比对（gold 指令），不渲染、无窗口（headless）。

#include <cmath>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::Color;
using jpov::FillRect2DCommand;
using jpov::InputSnapshot;
using jpov::RenderCommandList;
using jpov::Text2DCommand;
using jpov::TextAlignment;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;
using jpov::Vec2f;

InputSnapshot MakeEmptyInput() {
    InputSnapshot in{};
    return in;
}

// 判定浮点是否有限（无 NaN / Inf）。
bool IsFinite(float v) { return std::isfinite(v); }

// Text 应产出一条 kCenter 对齐、原字号、pos=box 中心的 Text2D 指令。
void CheckTextCommand(const Text2DCommand& t, const char* text, float font_size,
                      float cx, float cy, TextAlignment align) {
    CHECK_EQ(t.text, text) << "Text 内容不符";
    CHECK_EQ(t.font_size, font_size) << "Text 应保持原字号，不得缩放";
    CHECK(static_cast<int>(t.alignment) == static_cast<int>(align))
        << "Text 对齐方式不符";
    CHECK(IsFinite(t.pos.x()) && IsFinite(t.pos.y())) << "Text pos 出现 NaN";
    CHECK_EQ(t.pos.x(), cx);
    CHECK_EQ(t.pos.y(), cy);
}

// 从 RenderCommandList 取指定 index 的 FillRect2D 并校验其尺寸/位置。
void CheckFillRect(const FillRect2DCommand& r, float px, float py, float w,
                   float h) {
    CHECK(IsFinite(r.pos.x()) && IsFinite(r.pos.y()));
    CHECK(IsFinite(r.size.x()) && IsFinite(r.size.y()));
    CHECK_EQ(r.pos.x(), px);
    CHECK_EQ(r.pos.y(), py);
    CHECK_EQ(r.size.x(), w);
    CHECK_EQ(r.size.y(), h);
}

}  // namespace

namespace jpov {

class UiS1Test {
public:
    // Text：不同 box → 各自独立的 gold 指令（原字号 + box 中心 + kCenter）。
    static void TestTextMultipleBoxes() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.Text("A", UiRect{{10.0f, 20.0f}, {120.0f, 28.0f}});
        ui.Text("Hello", UiRect{{200.0f, 50.0f}, {300.0f, 24.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.text2d.size(), 2u) << "两个 box 应产出 2 条 Text2D";
        // box1 中心 = (10+120/2, 20+28/2) = (70, 34)
        CheckTextCommand(cmd.text2d[0], "A", theme.font_size, 70.0f, 34.0f,
                         TextAlignment::kCenter);
        // box2 中心 = (200+300/2, 50+24/2) = (350, 62)
        CheckTextCommand(cmd.text2d[1], "Hello", theme.font_size, 350.0f, 62.0f,
                         TextAlignment::kCenter);
        LOG(INFO) << "[PASS] Text 多 box → 各自 gold 指令（居中、原字号）";
    }

    // Text 空标签：不产出指令。
    static void TestTextEmptyLabel() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.Text("", UiRect{{10.0f, 10.0f}, {100.0f, 30.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.text2d.size(), 0u) << "空标签不应产出 Text2D";
        LOG(INFO) << "[PASS] Text 空标签 → 0 条指令";
    }

    // ColorSwatch：正方形象限居中；非方形 box 取 min 边长居中。
    static void TestColorSwatchSquare() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();
        const Color c{1.0f, 0.0f, 0.0f, 1.0f};

        ui.Begin(in, theme, 640.0f, 360.0f);
        // box 为正方形 (40x40) → side=40，铺满 box，pos 不变。
        ui.ColorSwatch("sw1", c, UiRect{{10.0f, 10.0f}, {40.0f, 40.0f}});
        // box 非正方形 (100x40) → side=min=40，水平居中：
        //   x = pos.x + (100-40)/2 = 10+30 = 40；y 不变。
        ui.ColorSwatch("sw2", c, UiRect{{10.0f, 100.0f}, {100.0f, 40.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 2u) << "两个 swatch 应产出 2 条 FillRect2D";
        // sw1: (10,10,40,40)
        CheckFillRect(cmd.fillrect2d[0], 10.0f, 10.0f, 40.0f, 40.0f);
        // sw2: 正方形 (40,100,40,40)，水平居中
        CheckFillRect(cmd.fillrect2d[1], 40.0f, 100.0f, 40.0f, 40.0f);
        // 颜色透传
        const FillRect2DCommand& r = cmd.fillrect2d[0];
        CHECK_EQ(r.fill_color.r, c.r);
        CHECK_EQ(r.fill_color.g, c.g);
        CHECK_EQ(r.fill_color.b, c.b);
        LOG(INFO) << "[PASS] ColorSwatch 真方形居中、颜色透传";
    }

    // 容错：负尺寸 box → 规格化后无 NaN（Text + Swatch）。
    static void TestNegativeBoxNoNaN() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        // 全部负尺寸 → SanitizeBox clamp 到 0 → 零面积跳过，无指令。
        ui.Text("x", UiRect{{-5.0f, -5.0f}, {-10.0f, -20.0f}});
        ui.ColorSwatch("y", Color{1.0f, 1.0f, 1.0f, 1.0f},
                       UiRect{{-5.0f, -5.0f}, {-10.0f, -20.0f}});
        // 一维负一维正 → 规格化后零面积跳过，无 NaN、无指令。
        ui.Text("z", UiRect{{10.0f, 10.0f}, {30.0f, -6.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.text2d.size(), 0u) << "负尺寸 Text 应被规格化剔除";
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "负尺寸 Swatch 应被规格化剔除";
        // 显式验证规格化结果无 NaN。
        UiRect neg{{10.0f, 20.0f}, {-5.0f, -8.0f}};
        UiRect out = Ui::SanitizeBox(neg);
        CHECK(IsFinite(out.pos.x()) && IsFinite(out.pos.y()));
        CHECK(IsFinite(out.size.x()) && IsFinite(out.size.y()));
        LOG(INFO) << "[PASS] 负尺寸 box → 规格化后 0 指令、无 NaN";
    }

    // 容错：越界 box → 剔除 0 指令。
    static void TestOffscreenCulled() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        // 完全在视口右侧/下方外 → 剔除。
        ui.Text("offscreen", UiRect{{700.0f, 10.0f}, {100.0f, 30.0f}});
        ui.ColorSwatch("sw", Color{1.0f, 1.0f, 1.0f, 1.0f},
                       UiRect{{10.0f, 400.0f}, {40.0f, 40.0f}});
        // 部分相交（左缘露出）→ 照画（GPU 兜底），不剔除。
        ui.Text("partial", UiRect{{-30.0f, 10.0f}, {100.0f, 30.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.text2d.size(), 1u) << "越界 Text 剔除、部分相交保留";
        CHECK_EQ(cmd.text2d[0].text, "partial");
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界 Swatch 应被剔除";
        LOG(INFO) << "[PASS] 越界 box 剔除 → 0 指令，部分相交照画";
    }

    static void RunAll() {
        TestTextMultipleBoxes();
        TestTextEmptyLabel();
        TestColorSwatchSquare();
        TestNegativeBoxNoNaN();
        TestOffscreenCulled();
        LOG(INFO) << "===== UI S1 (Text/ColorSwatch/容错) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s1_test");
    jpov::UiS1Test::RunAll();
    return 0;
}
