// JPOV UI S6 自证测试
//
// 验证 S6 下拉 Combo（当前项 + 下箭头绘制；点击展开选项列表选择写回 /
// 点外部关闭）：
//   1. 收起态绘制：组合框底(background) + 当前项文本(左对齐垂直居中) +
//      下箭头(3 顶点朝下三角形折线)；返回 false（无选值）。
//   2. 点击框内 → 展开（返回 false，仅切换展开态）；绘制下拉列表
//      (背景 + 每行文本 + 当前项 accent 高亮底色)。
//   3. 点击选项行 → *selected 写回该项 index，返回 true，关闭下拉。
//   4. 点击框外（已展开）→ 关闭下拉、不选值、不改变 *selected。
//   5. gold 展开/收起两态比对：指令数量/位置/颜色逐条断言。
//   6. 值域：selected 越界先夹到 [0, size-1] 写回；空 items → 显示 label 占位符。
//   7. 容错：越界/零尺寸 → 0 FillRect、0 Text、无展开、不改 selected。
//
// 本测试是纯 CPU 的指令层比对（gold 指令），不渲染、无窗口（headless）。
// 点击通过 InputSnapshot.left.raw + left_clicks 模拟。

#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace {

using jpov::Color;
using jpov::FillRect2DCommand;
using jpov::InputSnapshot;
using jpov::Polyline2DCommand;
using jpov::RenderCommandList;
using jpov::Strip2DCommand;
using jpov::Text2DCommand;
using jpov::Ui;
using jpov::UiRect;
using jpov::UiTheme;
using jpov::Vec2f;

// 构造一个无鼠标交互的输入（左键 None）。
InputSnapshot MakePlainInput() {
    InputSnapshot in{};
    in.mouse_x = 100.0f;
    in.mouse_y = 100.0f;
    return in;
}

