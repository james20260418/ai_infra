// JPOV UI S7 集成 demo 自证测试
//
// 验证 S7 集成调试台（jpov_ui_demo）的一次 Emit 能产出完整的 gold 指令全貌。
// 复用与交互 demo 完全一致的布局源（tools/jpov/demo/ui_demo_panel.h 的
// DrawUiDemoPanel）——同一份代码驱动 demo 与测试，杜绝 golden 分叉。
//
// 断言（纯 CPU 指令层，无窗口）：
//   1. 全貌指令计数：23 FillRect / 21 Text / 2 Polyline（空输入、默认状态）。
//   2. 每类控件/背景的代表性 rect（位置/尺寸/颜色）逐条断言：
//      静态背景(log 框 + 3 张电机卡片)、增益滑条(轨道/选中/句柄)、使能复选框、
//      Combo(运行模式)、Combo(显示模式，同屏第二个下拉)、输入框、色块、
//      按钮(重置/应用)、电机卡片(滑条/复选框)。
//   3. 全部控件标签文本存在（标题/各 label/电机标题/实时日志）。
//   4. 折线：使能勾选 ✓ + 两个 Combo 收起下箭头（2 条 strip）。
//   5. 函数包装复用：3 份电机卡片指令结构相同但位置在 x 方向随索引偏移。
//   6. 同屏两个下拉（面板真实布局）互不干扰：点开一个保持展开、另一个不受
//      牵连；点另一个则展开态转移（2026-09-15 Danis 实测 bug 的集成回归）。
//
// 状态的改变通过 InputSnapshot 模拟（本测试专注"空输入一次 Emit"的 gold 全貌，
// 交互行为的单元覆盖见 S2~S6 各自测试）。

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/demo/ui_demo_panel.h"

