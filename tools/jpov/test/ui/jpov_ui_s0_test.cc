// JPOV UI S0 基础设施自证测试
//
// 验证 Ui 的"Begin 收集 → Emit 追加"指令缓冲骨架：
//   1. 空面板：Begin → Emit 后 RenderCommandList 中 0 条 2D 指令。
//   2. 单个 FillRect2D（经 friend 直接触发内部原语）→ Emit 后 1 条 FillRect2D。
//   3. SanitizeBox：负尺寸 clamp 到 0。
//   4. OutsideViewport：AABB 越界剔除判定。
//   5. PushFillRect 越界剔除：视口外矩形不产出指令。
//
// 本测试是纯 CPU 的指令层比对，不渲染、无窗口（headless 单元测试）。

#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::Color;
using jpov::InputSnapshot;
using jpov::RenderCommandList;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;
using jpov::Vec2f;

// 构造一个空输入快照（全部通道 None）。
InputSnapshot MakeEmptyInput() {
    InputSnapshot in{};
    return in;
}

}  // namespace

namespace jpov {

// S0 自证测试。作为 Ui 的 friend 访问私有原语 PushFillRect 验证缓冲追加。
class UiS0Test {
public:
    // 空面板：Begin → End → Emit，RenderCommandList 中应无 2D 指令。
    static void TestEmptyPanel() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "空面板不应产出 FillRect2D";
        CHECK_EQ(cmd.text2d.size(), 0u) << "空面板不应产出 Text2D";
        CHECK_EQ(cmd.order.size(), 0u) << "空面板不应产出绘制顺序";
        LOG(INFO) << "[PASS] 空面板 Emit → 0 条指令";
    }

    // 单个 FillRect2D：经私有原语推入一个矩形 → Emit 后恰好 1 条。
    static void TestSingleFillRect() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.PushFillRect(UiRect{{10.0f, 20.0f}, {100.0f, 40.0f}},
                        Color{1.0f, 0.0f, 0.0f, 1.0f},
                        Color{0.0f, 0.0f, 0.0f, 1.0f}, 4.0f);
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "单矩形应产出恰好 1 条 FillRect2D";
        CHECK_EQ(cmd.order.size(), 1u) << "单矩形应产出 1 条绘制顺序";
        const jpov::FillRect2DCommand& r = cmd.fillrect2d[0];
        CHECK_EQ(r.pos.x(), 10.0f);
        CHECK_EQ(r.pos.y(), 20.0f);
        CHECK_EQ(r.size.x(), 100.0f);
        CHECK_EQ(r.size.y(), 40.0f);
        LOG(INFO) << "[PASS] 单 FillRect2D → 1 条指令 (pos=" << r.pos.x()
                  << "," << r.pos.y() << " size=" << r.size.x() << "x"
                  << r.size.y() << ")";
    }

    // SanitizeBox：负尺寸 clamp 到 0。
    static void TestSanitizeBox() {
        UiRect r{{-10.0f, -20.0f}, {-5.0f, -8.0f}};
        UiRect out = Ui::SanitizeBox(r);
        CHECK_EQ(out.size.x(), 0.0f) << "负宽应 clamp 到 0";
        CHECK_EQ(out.size.y(), 0.0f) << "负高应 clamp 到 0";

        UiRect r2{{0.0f, 0.0f}, {30.0f, -4.0f}};
        UiRect out2 = Ui::SanitizeBox(r2);
        CHECK_EQ(out2.size.x(), 30.0f) << "正宽保持不变";
        CHECK_EQ(out2.size.y(), 0.0f) << "负高应 clamp 到 0";
        LOG(INFO) << "[PASS] SanitizeBox 负尺寸 clamp 到 0";
    }

    // OutsideViewport：AABB 不相交判定。
    static void TestOutsideViewport() {
        const InputSnapshot in = MakeEmptyInput();
        const UiTheme theme = UiTheme::Default(16.0f);
        Ui ui;
        ui.Begin(in, theme, 640.0f, 360.0f);

        // 完全在视口内 → 不越界。
        CHECK(!ui.OutsideViewport(UiRect{{10.0f, 10.0f}, {100.0f, 50.0f}}));
        // 完全在右侧外 → 越界。
        CHECK(ui.OutsideViewport(UiRect{{700.0f, 10.0f}, {100.0f, 50.0f}}));
        // 完全在下方外 → 越界。
        CHECK(ui.OutsideViewport(UiRect{{10.0f, 400.0f}, {100.0f, 50.0f}}));
        // 部分相交（左缘露出）→ 不越界（照画，GPU 兜底）。
        CHECK(!ui.OutsideViewport(UiRect{{-30.0f, 10.0f}, {100.0f, 50.0f}}));
        // 零尺寸矩形 → 视为不占据面积，越界。
        CHECK(ui.OutsideViewport(UiRect{{10.0f, 10.0f}, {0.0f, 50.0f}}));
        LOG(INFO) << "[PASS] OutsideViewport AABB 判定";
    }

    // PushFillRect 越界剔除：视口外矩形不产出指令。
    static void TestCull() {
        Ui ui;
        RenderCommandList cmd;
        const UiTheme theme = UiTheme::Default(16.0f);
        const InputSnapshot in = MakeEmptyInput();

        ui.Begin(in, theme, 640.0f, 360.0f);
        ui.PushFillRect(UiRect{{700.0f, 10.0f}, {100.0f, 50.0f}},
                        Color{1.0f, 0.0f, 0.0f, 1.0f},
                        Color{0.0f, 0.0f, 0.0f, 1.0f}, 4.0f);
        ui.End();
        ui.Emit(&cmd);

        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "视口外矩形应被剔除";
        LOG(INFO) << "[PASS] 越界矩形被剔除 → 0 条指令";
    }

    static void RunAll() {
        TestEmptyPanel();
        TestSingleFillRect();
        TestSanitizeBox();
        TestOutsideViewport();
        TestCull();
        LOG(INFO) << "===== UI S0 基础设施自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s0_test");
    jpov::UiS0Test::RunAll();
    return 0;
}