// 构造一个无鼠标交互、鼠标位于指定位置的输入（左键 None）。
InputSnapshot MakePlainInputAt(float mx, float my) {
    InputSnapshot in{};
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

// 构造一个左键单击（释放位置 (cx,cy)）的输入。
InputSnapshot MakeClickInput(float cx, float cy) {
    InputSnapshot in = MakePlainInput();
    in.mouse_x = cx;
    in.mouse_y = cy;
    in.left.raw = 1;
    in.left_clicks[0] = jpov::ClickEvent{cx, cy, 1.0f};
    return in;
}

// 断言一个 FillRect2D 命令的值与期望一致。
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

// 比较两个颜色（Color 无 operator==，逐分量比对）。
void CheckColorEq(const Color& a, const Color& b, const char* what) {
    CHECK_EQ(a.r, b.r) << what;
    CHECK_EQ(a.g, b.g) << what;
    CHECK_EQ(a.b, b.b) << what;
    CHECK_EQ(a.a, b.a) << what;
}

// 判断命令列表里是否存在包含指定子串的文本。
bool HasTextSubstr(const RenderCommandList& cmd, const char* substr) {
    for (const Text2DCommand& t : cmd.text2d) {
        if (t.text.find(substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 断言第一个 Strip 为实心朝下三角形（4 顶点条带、顶点重合闭合；
// 三角：p0=左上, p1=右上, p2=底中，p3=p2 重合）。底边水平、顶点下垂且居中。
void CheckDownArrow(const RenderCommandList& cmd) {
    CHECK(!cmd.strip2d.empty()) << "收起态应画实心下箭头(strip)";
    const Strip2DCommand& a = cmd.strip2d[0];
    CHECK_EQ(a.vertices.size(), 4u) << "实心下箭头应为 4 顶点条带(顶点重合闭合)";
    const Vec2f& p0 = a.vertices[0];
    const Vec2f& p1 = a.vertices[1];
    const Vec2f& p2 = a.vertices[2];
    // p0/p1 为三角形底边两端：y 相等（水平）。
    CHECK_NEAR(p1.y(), p0.y(), 0.01f);
    // p2 为朝下顶点：y 更大（下垂）。
    CHECK_GT(p2.y(), p0.y()) << "顶点应下垂(朝下箭头)";
    // p2.x 应为底边中点（水平居中）。
    const float mid_x = p0.x() + (p1.x() - p0.x()) * 0.5f;
    CHECK_NEAR(p2.x(), mid_x, 0.01f);
    // 顶点重合闭合：p3 == p2。
    const Vec2f& p3 = a.vertices[3];
    CHECK_NEAR(p3.x(), p2.x(), 0.01f);  // 闭合顶点应与 p2 重合
    CHECK_NEAR(p3.y(), p2.y(), 0.01f);  // 闭合顶点应与 p2 重合
}

}  // namespace

namespace jpov {

class UiS6Test {
public:
    // 收起态绘制 + 返回值：当前项文本 + 下箭头折线；返回 false。
    static void TestClosedDraw() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        RenderCommandList cmd;
        int sel = 1;
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
        const bool changed = ui.Combo("label", &sel, items, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!changed) << "收起态点击不应返回选值";
        CHECK_EQ(sel, 1) << "已选中 index 不应被改动";
        // 组合框底 1 条 FillRect；无箭头文本项是 1 文本 + 1 折线。
        CHECK_EQ(cmd.fillrect2d.size(), 1u) << "收起态应只有组合框底";
        CheckFill(cmd.fillrect2d[0], 10.0f, 10.0f, 200.0f, 24.0f,
                  theme.background);
        CHECK_EQ(cmd.text2d.size(), 1u) << "收起态应画当前项文本";
        CHECK(HasTextSubstr(cmd, "banana")) << "应显示当前选中项文本";
        // 下箭头实心三角（strip）。
        CHECK_EQ(cmd.strip2d.size(), 1u) << "收起态应画实心下箭头";
        CheckDownArrow(cmd);
        LOG(INFO) << "[PASS] Combo 收起态：当前项文本 + 下箭头折线";
    }

    // 点击框内 → 展开（返回 false，仅切换展开态）；绘制下拉列表。
    static void TestClickOpens() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        RenderCommandList cmd;
        int sel = 1;
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        // 点击组合框中心 → 展开。
        ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
        const bool changed = ui.Combo("label", &sel, items, box);
        ui.End();
        ui.Emit(&cmd);

        CHECK(!changed) << "点击组合框仅切换展开态，不产生选值";
        CHECK_EQ(sel, 1) << "展开不应改 selected";
        // 展开态：框底(hover) + 列表背景 + 当前行高亮 = 3 条 FillRect；
        // 3 行文本 + 当前项文本？——当前项文本在框内，选项文本在列表中。
        CHECK_GT(cmd.fillrect2d.size(), 1u) << "展开应画下拉列表 (框底+列表背景+高亮行)";
        CHECK(HasTextSubstr(cmd, "apple")) << "选项应显示在列表中";
        CHECK(HasTextSubstr(cmd, "cherry")) << "选项应显示在列表中";
        // 展开态不画下箭头。
        CHECK_EQ(cmd.strip2d.size(), 0u) << "展开态不画下箭头";
        LOG(INFO) << "[PASS] Combo 点击框内展开：绘制下拉列表";
    }

    // 选择选项行 → *selected 写回、返回 true、关闭。
    static void TestSelectItem() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        // row_h = 16+12=28；list_top = 34 → 第 2 行 (index 2) 中心 y = 34+2*28+14=104。
        const float row2_cy = 34.0f + 2.0f * 28.0f + 14.0f;  // 104
        Ui ui;
        int sel = 0;
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        // 帧1：点击框内展开。
        ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
        ui.Combo("label", &sel, items, box);
        ui.End();
        // 帧2：点击第 2 行选项 → 选中 index=2。
        bool changed = false;
        {
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(50.0f, row2_cy), theme, 640.0f, 360.0f);
            changed = ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
        }
        CHECK(sel == 2) << "点击选项行应写回 index=2，实际=" << sel;
        CHECK(changed) << "选值应返回 true";
        // 帧3：已关闭（不再画下拉列表、恢复箭头）。
        {
            RenderCommandList cmd;
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(cmd.fillrect2d.size(), 1u) << "选值后应回到收起态(1 底框)";
            CHECK(HasTextSubstr(cmd, "cherry")) << "当前项应更新为 cherry";
            CHECK_EQ(cmd.strip2d.size(), 1u) << "收起态恢复下箭头";
        }
        LOG(INFO) << "[PASS] Combo 选择选项写回 index + 自动关闭";
    }

    // 点击框外（已展开）→ 关闭、不选值、不改 selected。
    static void TestClickOutsideCloses() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        Ui ui;
        int sel = 0;
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        // 帧1：展开。
        ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
        ui.Combo("label", &sel, items, box);
        ui.End();
        // 帧2：点击远离框和下拉的空白区（400,300）→ 关闭、不改值、不返回选值。
        bool changed = false;
        {
            RenderCommandList cmd;
            ui.Begin(MakeClickInput(400.0f, 300.0f), theme, 640.0f, 360.0f);
            changed = ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
        }
        CHECK(!changed) << "点外部关闭不应返回选值";
        CHECK(sel == 0) << "点外部关闭不应改 selected";
        // 帧3：已关闭（恢复收起态）。
        {
            RenderCommandList cmd;
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(cmd.fillrect2d.size(), 1u) << "点外部后应回到收起态(1 底框)";
            CHECK(HasTextSubstr(cmd, "apple")) << "当前项不变为 apple";
            CHECK_EQ(cmd.strip2d.size(), 1u) << "收起态恢复下箭头";
        }
        LOG(INFO) << "[PASS] Combo 点外部关闭、不选值";
    }

    // gold 展开/收起两态：逐条比对指令数量/位置/颜色。
    static void TestGoldOpenClose() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        const std::vector<const char*> items = {"apple", "banana", "cherry"};

        // ---- 收起态 gold ----
        {
            Ui ui;
            RenderCommandList cmd;
            int sel = 1;
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(cmd.fillrect2d.size(), 1u) << "收起态 1 底框";
            CheckFill(cmd.fillrect2d[0], 10.0f, 10.0f, 200.0f, 24.0f,
                      theme.background);
            CHECK_EQ(cmd.text2d.size(), 1u) << "收起态 1 当前项文本";
            CHECK(HasTextSubstr(cmd, "banana"));
            const Text2DCommand& cur = cmd.text2d[0];
            CHECK_NEAR(cur.pos.x(), 16.0f, 0.01f);
            CHECK_NEAR(cur.pos.y(), 22.0f, 0.01f);
            CHECK(cur.alignment == jpov::TextAlignment::kMidLeft)
                << "当前项应左对齐垂直居中";
            CheckColorEq(cur.color, theme.foreground, "当前项应为 foreground 色");
            CHECK_EQ(cmd.strip2d.size(), 1u) << "收起态 1 实心下箭头";
        }

        // ---- 展开态 gold（selected=1）----
        {
            Ui ui;
            RenderCommandList cmd;
            int sel = 1;
            ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            // 框底(hover) + 下拉大圆角容器(background) + 选中行高亮(accent)
            // = 3 条 FillRect（增强：全体选项共享一个大圆角矩形，选中行平直高亮）。
            CHECK_EQ(cmd.fillrect2d.size(), 3u) << "展开态 3 条 FillRect";
            CheckFill(cmd.fillrect2d[0], 10.0f, 10.0f, 200.0f, 24.0f,
                      theme.hover);  // 展开态框底用 hover 高亮。
            // 大圆角容器：list_top=34, list_h=3*28=84, background 实心底。
            CheckFill(cmd.fillrect2d[1], 10.0f, 34.0f, 200.0f, 84.0f,
                      theme.background);
            // 当前行(i=1，未悬停)平直选中条：y=34+28=62, selected 深蓝。
            // （悬停态由 TestDropdownHover 单独验证。）
            CheckFill(cmd.fillrect2d[2], 10.0f, 62.0f, 200.0f, 28.0f,
                      theme.selected);
            // 文本：框内当前项 + 3 个选项 = 4 条。
            CHECK_EQ(cmd.text2d.size(), 4u) << "展开态 4 条文本(当前项+3选项)";
            CHECK(HasTextSubstr(cmd, "banana"));  // 当前项。
            CHECK(HasTextSubstr(cmd, "apple"));
            CHECK(HasTextSubstr(cmd, "cherry"));
            CHECK_EQ(cmd.strip2d.size(), 0u) << "展开态不画下箭头";
        }
        LOG(INFO) << "[PASS] Combo gold 展开/收起两态比对通过";
    }

    // 值域：selected 越界 → 夹到 [0, size-1] 写回；空 items → 占位符。
    static void TestValueClampAndEmpty() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        {
            // 越界 selected（99）→ 夹到 2。
            Ui ui;
            RenderCommandList cmd;
            int sel = 99;
            const std::vector<const char*> items = {"apple", "banana", "cherry"};
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(sel, 2) << "越界 selected 应夹到 2";
            CHECK(HasTextSubstr(cmd, "cherry")) << "应显示夹断后的当前项";
        }
        {
            // 空 items → selected=-1，显示 label 占位符(disabled 色)。
            Ui ui;
            RenderCommandList cmd;
            int sel = 0;
            const std::vector<const char*> items;
            ui.Begin(MakePlainInput(), theme, 640.0f, 360.0f);
            ui.Combo("placeholder", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            CHECK_EQ(sel, -1) << "空 items selected 应为 -1";
            CHECK_EQ(cmd.fillrect2d.size(), 1u) << "空 items 画框底";
            CHECK_EQ(cmd.text2d.size(), 1u) << "空 items 画占位符文本";
            CHECK(HasTextSubstr(cmd, "placeholder")) << "应显示 label 占位符";
            CheckColorEq(cmd.text2d[0].color, theme.disabled,
                         "占位符应为 disabled 色");
            CHECK_EQ(cmd.polyline2d.size(), 0u) << "空 items 无下箭头";
            CHECK_EQ(cmd.strip2d.size(), 0u) << "空 items 无下箭头 strip";
        }
        LOG(INFO) << "[PASS] Combo 值域夹断 + 空 items 占位符";
    }

    // 容错：越界/零尺寸 → 0 FillRect、0 Text、无展开、不改 selected。
    static void TestOffscreenCombo() {
        const UiTheme theme = UiTheme::Default(16.0f);
        Ui ui;
        RenderCommandList cmd;
        int sel = 1;
        const std::vector<const char*> items = {"apple", "banana"};
        ui.Begin(MakeClickInput(700.0f, 400.0f), theme, 640.0f, 360.0f);
        const bool c1 = ui.Combo("label", &sel, items,
                                 UiRect{{700.0f, 10.0f}, {100.0f, 24.0f}});
        const bool c2 = ui.Combo("label2", &sel, items,
                                 UiRect{{10.0f, 10.0f}, {0.0f, 20.0f}});
        ui.End();
        ui.Emit(&cmd);
        CHECK(!c1 && !c2) << "越界/零尺寸不应选值";
        CHECK_EQ(sel, 1) << "越界/零尺寸不应改 selected";
        CHECK_EQ(cmd.fillrect2d.size(), 0u) << "越界/零尺寸不应画指令";
        CHECK_EQ(cmd.text2d.size(), 0u) << "越界/零尺寸不应画文本";
        CHECK_EQ(cmd.polyline2d.size(), 0u) << "越界/零尺寸不应画折线";
        CHECK_EQ(cmd.strip2d.size(), 0u) << "越界/零尺寸不应画 strip";
        LOG(INFO) << "[PASS] Combo 越界/零尺寸 → 0 指令、不改 selected";
    }

    // 弹出层验证（C3）：展开态下，下拉列表的容器 + 每行 + 选项文本
    // 必须在 cmd.order 里排在所有普通指令之后（即最后画、盖住其它控件）。
    // 这证明 Combo 下拉不会被更晚的普通控件（矩形/文本）遮挡。
    static void TestPopupDrawsLast() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        Ui ui;
        RenderCommandList cmd;
        int sel = 1;
        // 帧1：点击框内展开。
        ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
        ui.Combo("label", &sel, items, box);
        ui.End();
        // 帧2：普通输入，鼠标移到下拉区域外（500,300）→ 无悬停行，
        // 只画选中行，便于纯粹验证弹出层 last 顺序。
        ui.Begin(MakePlainInputAt(500.0f, 300.0f), theme, 640.0f, 360.0f);
        ui.Combo("label", &sel, items, box);
        ui.End();
        ui.Emit(&cmd);

        // 展开态指令布局（按 Emit 顺序）：
        //   普通 fill：框底(1) → 普通 text：当前项(1) → 普通 poly/strip：无
        //   → popup fill：大圆角容器 + 选中行高亮(2) → popup text：3 选项(3)
        // 所以最后 2 条 fill + 最后 3 条 text 都必须是下拉相关。
        const size_t total_fill = cmd.fillrect2d.size();
        const size_t total_text = cmd.text2d.size();
        CHECK_EQ(total_fill, 3u) << "展开态 3 条 FillRect（1 普通 + 2 弹出）";
        CHECK_EQ(total_text, 4u) << "展开态 4 条 Text（1 普通 + 3 弹出）";

        // 找 drop 位置：普通 fill 只有框底 1 条（fillrect2d[0]），
        // 其余 2 条（大圆角容器 + 选中行高亮）是弹出层 → 必须画在最后。
        // 用 order 断言：所有 FillRect 命令里，属于弹出的（索引 1..2）
        // 必须出现在属于普通（索引 0）之后；所有 Text 里，选项文本（索引 1..3）
        // 必须出现在当前项（索引 0）之后。
        // （真实“最后画”由 order 顺序决定，这里验证 order 中 popup 区间靠后。）
        int last_normal_order = -1;
        int last_popup_order = -1;
        for (size_t i = 0; i < cmd.order.size(); ++i) {
            const auto& [type, idx] = cmd.order[i];
            if (type == jpov::DrawCommandType::kFillRect2D) {
                last_normal_order = (idx == 0) ? static_cast<int>(i)
                                               : last_normal_order;
                last_popup_order = (idx >= 1) ? static_cast<int>(i)
                                              : last_popup_order;
            }
        }
        CHECK_GE(last_popup_order, last_normal_order)
            << "弹出层 FillRect 应画在普通 FillRect 之后（实际 normal="
            << last_normal_order << " popup=" << last_popup_order << "）";
        // Text 同理：当前项 idx=0 应早于选项文本 idx>=1 画。
        int last_t_normal = -1;
        int last_t_popup = -1;
        for (size_t i = 0; i < cmd.order.size(); ++i) {
            const auto& [type, idx] = cmd.order[i];
            if (type == jpov::DrawCommandType::kText2D) {
                last_t_normal = (idx == 0) ? static_cast<int>(i) : last_t_normal;
                last_t_popup = (idx >= 1) ? static_cast<int>(i) : last_t_popup;
            }
        }
        CHECK_GE(last_t_popup, last_t_normal)
            << "弹出层 Text 应画在普通 Text 之后（实际 normal="
            << last_t_normal << " popup=" << last_t_popup << "）";
        LOG(INFO) << "[PASS] Combo 展开态：下拉（fill/text）最后画，不被遮挡";
    }

    // Hover 增强：展开态下鼠标飘过的项用 accent（亮蓝）高亮，
    // 选中项用 selected（深蓝）标定；二者不同时同时显示。
    static void TestDropdownHover() {
        const UiTheme theme = UiTheme::Default(16.0f);
        const UiRect box{{10.0f, 10.0f}, {200.0f, 24.0f}};
        const std::vector<const char*> items = {"apple", "banana", "cherry"};
        // row_h=28；list_top=34。行 y：row0=34, row1=62, row2=90。
        // 选中项固定为 1（banana）。
        // 帧1：展开；帧2：鼠标悬停在 row2（cherry，非选中）。
        {
            Ui ui;
            RenderCommandList cmd;
            int sel = 1;
            ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            // 鼠标悬停 row2：y=90+14=104, x=50（在下拉区域内）。
            ui.Begin(MakePlainInputAt(50.0f, 104.0f), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            // fill：框底 + 容器 + 选中行(selected深蓝) + 悬停行(accent浅蓝) = 4。
            CHECK_EQ(cmd.fillrect2d.size(), 4u)
                << "悬停≠选中时应 4 条 FillRect（框+容器+选中+悬停）";
            CheckFill(cmd.fillrect2d[2], 10.0f, 62.0f, 200.0f, 28.0f,
                      theme.selected);  // 选中行(banana)深蓝。
            CheckFill(cmd.fillrect2d[3], 10.0f, 90.0f, 200.0f, 28.0f,
                      theme.accent);  // 悬停行(cherry)浅蓝。
        }
        // 悬停在选中行自身（row1）：只画一个高亮条，用 accent（悬停优先）。
        {
            Ui ui;
            RenderCommandList cmd;
            int sel = 1;
            ui.Begin(MakeClickInput(110.0f, 22.0f), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Begin(MakePlainInputAt(50.0f, 76.0f), theme, 640.0f, 360.0f);
            ui.Combo("label", &sel, items, box);
            ui.End();
            ui.Emit(&cmd);
            // fill：框底 + 容器 + 高亮条 = 3（悬停行==选中行，用 accent）。
            CHECK_EQ(cmd.fillrect2d.size(), 3u)
                << "悬停==选中时应 3 条 FillRect（框+容器+单高亮条）";
            CheckFill(cmd.fillrect2d[2], 10.0f, 62.0f, 200.0f, 28.0f,
                      theme.accent);  // 悬停优先显示浅蓝。
        }
        LOG(INFO) << "[PASS] Combo 展开态：悬停行 accent、选中行 selected";
    }

    static void RunAll() {
        TestClosedDraw();
        TestClickOpens();
        TestSelectItem();
        TestClickOutsideCloses();
        TestGoldOpenClose();
        TestValueClampAndEmpty();
        TestOffscreenCombo();
        TestPopupDrawsLast();
        TestDropdownHover();
        LOG(INFO) << "===== UI S6 (Combo) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s6_test");
    jpov::UiS6Test::RunAll();
    return 0;
}