namespace {

// 断言一个 FillRect 命令的值与期望一致。
void CheckFill(const jpov::FillRect2DCommand& r, float px, float py, float w,
               float h, const jpov::Color& fill) {
    CHECK_NEAR(r.pos.x(), px, 0.01f);
    CHECK_NEAR(r.pos.y(), py, 0.01f);
    CHECK_NEAR(r.size.x(), w, 0.01f);
    CHECK_NEAR(r.size.y(), h, 0.01f);
    CHECK_EQ(r.fill_color.r, fill.r);
    CHECK_EQ(r.fill_color.g, fill.g);
    CHECK_EQ(r.fill_color.b, fill.b);
}

// 判断文本列表是否存在含指定子串的项。
bool HasTextSubstr(const jpov::RenderCommandList& cmd, const char* substr) {
    for (const jpov::Text2DCommand& t : cmd.text2d) {
        if (t.text.find(substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 查找左上角坐标命中的首个 FillRect（面板局部坐标精确匹配）；无则 nullptr。
// 用于断言"某个下拉框当前是展开态(hover)还是收起态(background)"。
const jpov::FillRect2DCommand* FindFillAt(const jpov::RenderCommandList& cmd,
                                          const jpov::Vec2f& pos) {
    for (const jpov::FillRect2DCommand& r : cmd.fillrect2d) {
        if (std::abs(r.pos.x() - pos.x()) < 0.01f &&
            std::abs(r.pos.y() - pos.y()) < 0.01f) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

namespace jpov {

class UiS7Test {
public:
    // 构造无交互输入（鼠标停在右上角 Log 空白，左键 None，无按键）。
    static InputSnapshot PlainInput() {
        InputSnapshot in{};
        in.mouse_x = 900.0f;
        in.mouse_y = 400.0f;
        return in;
    }

    // 构造无交互输入、鼠标在指定位置（左键 None）。
    static InputSnapshot PlainInputAt(float mx, float my) {
        InputSnapshot in{};
        in.mouse_x = mx;
        in.mouse_y = my;
        return in;
    }

    // 构造左键单击（释放位置 (cx,cy)）。
    static InputSnapshot MakeClickInput(float cx, float cy) {
        InputSnapshot in{};
        in.mouse_x = cx;
        in.mouse_y = cy;
        in.left.raw = 1;
        in.left_clicks[0] = jpov::ClickEvent{cx, cy, 1.0f};
        return in;
    }

    // 用空输入、默认状态，一次 Begin → DrawUiDemoPanel → End → Emit，全貌比对。
    static void TestPanelGold() {
        const UiTheme theme = UiTheme::Default(kUiDemoFontSize);
        UiDemoState st;  // 默认状态：gain=5, enable=true, profile=1, name 空,
                         // 3 电机 speed=0 / inverted=false，Log 空。

        Ui ui;
        RenderCommandList cmd;
        ui.Begin(PlainInput(), theme, kUiDemoWidth, kUiDemoHeight);
        DrawUiDemoPanel(ui, &cmd, st);  // 静态背景直接写进 cmd；控件走 ui。
        ui.End();
        ui.Emit(&cmd);

        // ---- 1. 全貌指令计数（gold：布局变要随之更新）----
        // 静态 4 条：log 底框 + 3 电机卡片底。
        // Ui 控件 19 条：增益滑条(轨道+选中+句柄=3)、使能方框(1)、Combo 框(1)、
        //   Combo 框(显示模式，1)、输入框底(1)、色块(1)、按钮×2(2)、
        //   电机×3 内(每: 滑条轨道+句柄+复选框框=3 → 9)。
        // → 23 FillRect。
        // Text 21 条：标题/全局参数/增益: 5/使能/Combo项(高速)/Combo项(实体)/
        //   输入占位/色样/重置/应用/电机组标题/电机×3(标题+speed+反转=9)/实时日志。
        // → Polyline 1 条：使能勾选 ✓；Strip 2 条：两个 Combo 各一条收起实心下箭头。
        CHECK_EQ(cmd.fillrect2d.size(), 23u)
            << "全貌 FillRect 数量不符，实际=" << cmd.fillrect2d.size();
        CHECK_EQ(cmd.text2d.size(), 21u)
            << "全貌 Text 数量不符，实际=" << cmd.text2d.size();
        CHECK_EQ(cmd.polyline2d.size(), 1u)
            << "全貌 Polyline 数量不符（期望 使能✓），实际="
            << cmd.polyline2d.size();
        CHECK_EQ(cmd.strip2d.size(), 2u)
            << "全貌 Strip 数量不符（期望 两个 combo 实心箭头），实际="
            << cmd.strip2d.size();

        const Color bg = theme.background;
        const Color accent = theme.accent;
        const Color fg = theme.foreground;
        const Color hover = theme.hover;
        const Color log_bg{0.09f, 0.10f, 0.12f, 1.0f};
        const Color card_bg{0.16f, 0.18f, 0.21f, 1.0f};

        // ---- 2a. 静态背景：log 底框（右列矩形）+ 3 张电机卡片底 ----
        CheckFill(cmd.fillrect2d[0], kUiDemoRightX, kUiDemoRightTop,
                  kUiDemoRightW, kUiDemoRightBottom - kUiDemoRightTop, log_bg);
        const float step = kUiDemoMotorCardW + kUiDemoMotorCardGap;
        for (int i = 0; i < UiDemoState::kMotorCount; ++i) {
            const float x = kUiDemoPad + static_cast<float>(i) * step;
            CheckFill(cmd.fillrect2d[1u + static_cast<size_t>(i)], x,
                      kUiDemoMotorCardY, kUiDemoMotorCardW,
                      kUiDemoMotorCardH, card_bg);
        }

        // ---- 2b. 增益滑条（轨道/选中/句柄）----
        // GlobalRow(0)={16,84,544,26}; thickness=7.8; handle_h=26; handle_w=26/φ=16.07; half=8.04;
        // track_left=16+8.04=24.04, track_right=560-8.04=551.96, track_len=527.92;
        // t=0.5 → hx=24.04+0.5*527.92=288;
        // 轨道={24.04,93.1,527.92,7.8}; 选中={24.04,93.1,263.96,7.8};
        // handle 中心(288,110) → rect={288-8.04=279.96≈280,84,16.07,26}。
        CheckFill(cmd.fillrect2d[4], 24.03f, 93.1f, 527.93f, 7.8f, bg);
        CheckFill(cmd.fillrect2d[5], 24.03f, 93.1f, 263.97f, 7.8f, accent);
        // 增益滑条句柄：danis bug#9 句柄改用深色 hover（不再与浅色文本同色）；
        // bug#10 方形改黄金比矩形（宽:高=1:φ≈0.618，x 窄 y 高）。
        CheckFill(cmd.fillrect2d[6], 279.97f, 84.0f, 16.07f, 26.0f, hover);
        CHECK(HasTextSubstr(cmd, "增益: 5")) << "增益滑条应显示值 5";

        // ---- 2c. 使能复选框（enable=true → accent 方框 + ✓ 折线）----
        // GlobalRow(1,160)={16,118,160,26} → 真方形 side=26 居中。
        CheckFill(cmd.fillrect2d[7], 83.0f, 118.0f, 26.0f, 26.0f, accent);
        CHECK(HasTextSubstr(cmd, "使能 Enable"));

        // ---- 2d. Combo ×2（closed 框底 background；当前项="高速"/"实体"）----
        // 运行模式 Combo：GlobalRow(2,240)={16,152,240,26}。
        CheckFill(cmd.fillrect2d[8], 16.0f, 152.0f, 240.0f, 26.0f, bg);
        CHECK(HasTextSubstr(cmd, "高速")) << "Combo 当前项应为默认 profile=1";
        // 显示模式 Combo（同屏第二个下拉）：同排右移 256 → {272,152,240,26}。
        CheckFill(cmd.fillrect2d[9], 272.0f, 152.0f, 240.0f, 26.0f, bg);
        CHECK(HasTextSubstr(cmd, "实体")) << "第二个 Combo 当前项应为默认 view_mode=0";

        // ---- 2e. 输入框（buffer 空 → 占位符；底框 background）----
        CheckFill(cmd.fillrect2d[10], 16.0f, 186.0f, 360.0f, 26.0f, bg);
        CHECK(HasTextSubstr(cmd, "输入电机名…")) << "输入框应显示占位符";

        // ---- 2f. 色块 + 标注 ----
        CheckFill(cmd.fillrect2d[11], 80.0f, 220.0f, 40.0f, 40.0f,
                  Color{0.25f, 0.60f, 0.95f, 1.0f});
        CHECK(HasTextSubstr(cmd, "色样"));

        // ---- 2g. 按钮行（reset / apply，底色 hover 深色——非悬停态默认）----
        CheckFill(cmd.fillrect2d[12], 16.0f, 274.0f, 120.0f, 30.0f, hover);
        CheckFill(cmd.fillrect2d[13], 152.0f, 274.0f, 120.0f, 30.0f, hover);
        CHECK(HasTextSubstr(cmd, "重置"));
        CHECK(HasTextSubstr(cmd, "应用"));

        // ---- 2h. 电机组（函数包装复用 ×3）----
        CHECK(HasTextSubstr(cmd, "电机组"));
        CHECK(HasTextSubstr(cmd, "电机 1"));
        CHECK(HasTextSubstr(cmd, "电机 2"));
        CHECK(HasTextSubstr(cmd, "电机 3"));
        CHECK(HasTextSubstr(cmd, "speed: 0")) << "每份电机滑条应显示 speed: 0";
        // 每张卡片内：1 滑条轨道 + 1 句柄 + 1 复选框框 = 3 条 FillRect（cards 从 idx 14 起）。
        // 卡片 i 的速度滑条轨道：box={x+16,320+34,cardW-32,26}，
        //   thickness=7.8, handle_h=min(max(26,8),cardW-32); cardW≈173→ 内宽141.
        const float kGoldenRatio = 1.618033988749895f;
        const size_t card_base = 14u;
        for (int i = 0; i < UiDemoState::kMotorCount; ++i) {
            const float x = kUiDemoPad + static_cast<float>(i) * step;
            const float x_in = x + kUiDemoPad;
            const float w_in = kUiDemoMotorCardW - 2.0f * kUiDemoPad;  // ≈141
            // 黄金比句柄（bug#10）：高=handle_h=26，宽=高/φ≈16.07，half=8.03。
            const float handle_h = std::min(std::max(26.0f, 8.0f), w_in);  // =26
            const float half = handle_h / kGoldenRatio * 0.5f;
            const float track_left = x_in + half;
            const float track_right = x_in + w_in - half;
            const float track_len = std::max(1.0f, track_right - track_left);
            const float cy = 320.0f + 34.0f + 13.0f;  // box y + 34 + h/2
            const float thick = std::max(4.0f, 26.0f * 0.30f);  // 7.8
            // 卡片底(1) 已计；卡片内 fillrect 顺序：轨道、句柄、复选框框。
            CheckFill(cmd.fillrect2d[card_base + static_cast<size_t>(i) * 3u + 0u],
                      track_left, cy - thick * 0.5f, track_len, thick, bg);
            // 复选框：box={x+16,320+70,110,26} → 真方形 side=26，x 偏移 (110-26)/2=42。
            const float cb_x = x + kUiDemoPad + (110.0f - 26.0f) * 0.5f;
            CheckFill(cmd.fillrect2d[card_base + static_cast<size_t>(i) * 3u + 2u],
                      cb_x, 320.0f + 70.0f, 26.0f, 26.0f, bg);
        }

        // ---- 2i. 实时日志 ----
        CHECK(HasTextSubstr(cmd, "实时日志"));

        // ---- 3. 折线/条带：使能勾选 ✓（polyline）+ 两个 Combo 收起实心箭头（strip）----
        // 使能勾选 ✓：3 顶点折线（非平坦底边，中间点下沉再上挑）。
        CHECK_EQ(cmd.polyline2d.size(), 1u);
        CHECK_EQ(cmd.polyline2d[0].vertices.size(), 3u)
            << "使能勾选应为 3 顶点折线";
        // Combo 实心下箭头 ×2（运行模式 / 显示模式）：各为 4 顶点三角形条带
        //（顶点重合闭合），底边两端 y 相等（水平），顶点朝下居中。
        for (const Strip2DCommand& a : cmd.strip2d) {
            CHECK_EQ(a.vertices.size(), 4u) << "Combo 实心箭头应为 4 顶点条带";
            CHECK_NEAR(a.vertices[1].y(), a.vertices[0].y(), 0.01f);
            CHECK_GT(a.vertices[2].y(), a.vertices[0].y()) << "顶点应朝下";
            const float mid_x = a.vertices[0].x() +
                (a.vertices[1].x() - a.vertices[0].x()) * 0.5f;
            CHECK_NEAR(a.vertices[2].x(), mid_x, 0.01f);
            // 闭合顶点与 p2 重合。
            CHECK_NEAR(a.vertices[3].x(), a.vertices[2].x(), 0.01f);
            CHECK_NEAR(a.vertices[3].y(), a.vertices[2].y(), 0.01f);
        }
        // 两个下拉的箭头 x 互不相同（确实是两个独立控件、同屏两个下拉）。
        CHECK_LT(cmd.strip2d[0].vertices[0].x(), cmd.strip2d[1].vertices[0].x())
            << "第二个 Combo 的下箭头应在其右侧（同屏两个下拉）";

        LOG(INFO) << "[PASS] S7 集成 demo 一次 Emit gold 全貌比对通过"
                  << " (fill=" << cmd.fillrect2d.size()
                  << ", text=" << cmd.text2d.size()
                  << ", poly=" << cmd.polyline2d.size()
                  << ", strip=" << cmd.strip2d.size() << ")";
    }

    // 验证实时 Log 输出框：追加 log 后，面板会把最新条目以 Text 指令体现。
    static void TestLogVisible() {
        const UiTheme theme = UiTheme::Default(kUiDemoFontSize);
        UiDemoState st;
        st.log.Append(st.time_s, "点击【重置】");
        st.time_s = 1.0;

        Ui ui;
        RenderCommandList cmd;
        ui.Begin(PlainInput(), theme, kUiDemoWidth, kUiDemoHeight);
        DrawUiDemoPanel(ui, &cmd, st);
        ui.End();
        ui.Emit(&cmd);

        // 基准 text=21；多一条 log 行 → 22，且含其内容。
        CHECK_EQ(cmd.text2d.size(), 22u)
            << "追加 1 条 log 后 Text 应 +1（21→22），实际=" << cmd.text2d.size();
        CHECK(HasTextSubstr(cmd, "点击【重置】")) << "log 文本应被画出";
        LOG(INFO) << "[PASS] S7 实时 Log：追加条目被面板绘制";
    }

    // 集成回归：面板里的同屏两个下拉互不干扰（2026-09-15 Danis 手测的 bug
    // "一点就收"）。本测试直接调 DrawUiDemoPanel（真实面板、真实布局），
    // 所以和交互 demo 走的是同一条代码路径；点击坐标取自面板导出的 box 访问器
    // （UiDemoProfileComboBox / UiDemoViewModeComboBox），不会与布局漂移。
    static void TestTwoComboDropdownsIndependent() {
        const UiTheme theme = UiTheme::Default(kUiDemoFontSize);
        UiDemoState st;
        Ui ui;
        const UiRect box_profile = UiDemoProfileComboBox();
        const UiRect box_view = UiDemoViewModeComboBox();
        // 两框中心（同一排）。
        const float p_cx = box_profile.pos.x() + box_profile.size.x() * 0.5f;
        const float p_cy = box_profile.pos.y() + box_profile.size.y() * 0.5f;
        const float v_cx = box_view.pos.x() + box_view.size.x() * 0.5f;
        const float v_cy = box_view.pos.y() + box_view.size.y() * 0.5f;
        // 展开标志：两框当前项均与其列表第一项相同，故用各自"非当前项"的
        // 选项文本作为"该下拉已展开"的可判据（收起态只画当前项文本）。
        //   「运行模式」当前=高速(1)，展开才可见 节能(2)；
        //   「显示模式」当前=实体(0)，展开才可见 法线(2)。
        CHECK(st.profile == 1) << "前置：默认 profile 应为 1(高速)";
        CHECK(st.view_mode == 0) << "前置：默认 view_mode 应为 0(实体)";

        auto draw = [&](const InputSnapshot& in, RenderCommandList* cmd) {
            ui.Begin(in, theme, kUiDemoWidth, kUiDemoHeight);
            DrawUiDemoPanel(ui, cmd, st);
            ui.End();
            ui.Emit(cmd);
        };

        // 帧1：点开「运行模式」（左）。
        {
            RenderCommandList cmd;
            draw(MakeClickInput(p_cx, p_cy), &cmd);
            CHECK(HasTextSubstr(cmd, "节能")) << "点左下拉应展开（列表可见）";
            CHECK(!HasTextSubstr(cmd, "法线")) << "右下拉不应被牵连展开";
            // 左展开(hover) / 右收起(background)。
            const FillRect2DCommand* lf = FindFillAt(cmd, box_profile.pos);
            const FillRect2DCommand* rf = FindFillAt(cmd, box_view.pos);
            CHECK(lf != nullptr && rf != nullptr);
            CHECK_EQ(lf->fill_color.r, theme.hover.r) << "左下拉应为展开态";
            CHECK_EQ(rf->fill_color.r, theme.background.r)
                << "右下拉应保持收起态";
        }

        // 帧2（回归关键）：无点击 → 左下拉必须仍然展开。
        {
            RenderCommandList cmd;
            draw(PlainInputAt(1100.0f, 650.0f), &cmd);
            CHECK(HasTextSubstr(cmd, "节能"))
                << "【回归】无点击时左下拉应保持展开（bug：一闪即收）";
            CHECK(!HasTextSubstr(cmd, "法线")) << "右下拉仍应收起";
        }

        // 帧3：点右下拉 → 展开态转移（左收起、右展开）。
        {
            RenderCommandList cmd;
            draw(MakeClickInput(v_cx, v_cy), &cmd);
            CHECK(!HasTextSubstr(cmd, "节能")) << "点右下拉后左应收起";
            CHECK(HasTextSubstr(cmd, "法线")) << "点右下拉后右应展开";
            const FillRect2DCommand* lf = FindFillAt(cmd, box_profile.pos);
            const FillRect2DCommand* rf = FindFillAt(cmd, box_view.pos);
            CHECK(lf != nullptr && rf != nullptr);
            CHECK_EQ(lf->fill_color.r, theme.background.r) << "左已收起";
            CHECK_EQ(rf->fill_color.r, theme.hover.r) << "右展开态";
        }

        // 帧4：无点击 → 右下拉保持展开（左先画也不得清掉它）。
        {
            RenderCommandList cmd;
            draw(PlainInputAt(1100.0f, 650.0f), &cmd);
            CHECK(HasTextSubstr(cmd, "法线"))
                << "【回归】右下拉应保持展开（先画的左下拉不得清掉它）";
            CHECK(!HasTextSubstr(cmd, "节能")) << "左仍应收起";
        }

        // 帧5：点空白 → 全部收起，两个下拉各画一个下箭头。
        {
            RenderCommandList cmd;
            draw(MakeClickInput(1100.0f, 650.0f), &cmd);
            CHECK(!HasTextSubstr(cmd, "节能")) << "点空白后左收起";
            CHECK(!HasTextSubstr(cmd, "法线")) << "点空白后右收起";
            CHECK_EQ(cmd.strip2d.size(), 2u) << "两个下拉均恢复下箭头";
        }
        LOG(INFO) << "[PASS] S7 集成：同屏两个下拉互不干扰（2026-09-15 bug 回归）";
    }

    static void RunAll() {
        TestPanelGold();
        TestLogVisible();
        TestTwoComboDropdownsIndependent();
        LOG(INFO) << "===== UI S7 (集成 demo) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s7_test");
    jpov::UiS7Test::RunAll();
    return 0;
}
