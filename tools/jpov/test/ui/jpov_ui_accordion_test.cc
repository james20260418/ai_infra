// JPOV UI AccordionHeader 自证测试
//
// 验证折叠面板标题栏 AccordionHeader（状态外置，同 Checkbox）：
//   1. 展开态绘制：1 条底框（非悬停 = theme_.hover）+ 标题文本 + 1 个实心三角
//      （朝下 ▾：顶点在下方）。返回 true（本帧展开）。
//   2. 折叠态绘制：三角朝右 ▸（顶点在右侧、纵向居中），与展开态几何不同。
//   3. 点击切换：栏内点击 → 翻转外置 *open；返回值为**翻转后**的展开态。
//   4. 栏外点击 → 不翻转。
//   5. 弹层独占：落在已展开 Combo 下拉列表覆盖区的点击不翻转本栏
//      （与其它控件一致的弹层独占行为）。
//   6. 容错：越界/零尺寸 → 0 指令、原样返回展开态。
//
// 纯 CPU 指令层比对，不渲染、无窗口（headless）。

#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::ClickEvent;
using jpov::InputSnapshot;
using jpov::RenderCommandList;
using jpov::Strip2DCommand;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;

// 鼠标位于 (mx,my)、左键无交互的输入。
InputSnapshot MakeInput(float mx, float my) {
    InputSnapshot in{};
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

// 左键单击（释放位置 (cx,cy)）的输入。
InputSnapshot MakeClickInput(float mx, float my, float cx, float cy) {
    InputSnapshot in = MakeInput(mx, my);
    in.left.raw = 1;  // 1 次 Click
    in.left_clicks[0] = ClickEvent{cx, cy, 1.0f};
    return in;
}

}  // namespace

