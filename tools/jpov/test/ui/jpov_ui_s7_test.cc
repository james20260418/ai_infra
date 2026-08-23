// JPOV UI S7 集成 demo 自证测试
//
// 验证 S7 集成调试台（jpov_ui_demo）的一次 Emit 能产出完整的 gold 指令全貌。
// 复用与交互 demo 完全一致的布局源（tools/jpov/demo/ui_demo_panel.h 的
// DrawUiDemoPanel）——同一份代码驱动 demo 与测试，杜绝 golden 分叉。
//
// 断言（纯 CPU 指令层，无窗口）：
//   1. 全貌指令计数：22 FillRect / 20 Text / 2 Polyline（空输入、默认状态）。
//   2. 每类控件/背景的代表性 rect（位置/尺寸/颜色）逐条断言：
//      静态背景(log 框 + 3 张电机卡片)、增益滑条(轨道/选中/句柄)、使能复选框、
//      Combo、输入框、色块、按钮(重置/应用)、电机卡片(滑条/复选框)。
//   3. 全部控件标签文本存在（标题/各 label/电机标题/实时日志）。
//   4. 折线：使能勾选 ✓ + Combo 收起下箭头（2 条）。
//   5. 函数包装复用：3 份电机卡片指令结构相同但位置在 x 方向随索引偏移。
//
// 状态的改变通过 InputSnapshot 模拟（本测试专注"空输入一次 Emit"的 gold 全貌，
// 交互行为的单元覆盖见 S2~S6 各自测试）。

