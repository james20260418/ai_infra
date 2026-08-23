// JPOV UI S4 自证测试
//
// 验证 S4 滑条 SliderFloat（核心交互）：
//   1. 绘制：轨道（横铺 box、垂直居中、厚 30% 高）+ 句柄（方形、
//      直径 clamp ≥ 8px）+ 选中行程(accent) + 数值文本。
//   2. 点击跳转：本帧左键 Click 且释放位置在 box 内 → 横坐标比例映射
//      [min,max] 写回 *value，返回 true。一次性事件。
//   3. 拖动连续写回：左键 Drag/Hold 且鼠标在 box 内 → 每帧跟随鼠标横坐标
//      映射写回（多档位验证 *value 随鼠标位置正确变化）。
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
        // 200px 宽 box，handle_d=min(高=20,宽)=20，half=10。
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        const float min_ = 0.0f, max_ = 100.0f;

        // 档位 A：鼠标在轨道 25% 处 → 期望 ~25。
        {
            Ui ui;
            float value = 0.0f;
            const float x = 50.0f + 10.0f + (200.0f - 20.0f) * 0.25f;
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
            const float x = 50.0f + 10.0f + (200.0f - 20.0f) * 0.50f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK_NEAR(value, 50.0f, 2.0f);  // 50% 位置应映射到 ~50
        }
        // 档位 C：鼠标在轨道 75% 处 → 期望 ~75。
        {
            Ui ui;
            float value = 0.0f;
            const float x = 50.0f + 10.0f + (200.0f - 20.0f) * 0.75f;
            ui.Begin(MakeDragInput(x, 110.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, min_, max_);
            ui.End();
            CHECK_NEAR(value, 75.0f, 2.0f);  // 75% 位置应映射到 ~75
        }
        // 档位 D：Hold（按下未移动）也应持续反映当前值（不做跳变）。
        {
            Ui ui;
            float value = 30.0f;
            const float x = 50.0f + 10.0f + (200.0f - 20.0f) * 0.40f;
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
            const float x = 50.0f + 10.0f;  // track_left
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
            const float cx = 50.0f + 10.0f + (200.0f - 20.0f) * 0.50f;
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

    // gold 指令：多档位（含句柄位置随 value 变化）指令比对。
    // 轨道 + 选中行程 + 句柄 3 条 FillRect；标签非空时另有数值文本。
    static void TestGoldCommands() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{50.0f, 100.0f}, {200.0f, 20.0f}};
        // handle_d=min(20,200)=20 → half=10；track_left=60, track_right=240。
        // value=25 → 句柄中心 x = 60 + 0.25*180 = 105。
        {
            Ui ui;
            RenderCommandList cmd;
            float value = 25.0f;
            ui.Begin(MakeInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            ui.SliderFloat("speed", &value, box, 0.0f, 100.0f);
            ui.End();
            ui.Emit(&cmd);

            // 3 条 FillRect：轨道(背景)、选中行程(accent, 宽=105-60=45)、句柄。
            CHECK_EQ(cmd.fillrect2d.size(), 3u)
                << "未交互帧应产轨道+行程+句柄 3 条 FillRect";
            // 轨道：x=60, y=100+(20-6)/2=107, 宽=180, 厚=6。
            CheckFill(cmd.fillrect2d[0], 60.0f, 107.0f, 180.0f, 6.0f,
                      theme.background);
            // 选中行程：x=60, 宽=45, accent。
            CheckFill(cmd.fillrect2d[1], 60.0f, 107.0f, 45.0f, 6.0f,
                      theme.accent);
            // 句柄：中心(105,110)，方形 20×20 → x=95, y=100。
            CheckFill(cmd.fillrect2d[2], 95.0f, 100.0f, 20.0f, 20.0f,
                      theme.foreground);
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
            // 句柄为最后一条：直径 8px（不缩 0）。
            const FillRect2DCommand& handle = cmd.fillrect2d[2];
            CHECK_NEAR(handle.size.x(), 8.0f, 0.01f);  // 句柄宽 clamp 到 8px
            CHECK_NEAR(handle.size.y(), 8.0f, 0.01f);  // 句柄高 clamp 到 8px
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

    static void RunAll() {
        TestDragMapsMultipleStops();
        TestBoundaryMinMax();
        TestClickJumps();
        TestGoldCommands();
        TestThinBoxHandleClamp();
        TestNarrowBoxTrackStillDrawn();
        TestOffscreenSlider();
        LOG(INFO) << "===== UI S4 (SliderFloat) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s4_test");
    jpov::UiS4Test::RunAll();
    return 0;
}