namespace jpov {

class UiAccordionTest {
public:
    // 展开态：底框 + 标题 + 朝下三角；返回 true。
    static void TestExpandedDraw() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 26.0f}};
        bool open = true;
        Ui ui;
        RenderCommandList cmd;
        ui.Begin(MakeInput(-100.0f, -100.0f), theme, 640.0f, 360.0f);
        const bool ret = ui.AccordionHeader("3D 画笔", &open, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(ret) << "展开态应返回 true";
        CHECK(open) << "无点击不应改变展开态";
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "标题栏应画 1 条底框";
        CHECK_EQ(cmd.fillrect2d[0].pos.x(), 10.0f);
        CHECK_EQ(cmd.fillrect2d[0].pos.y(), 10.0f);
        CHECK_EQ(cmd.fillrect2d[0].size.x(), 200.0f);
        CHECK_EQ(cmd.fillrect2d[0].size.y(), 26.0f);
        CHECK(cmd.fillrect2d[0].fill_color.r == theme.hover.r &&
              cmd.fillrect2d[0].fill_color.g == theme.hover.g &&
              cmd.fillrect2d[0].fill_color.b == theme.hover.b)
            << "非悬停底框应为 theme_.hover";
        CHECK_EQ(cmd.text2d.size(), 1u) << "标题栏应画 1 条标题文本";
        CHECK(cmd.text2d[0].text == "3D 画笔") << "标题文本应为标签本身";
        CHECK_EQ(cmd.strip2d.size(), 1u) << "应画 1 个指示三角";
        const std::vector<jpov::Vec2f>& v = cmd.strip2d[0].vertices;
        CHECK_EQ(v.size(), 4u) << "退化 strip 应 4 顶点";
        // 朝下 ▾：底中点（下标 2）的 y 最大；顶边两点同 y。
        CHECK(v[0].y() == v[1].y()) << "顶边两点应同 y";
        CHECK(v[2].y() > v[0].y()) << "展开态三角顶点应在下方（朝下）";
        CHECK(v[3].y() == v[2].y() && v[3].x() == v[2].x())
            << "末点应与第 3 点重合（退化闭合）";
        LOG(INFO) << "[PASS] 展开态：底框 + 标题 + 朝下三角，返回 true";
    }

    // 折叠态：三角朝右 ▸（顶点在右侧、纵向居中），且返回 false。
    static void TestCollapsedTrianglePointsRight() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 26.0f}};
        bool open = false;
        Ui ui;
        RenderCommandList cmd;
        ui.Begin(MakeInput(-100.0f, -100.0f), theme, 640.0f, 360.0f);
        const bool ret = ui.AccordionHeader("3D 画笔", &open, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!ret) << "折叠态应返回 false";
        CHECK_EQ(cmd.strip2d.size(), 1u) << "折叠态仍应画指示三角";
        const std::vector<jpov::Vec2f>& v = cmd.strip2d[0].vertices;
        CHECK_EQ(v.size(), 4u);
        // 朝右 ▸：顶点（下标 2）x 最大；纵向介于上下两点之间。
        CHECK(v[2].x() > v[0].x()) << "折叠态三角顶点应在右侧（朝右）";
        CHECK(v[0].x() == v[1].x()) << "左边两点应同 x";
        CHECK(v[2].y() > v[0].y() && v[2].y() < v[1].y())
            << "顶点应纵向介于上下两点之间";
        LOG(INFO) << "[PASS] 折叠态：三角朝右，返回 false";
    }

    // 栏内点击 → 翻转 *open；返回值 = 翻转后的展开态。
    static void TestClickInsideToggles() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 26.0f}};
        bool open = true;
        Ui ui;
        RenderCommandList cmd;
        ui.Begin(MakeClickInput(60.0f, 20.0f, 60.0f, 20.0f), theme, 640.0f, 360.0f);
        const bool ret = ui.AccordionHeader("T", &open, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!open) << "栏内点击应把展开态翻转为折叠";
        CHECK(!ret) << "返回值应为翻转后的展开态(false)";
        LOG(INFO) << "[PASS] 栏内点击翻转展开态";
    }

    // 栏外点击 → 不翻转。
    static void TestClickOutsideNoToggle() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 26.0f}};
        bool open = true;
        Ui ui;
        RenderCommandList cmd;
        ui.Begin(MakeClickInput(600.0f, 300.0f, 600.0f, 300.0f), theme, 640.0f,
                 360.0f);
        const bool ret = ui.AccordionHeader("T", &open, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(open) << "栏外点击不应改变展开态";
        CHECK(ret) << "返回值应保持展开";
        LOG(INFO) << "[PASS] 栏外点击不翻转";
    }

    // 弹层独占：落在上一帧展开的 Combo 下拉列表覆盖区内的点击不翻转本栏。
    static void TestPopupOcclusionBlocksToggle() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const std::vector<const char*> items = {"甲", "乙", "丙"};
        const UiRect combo_box{{10.0f, 10.0f}, {200.0f, 26.0f}};
        Ui ui;

        // 帧 1：点击组合框 → 展开下拉；记下 open_popup_rect_ = {10, 36, 200, 3*28}。
        {
            int sel = 0;
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(60.0f, 20.0f, 60.0f, 20.0f), theme, 640.0f,
                     360.0f);
            ui.Combo("c", &sel, items, combo_box);
            ui.End();
            ui.Emit(&cmd);
        }

        // 帧 2：把标题栏放在下拉覆盖区内，点击落在两者交集 → 应被弹层独占、
        // 不翻转。（注意本帧不画 Combo，独占矩形沿用帧 1 记录的。）
        bool open = true;
        const UiRect header{{10.0f, 40.0f}, {200.0f, 26.0f}};
        RenderCommandList cmd;
        ui.Begin(MakeClickInput(60.0f, 50.0f, 60.0f, 50.0f), theme, 640.0f,
                 360.0f);
        ui.AccordionHeader("T", &open, header);
        ui.End();
        ui.Emit(&cmd);

        CHECK(open) << "被已展开下拉覆盖的点击不应翻转标题栏";
        LOG(INFO) << "[PASS] 弹层独占：覆盖区内点击不翻转";
    }

    // 容错：越界 / 零尺寸 → 0 指令、原样返回展开态。
    static void TestOffscreen() {
        const UiTheme theme = UiTheme::Default(16.0f);
        Ui ui;

        bool open_a = true;
        RenderCommandList cmd_a;
        ui.Begin(MakeInput(0.0f, 0.0f), theme, 640.0f, 360.0f);
        const bool ret_a =
            ui.AccordionHeader("T", &open_a, UiRect{{-1000.0f, -1000.0f}, {200.0f, 26.0f}});
        ui.End();
        ui.Emit(&cmd_a);
        CHECK(ret_a && open_a) << "越界应原样返回展开态";
        CHECK(cmd_a.fillrect2d.empty() && cmd_a.text2d.empty() &&
              cmd_a.strip2d.empty())
            << "越界不应产生任何指令";

        bool open_b = false;
        RenderCommandList cmd_b;
        ui.Begin(MakeInput(0.0f, 0.0f), theme, 640.0f, 360.0f);
        const bool ret_b =
            ui.AccordionHeader("T", &open_b, UiRect{{10.0f, 10.0f}, {0.0f, 0.0f}});
        ui.End();
        ui.Emit(&cmd_b);
        CHECK(!ret_b && !open_b) << "零尺寸应原样返回折叠态";
        CHECK(cmd_b.fillrect2d.empty() && cmd_b.strip2d.empty())
            << "零尺寸不应产生指令";
        LOG(INFO) << "[PASS] 越界/零尺寸 → 0 指令、原样返回";
    }

    static void RunAll() {
        TestExpandedDraw();
        TestCollapsedTrianglePointsRight();
        TestClickInsideToggles();
        TestClickOutsideNoToggle();
        TestPopupOcclusionBlocksToggle();
        TestOffscreen();
        LOG(INFO) << "===== UI Accordion 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_accordion_test");
    jpov::UiAccordionTest::RunAll();
    return 0;
}
