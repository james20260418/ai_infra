// JPOV UI 弹层独占自证测试（无窗口，纯 CPU 指令层）。
//
// 背景（2026-10-10 修复的 bug）：Combo 下拉列表画在"弹出层"（最上层），
// 但命中测试原本是逐控件对原始输入各自判定、没有 z 序 → 落在下拉矩形内的
// 鼠标事件会被"后方"控件同时消费（点击击穿到输入框/按钮、拖动击穿到滑条）。
//
// 修复：Ui 记录本帧画出的展开下拉矩形（弹出层独占区），下一帧对本帧所有
// 【非拥有者】控件屏蔽落在其内的**全部鼠标事件**（点击/悬停/按下/拖动起始）；
// 拥有者（当前展开的那个 Combo）照常处理其列表内的鼠标事件。
//
// 本测试验证（每条都配一个"无弹层时应正常响应"的正向对照，防断言恒真）：
//   1. 弹层内点选项：拥有者正常选值；后方按钮不被击穿。
//   2. 弹层内点选项：后方输入框不被聚焦（后续键入不落入）。
//   3. 弹层内按住：后方滑条不被拖动（拖动起始也被屏蔽）。
//   4. 弹层内点选项：被压在其下的另一个 Combo 不误开/误选。
//   5. 弹层外部点击：正常抵达后方控件，且下拉关闭（关闭后不再独占）。
//
// 坐标：root 面板在窗口原点，故面板局部坐标 = 窗口坐标。
//   Combo 框 {10,10,120,26}，3 选项 → 下拉列表 {10,36,120,84}（行高 28）。
//   Button {10,44,120,26} / Slider {10,74,120,26} / Input {10,104,120,26}
//   均落在下拉列表覆盖范围内。