#include <algorithm>
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
        // Ui 控件 18 条：增益滑条(轨道+选中+句柄=3)、使能方框(1)、Combo 框(1)、
        //   输入框底(1)、色块(1)、按钮×2(2)、电机×3 内(每: 滑条轨道+句柄+复选框框=3 → 9)。
        // → 22 FillRect。
        // Text 20 条：标题/全局参数/增益: 5/使能/Combo项/输入占位/色样/重置/应用/
        //   电机组标题/电机×3(标题+speed+反转=9)/实时日志。
        // → Polyline 2 条：使能勾选 ✓ + Combo 收起下箭头。
        CHECK_EQ(cmd.fillrect2d.size(), 22u)
            << "全貌 FillRect 数量不符，实际=" << cmd.fillrect2d.size();
        CHECK_EQ(cmd.text2d.size(), 20u)
            << "全貌 Text 数量不符，实际=" << cmd.text2d.size();
        CHECK_EQ(cmd.polyline2d.size(), 2u)
            << "全貌 Polyline 数量不符（期望 使能✓ + combo箭头），实际="
            << cmd.polyline2d.size();

        const Color bg = theme.background;
        const Color accent = theme.accent;
        const Color fg = theme.foreground;
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
        // GlobalRow(0)={16,84,544,26}; thickness=7.8; handle_d=26; half=13;
        // track={29,93.1,518,7.8}; t=0.5 → selected={29,93.1,259,7.8}; handle={275,84,26,26}.
        CheckFill(cmd.fillrect2d[4], 29.0f, 93.1f, 518.0f, 7.8f, bg);
        CheckFill(cmd.fillrect2d[5], 29.0f, 93.1f, 259.0f, 7.8f, accent);
        CheckFill(cmd.fillrect2d[6], 275.0f, 84.0f, 26.0f, 26.0f, fg);
        CHECK(HasTextSubstr(cmd, "增益: 5")) << "增益滑条应显示值 5";

        // ---- 2c. 使能复选框（enable=true → accent 方框 + ✓ 折线）----
        // GlobalRow(1,160)={16,118,160,26} → 真方形 side=26 居中。
        CheckFill(cmd.fillrect2d[7], 83.0f, 118.0f, 26.0f, 26.0f, accent);
        CHECK(HasTextSubstr(cmd, "使能 Enable"));

        // ---- 2d. Combo（closed 框底 background；当前项="高速"）----
        CheckFill(cmd.fillrect2d[8], 16.0f, 152.0f, 240.0f, 26.0f, bg);
        CHECK(HasTextSubstr(cmd, "高速")) << "Combo 当前项应为默认 profile=1";

        // ---- 2e. 输入框（buffer 空 → 占位符；底框 background）----
        CheckFill(cmd.fillrect2d[9], 16.0f, 186.0f, 360.0f, 26.0f, bg);
        CHECK(HasTextSubstr(cmd, "输入电机名…")) << "输入框应显示占位符";

        // ---- 2f. 色块 + 标注 ----
        CheckFill(cmd.fillrect2d[10], 80.0f, 220.0f, 40.0f, 40.0f,
                  Color{0.25f, 0.60f, 0.95f, 1.0f});
        CHECK(HasTextSubstr(cmd, "色样"));

        // ---- 2g. 按钮行（reset / apply，底色 accent）----
        CheckFill(cmd.fillrect2d[11], 16.0f, 274.0f, 120.0f, 30.0f, accent);
        CheckFill(cmd.fillrect2d[12], 152.0f, 274.0f, 120.0f, 30.0f, accent);
        CHECK(HasTextSubstr(cmd, "重置"));
        CHECK(HasTextSubstr(cmd, "应用"));

        // ---- 2h. 电机组（函数包装复用 ×3）----
        CHECK(HasTextSubstr(cmd, "电机组"));
        CHECK(HasTextSubstr(cmd, "电机 1"));
        CHECK(HasTextSubstr(cmd, "电机 2"));
        CHECK(HasTextSubstr(cmd, "电机 3"));
        CHECK(HasTextSubstr(cmd, "speed: 0")) << "每份电机滑条应显示 speed: 0";
        // 每张卡片内：1 滑条轨道 + 1 句柄 + 1 复选框框 = 3 条 FillRect（cards 从 idx 13 起）。
        // 卡片 i 的速度滑条轨道：box={x+16,320+34,cardW-32,26}，
        //   thickness=7.8, handle_d=min(max(26,8),cardW-32); cardW≈173→ 内宽141.
        const size_t card_base = 13u;
        for (int i = 0; i < UiDemoState::kMotorCount; ++i) {
            const float x = kUiDemoPad + static_cast<float>(i) * step;
            const float x_in = x + kUiDemoPad;
            const float w_in = kUiDemoMotorCardW - 2.0f * kUiDemoPad;  // ≈141
            const float handle_d = std::min(std::max(26.0f, 8.0f), w_in);  // =26
            const float half = handle_d * 0.5f;
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

        // ---- 3. 折线：使能勾选 ✓（首条）+ Combo 收起下箭头（次条）----
        // 绘制顺序：使能复选框在前、Combo 在后 → polyline2d[0]=✓, [1]=箭头。
        CHECK_EQ(cmd.polyline2d.size(), 2u);
        // 使能勾选 ✓：3 顶点折线（非平坦底边，中间点下沉再上挑）。
        CHECK_EQ(cmd.polyline2d[0].vertices.size(), 3u)
            << "使能勾选应为 3 顶点折线";
        // Combo 下箭头：3 顶点三角形，底边两端 y 相等（水平），顶点朝下居中。
        {
            const Polyline2DCommand& a = cmd.polyline2d[1];
            CHECK_EQ(a.vertices.size(), 3u) << "Combo 下箭头应为 3 顶点三角形";
            CHECK_NEAR(a.vertices[1].y(), a.vertices[0].y(), 0.01f);
            CHECK_GT(a.vertices[2].y(), a.vertices[0].y()) << "顶点应朝下";
            const float mid_x = a.vertices[0].x() +
                (a.vertices[1].x() - a.vertices[0].x()) * 0.5f;
            CHECK_NEAR(a.vertices[2].x(), mid_x, 0.01f);
        }

        LOG(INFO) << "[PASS] S7 集成 demo 一次 Emit gold 全貌比对通过"
                  << " (fill=" << cmd.fillrect2d.size()
                  << ", text=" << cmd.text2d.size()
                  << ", poly=" << cmd.polyline2d.size() << ")";
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

        // 基准 text=20；多一条 log 行 → 21，且含其内容。
        CHECK_EQ(cmd.text2d.size(), 21u)
            << "追加 1 条 log 后 Text 应 +1（20→21），实际=" << cmd.text2d.size();
        CHECK(HasTextSubstr(cmd, "点击【重置】")) << "log 文本应被画出";
        LOG(INFO) << "[PASS] S7 实时 Log：追加条目被面板绘制";
    }

    static void RunAll() {
        TestPanelGold();
        TestLogVisible();
        LOG(INFO) << "===== UI S7 (集成 demo) 自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_s7_test");
    jpov::UiS7Test::RunAll();
    return 0;
}
