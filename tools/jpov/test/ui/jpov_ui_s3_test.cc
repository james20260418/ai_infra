// JPOV UI S3 自证测试
//
// 验证 S3 复选框 Checkbox（状态外置）：
//   1. 未勾选态绘制：box 内真方形（min(宽,高) 居中）、fill=background、
//      无勾选折线；标签文本在 box 中心。
//   2. 勾选态绘制：fill=accent + 内画勾（2 段折线，3 顶点）+ 文本。
//   3. 点击翻转：本帧左键 Click + 释放位置在 box 内 → *value 翻转且返回 true；
//      同一 *value 再点击 → 再翻转（true↔false）。仅当帧一次性事件。
//   4. 点击在 box 外 → 不翻转、返回 false。
//   5. 容错：越界/零尺寸 → 0 指令、不命中、value 不变。
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
using jpov::Polyline2DCommand;
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

class UiS3Test {
public:
    // 未勾选态：value=false → 方框底=background、无折线勾、文本在 box 中心。
    static void TestUncheckedDraw() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        // 非正方形 box（120x36）：真方形取 min=36，居中 → x 从 42 到 78。
        const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
        bool value = false;

        ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
        const bool clicked = ui.Checkbox("on", &value, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!clicked) << "鼠标在外不触发点击";
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "未勾选应产出一条方框 FillRect2D";
        // 真方形：side=min(120,36)=36，x=20+(120-36)/2=62，y=30。
        const float side = 36.0f;
        CheckFill(cmd.fillrect2d[0], 62.0f, 30.0f, side, side, theme.background);
        CHECK_EQ(cmd.polyline2d.size(), 0u) << "未勾选态不应画勾选折线";
        // 标签文本在 box 中心。
        CHECK_EQ(cmd.text2d.size(), 1u) << "应产出标签 Text2D";
        CHECK_EQ(cmd.text2d[0].text, "on");
        CHECK_EQ(cmd.text2d[0].pos.x(), box.pos.x() + box.size.x() * 0.5f);
        CHECK_EQ(cmd.text2d[0].pos.y(), box.pos.y() + box.size.y() * 0.5f);
        LOG(INFO) << "[PASS] Checkbox 未勾选态：background 方框 + 无勾 + 居中文本";
    }

    // 勾选态：value=true → 方框底=accent + 一条折线勾(3 顶点) + 文本。
    static void TestCheckedDraw() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
        bool value = true;

        ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
        ui.Checkbox("on", &value, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "勾选态应产出一方框";
        CheckFill(cmd.fillrect2d[0], 62.0f, 30.0f, 36.0f, 36.0f, theme.accent);
        // 勾选折线：一条 3 顶点 2 段折线。
        CHECK_EQ(cmd.polyline2d.size(), 1u) << "勾选态应产出一条勾选折线";
        const Polyline2DCommand& p = cmd.polyline2d[0];
        CHECK_EQ(p.vertices.size(), 3u) << "勾应为 2 段折线(3 顶点)";
        // 折线顶点应在方框内（>= 方框边界）。
        const float left = 62.0f, top = 30.0f, right = 98.0f, bottom = 66.0f;
        for (const Vec2f& v : p.vertices) {
            CHECK_GE(v.x(), left);
            CHECK_GE(v.y(), top);
            CHECK_LE(v.x(), right);
            CHECK_LE(v.y(), bottom);
        }
        CHECK_GT(p.line_width, 0.0f) << "勾线宽应 > 0";
        LOG(INFO) << "[PASS] Checkbox 勾选态：accent 方框 + 3 顶点勾线 + 文本";
    }

    // 点击翻转：value false→true 返回 true；同一 value 再点 true→false。
    static void TestClickToggles() {
        // 帧 1：value=false，点击方框内 → true 且 value 变 true。
        {
            Ui ui;
            RenderCommandList cmd;
            const UiTheme theme = UiTheme::Default(16.0f);
            const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
            bool value = false;
            const float cx = 60.0f, cy = 48.0f;  // 方框内。

            ui.Begin(MakeClickInput(cx, cy, cx, cy), theme, 640.0f, 360.0f);
            const bool clicked = ui.Checkbox("on", &value, box);
            ui.End();

            CHECK(clicked) << "点击 should 返回 true";
            CHECK(value) << "点击后 *value 应由 false 翻转为 true";
        }
        // 帧 2：同一 value=true，再点方框内 → true 且 value 变 false。
        {
            Ui ui;
            RenderCommandList cmd;
            const UiTheme theme = UiTheme::Default(16.0f);
            const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
            bool value = true;
            const float cx = 60.0f, cy = 48.0f;

            ui.Begin(MakeClickInput(cx, cy, cx, cy), theme, 640.0f, 360.0f);
            const bool clicked = ui.Checkbox("on", &value, box);
            ui.End();

            CHECK(clicked) << "再次点击应返回 true";
            CHECK(!value) << "再次点击后 *value 应由 true 翻转为 false";
        }
        LOG(INFO) << "[PASS] Checkbox 点击翻转外置 bool（true↔false）";
    }

    // 点击返回是一次性事件：无 Click 帧 → 返回 false、value 不变。
    static void TestClickOncePerFrame() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
        bool value = false;
        const float cx = 60.0f, cy = 48.0f;

        // 鼠标在框内（会悬停变色），但本帧无 Click。
        ui.Begin(MakeInput(cx, cy), theme, 640.0f, 360.0f);
        const bool clicked = ui.Checkbox("on", &value, box);
        ui.End();

        CHECK(!clicked) << "仅悬停(无 Click)不触发点击";
        CHECK(!value) << "无 Click 帧不应翻转 *value";
        LOG(INFO) << "[PASS] Checkbox 一次性事件：无 Click 帧不翻转";
    }

    // 点击在 box 外 → 不翻转、返回 false。
    static void TestClickOutsideNotFired() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{20.0f, 30.0f}, {120.0f, 36.0f}};
        bool value = false;
        const float cx = 500.0f, cy = 300.0f;  // 框外远处。

        ui.Begin(MakeClickInput(60.0f, 48.0f, cx, cy), theme, 640.0f, 360.0f);
        const bool clicked = ui.Checkbox("on", &value, box);
        ui.End();

        CHECK(!clicked) << "Click 释放位置在框外不应触发";
        CHECK(!value) << "框外点击不应翻转 *value";
        LOG(INFO) << "[PASS] Checkbox 框外点击不触发、不翻转";
    }

    // 容错：越界/零尺寸 → 0 指令、不命中、value 不变。
    static void TestOffscreenCheckbox() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        bool value = false;

        ui.Begin(MakeClickInput(700.0f, 400.0f, 700.0f, 400.0f), theme,
                 640.0f, 360.0f);
        // 完全在视口右侧外。
        const bool c1 =
            ui.Checkbox("off", &value, UiRect{{700.0f, 10.0f}, {100.0f, 30.0f}});
        // 零尺寸。
        const bool c2 =
            ui.Checkbox("zero", &value, UiRect{{10.0f, 10.0f}, {0.0f, 0.0f}});
        ui.End();
        ui.Emit(&cmd);

        CHECK(!c1 && !c2) << "越界/零尺寸复选框不应命中";
        CHECK(!value) << "越界/零尺寸点击不应翻转 *value";
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界/零尺寸不应画方框";
        CHECK_EQ(cmd.text2d.size(), 0u) << "越界/零尺寸不应画文本";
        CHECK_EQ(cmd.polyline2d.size(), 0u) << "越界/零尺寸不应画折线";
        LOG(INFO) << "[PASS] Checkbox 越界/零尺寸 → 0 指令、不命中、value 不变";
    }

    static void RunAll() {
        TestUncheckedDraw();
        TestCheckedDraw();
        TestClickToggles();
        TestClickOncePerFrame();
        TestClickOutsideNotFired();
        TestOffscreenCheckbox();
        LOG(INFO) << "===== UI S3 (Checkbox) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s3_test");
    jpov::UiS3Test::RunAll();
    return 0;
}