#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {
namespace {

// UiTheme::Default 的字号/内边距（用于推算下拉行高）。
constexpr float kFontSize = 16.0f;
constexpr float kPad = 6.0f;                          // padding_px
constexpr float kRowH = kFontSize + 2.0f * kPad;      // = 28
constexpr float kColX = 70.0f;                        // 各控件横向中心
constexpr float kFrDtMs = 1000.0f / 30.0f;

UiRect ComboBox() {
    return UiRect{{10.0f, 10.0f}, {120.0f, 26.0f}};
}
UiRect ButtonBox() {
    return UiRect{{10.0f, 44.0f}, {120.0f, 26.0f}};   // 落在下拉列表内
}
UiRect SliderBox() {
    return UiRect{{10.0f, 74.0f}, {120.0f, 26.0f}};   // 落在下拉列表内
}
UiRect InputBox() {
    return UiRect{{10.0f, 104.0f}, {120.0f, 26.0f}};  // 落在下拉列表内
}
UiRect BottomButtonBox() {
    return UiRect{{10.0f, 200.0f}, {120.0f, 26.0f}};  // 在下拉列表外
}

// 下拉列表第 i 行的垂直中心 y（列表从 Combo 框下缘起）。
float OptionCenterY(int i) {
    const float list_top = ComboBox().pos.y() + ComboBox().size.y();  // 36
    return list_top + (static_cast<float>(i) + 0.5f) * kRowH;
}

// ---- 输入构造 ----
InputSnapshot Plain() {
    InputSnapshot in{};
    in.mouse_x = 999.0f;  // 远离所有控件
    in.mouse_y = 999.0f;
    return in;
}
InputSnapshot ClickAt(float x, float y) {
    InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    in.left.raw = 1;  // 1 次 Click
    in.left_clicks[0] = ClickEvent{x, y, 1.0f};
    return in;
}
InputSnapshot HoldAt(float x, float y) {
    InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    in.left.raw = -2;  // Hold（按下未移动）
    return in;
}
InputSnapshot KeyAt(KeyCode key) {
    InputSnapshot in{};
    in.mouse_x = 999.0f;
    in.mouse_y = 999.0f;
    in.keys[static_cast<int>(key)].raw = 1;  // 1 次 Click
    return in;
}

bool CmdHasText(const RenderCommandList& cmd, const char* needle) {
    for (const Text2DCommand& t : cmd.text2d) {
        if (t.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---- 集成场景：一块面板上摆 Combo + 被其下拉列表盖住的其它控件 ----
struct Scene {
    int sel = 1;  // 初值非 0，便于观察"是否被点击改变"
    std::vector<const char*> items = {"A", "B", "C"};
    float gain = 0.0f;
    char name[16] = "";
};

struct FrameOut {
    bool combo_changed = false;
    bool button_clicked = false;
    bool bottom_clicked = false;
    bool slider_changed = false;
    bool input_focused = false;
};

void DrawScene(Ui* ui, Scene* s, FrameOut* out) {
    out->combo_changed = ui->Combo("combo", &s->sel, s->items, ComboBox());
    out->button_clicked = ui->Button("btn", ButtonBox());
    out->slider_changed =
        ui->SliderFloat("gain", &s->gain, SliderBox(), 0.0f, 100.0f);
    out->input_focused =
        ui->InputText("name", s->name, sizeof(s->name), InputBox());
    out->bottom_clicked = ui->Button("bottom", BottomButtonBox());
}

void RunFrame(Ui* ui, Scene* s, const InputSnapshot& in, FrameOut* out) {
    UiTheme theme = UiTheme::Default(kFontSize);
    RenderCommandList cmd;
    ui->Begin(in, theme, 1280.0f, 720.0f, kFrDtMs);
    DrawScene(ui, s, out);
    ui->End();
    ui->Emit(&cmd);
}

// 开下拉：先稳定一帧，再点 Combo 框（点框内只切换展开态，不改选中值）。
void OpenCombo(Ui* ui, Scene* s, FrameOut* out) {
    RunFrame(ui, s, Plain(), out);
    RunFrame(ui, s, ClickAt(kColX, 23.0f), out);  // Combo 框中心
    CHECK_EQ(s->sel, 1) << "开下拉不应改变选中值";
}

}  // namespace

class UiPopupOcclusionTest {
public:
    // 1. 弹层内点选项：拥有者选值，后方按钮不被击穿。
    static void TestOptionClickOverButton() {
        Ui ui;
        Scene s;
        FrameOut out;
        OpenCombo(&ui, &s, &out);
        RunFrame(&ui, &s, ClickAt(kColX, OptionCenterY(0)), &out);  // 落在按钮上
        CHECK(out.combo_changed) << "拥有者应能选到选项";
        CHECK_EQ(s.sel, 0) << "应选中选项 0";
        CHECK(!out.button_clicked) << "弹层内点击不得击穿到后方按钮";
        LOG(INFO) << "[PASS] 弹层内点选项 → 拥有者选值、后方按钮不响应";
    }

    // 正向对照：无弹层时点同一位置，按钮应正常响应（证明上面断言非恒真）。
    static void TestButtonFiresWithoutPopup() {
        Ui ui;
        Scene s;
        FrameOut out;
        RunFrame(&ui, &s, Plain(), &out);
        RunFrame(&ui, &s, ClickAt(kColX, OptionCenterY(0)), &out);
        CHECK(out.button_clicked) << "无弹层时按钮应正常响应";
        LOG(INFO) << "[PASS] 无弹层 → 按钮正常响应（正向对照）";
    }

    // 2. 弹层内点选项：后方输入框不被聚焦，后续键入不落入。
    static void TestOptionClickOverInputNoFocus() {
        Ui ui;
        Scene s;
        FrameOut out;
        OpenCombo(&ui, &s, &out);
        RunFrame(&ui, &s, ClickAt(kColX, OptionCenterY(2)), &out);  // 落在输入框上
        CHECK_EQ(s.sel, 2) << "拥有者应选中选项 2";
        RunFrame(&ui, &s, KeyAt(KeyCode::A), &out);
        CHECK(s.name[0] == '\0')
            << "弹层内点击不得聚焦后方输入框（键入不应落入）";
        LOG(INFO) << "[PASS] 弹层内点选项 → 后方输入框不被聚焦";
    }

    // 正向对照：无弹层时点输入框 → 聚焦并接受键入。
    static void TestInputFocusWithoutPopup() {
        Ui ui;
        Scene s;
        FrameOut out;
        RunFrame(&ui, &s, Plain(), &out);
        RunFrame(&ui, &s, ClickAt(kColX, OptionCenterY(2)), &out);
        CHECK(out.input_focused) << "无弹层时点输入框应聚焦";
        RunFrame(&ui, &s, KeyAt(KeyCode::A), &out);
        CHECK(s.name[0] == 'a') << "聚焦后键入应落入 buffer";
        LOG(INFO) << "[PASS] 无弹层 → 输入框聚焦并接受键入（正向对照）";
    }

    // 3. 弹层内按住：后方滑条不被拖动（拖动起始也被屏蔽）。
    static void TestPressOverPopupNoSliderDrag() {
        Ui ui;
        Scene s;
        FrameOut out;
        OpenCombo(&ui, &s, &out);
        const float before = s.gain;
        RunFrame(&ui, &s, HoldAt(kColX, OptionCenterY(1)), &out);  // 落在滑条上
        CHECK(!out.slider_changed) << "弹层内按住不得起始拖动后方滑条";
        CHECK_EQ(s.gain, before) << "滑条值不应改变";
        LOG(INFO) << "[PASS] 弹层内按住 → 后方滑条不被拖动";
    }

    // 正向对照：无弹层时按住滑条 → 起始拖动并写值。
    static void TestPressStartsSliderDragWithoutPopup() {
        Ui ui;
        Scene s;
        FrameOut out;
        RunFrame(&ui, &s, Plain(), &out);
        RunFrame(&ui, &s, HoldAt(kColX, OptionCenterY(1)), &out);
        CHECK(s.gain > 0.0f) << "无弹层时按住滑条应起始拖动写值";
        LOG(INFO) << "[PASS] 无弹层 → 滑条正常拖动（正向对照）";
    }

    // 4. 弹层外部点击：正常抵达后方控件，且下拉关闭（之后不再独占）。
    static void TestClickOutsidePopupPassesAndCloses() {
        Ui ui;
        Scene s;
        FrameOut out;
        OpenCombo(&ui, &s, &out);
        RunFrame(&ui, &s, ClickAt(kColX, 213.0f), &out);  // 列表外：下方按钮
        CHECK(out.bottom_clicked) << "下拉外部点击应正常抵达后方控件";
        // 下拉应已关闭：再点原选项位置应命中按钮（弹层不再遮挡）。
        RunFrame(&ui, &s, ClickAt(kColX, OptionCenterY(0)), &out);
        CHECK(out.button_clicked) << "下拉应已关闭（弹层不再独占该区域）";
        LOG(INFO) << "[PASS] 弹层外点击 → 穿透并关闭下拉";
    }

    // 5. 被弹层压住的另一个 Combo 不误开/误选（弹层独占覆盖同屏多下拉）。
    static void TestComboUnderPopupDoesNotReact() {
        Ui ui;
        int sel_a = 1;
        int sel_b = 1;
        const std::vector<const char*> items_a = {"a0", "a1", "a2"};
        const std::vector<const char*> items_b = {"b0", "b1", "b2"};
        const UiRect box_a = {{10.0f, 10.0f}, {120.0f, 26.0f}};
        const UiRect box_b = {{10.0f, 44.0f}, {120.0f, 26.0f}};  // 落在 A 的下拉内
        UiTheme theme = UiTheme::Default(kFontSize);
        const auto frame = [&](const InputSnapshot& in) {
            RenderCommandList cmd;
            ui.Begin(in, theme, 1280.0f, 720.0f, kFrDtMs);
            ui.Combo("A", &sel_a, items_a, box_a);
            ui.Combo("B", &sel_b, items_b, box_b);
            ui.End();
            ui.Emit(&cmd);
            return cmd;
        };
        frame(Plain());
        frame(ClickAt(kColX, 23.0f));  // 开 A
        RenderCommandList cmd = frame(ClickAt(kColX, 50.0f));  // 点 A 选项 0
        CHECK_EQ(sel_a, 0) << "A 为拥有者，应选值";
        CHECK_EQ(sel_b, 1) << "B 被 A 的弹层盖住，不得响应（不误开/误选）";
        CHECK(!CmdHasText(cmd, "b0")) << "B 不应展开（其选项文本不应出现）";
        LOG(INFO) << "[PASS] 弹层覆盖下的另一个 Combo 不误响应";
    }

    // 6. 弹层内点击算作"点别处"：聚焦中的输入框应失焦（Danis 2026-10-10 定）。
    //    几何：Combo 框与输入框重叠，使"点开下拉"那一下同时聚焦了输入框，
    //    从而构造出"输入框聚焦且下拉已展开"的状态；随后点下拉选项
    //    （位置同时落在输入框内）——修复前会再次聚焦输入框，修复后应失焦。
    static void TestPopupClickDefocusesFocusedInput() {
        Ui ui;
        int sel = 1;
        const std::vector<const char*> items = {"A", "B", "C"};
        char name[16] = "";
        const UiRect combo_box = {{10.0f, 10.0f}, {120.0f, 26.0f}};
        const UiRect input_box = {{10.0f, 20.0f}, {120.0f, 26.0f}};  // 与框重叠
        UiTheme theme = UiTheme::Default(kFontSize);
        const auto frame = [&](const InputSnapshot& in) {
            RenderCommandList cmd;
            ui.Begin(in, theme, 1280.0f, 720.0f, kFrDtMs);
            ui.Combo("combo", &sel, items, combo_box);
            const bool focused =
                ui.InputText("name", name, sizeof(name), input_box);
            ui.End();
            ui.Emit(&cmd);
            return focused;
        };
        frame(Plain());
        // 点重叠区（y=30 ∈ Combo 框 & 输入框）→ 输入框聚焦 + 下拉展开。
        const bool f1 = frame(ClickAt(kColX, 30.0f));
        CHECK(f1) << "点重叠区应聚焦输入框（构造前置状态）";
        // 点下拉选项 0（y=40 ∈ 下拉列表 & 输入框）→ 弹层独占 → 应失焦。
        const bool f2 = frame(ClickAt(kColX, 40.0f));
        CHECK(!f2) << "弹层内点击算作点别处 → 聚焦中的输入框应失焦";
        CHECK_EQ(sel, 0) << "拥有者仍应选到选项 0";
        LOG(INFO) << "[PASS] 弹层内点击 → 聚焦中的输入框失焦";
    }

    static void RunAll() {
        TestOptionClickOverButton();
        TestButtonFiresWithoutPopup();
        TestOptionClickOverInputNoFocus();
        TestInputFocusWithoutPopup();
        TestPressOverPopupNoSliderDrag();
        TestPressStartsSliderDragWithoutPopup();
        TestClickOutsidePopupPassesAndCloses();
        TestComboUnderPopupDoesNotReact();
        TestPopupClickDefocusesFocusedInput();
        LOG(INFO) << "===== UI 弹层独占自证全部通过 =====";
    }
};

}  // namespace jpov

int main() {
    google::InitGoogleLogging("jpov_ui_popup_occlusion_test");
    jpov::UiPopupOcclusionTest::RunAll();
    return 0;
}
