// base tint 调色板 UI 单元测试（无窗口，纯 UI 指令层）：
//   验证 DrawBaseTintPalette 的 R/G/B 滑条**绑定到正确的通道**（不能两个通道串线）。
//   背景：Danis 2026-10-27 反馈「tint2 区域不响应」——先排除「滑条绑错字段」这种 UI 层原因。
//
//   布局（本测试固定）：first_row=(20,20) 380×30、step_y=42
//     row0 enable, row1 Tint1 色块, row2 Tint1 RGB, row3 Tint2 色块, row4 Tint2 RGB
//   RGB 三列等宽：(380-2*8)/3 = 121.33。

#include <cmath>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/interface/tint_ui.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {
namespace {

InputSnapshot MakeInput(float mx, float my) {
    InputSnapshot in{};
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

void RunFrame(Ui* ui, BaseTint* tint, const InputSnapshot& in) {
    RenderCommandList cmd;
    ui->Begin(in, UiTheme::Default(16.0f), 640.0f, 360.0f, 16.0f);
    DrawBaseTintPalette(*ui, tint, UiRect{{20.0f, 20.0f}, {380.0f, 30.0f}}, 42.0f);
    ui->End();
    ui->Emit(&cmd);
}

// 第 row 行、第 col 列（0=R,1=G,2=B）滑条的点击点（横向 60% 处）。
ClickEvent SliderClick(int row, int col) {
    constexpr float kX = 20.0f;
    constexpr float kW = 380.0f;
    constexpr float kGap = 8.0f;
    constexpr float kSliderW = (kW - kGap * 2.0f) / 3.0f;
    const float x = kX + static_cast<float>(col) * (kSliderW + kGap) +
                    kSliderW * 0.6f;
    const float y = 20.0f + static_cast<float>(row) * 42.0f + 15.0f;
    return ClickEvent{x, y, 1.0f};
}

InputSnapshot ClickOn(const ClickEvent& c) {
    InputSnapshot in = MakeInput(c.x, c.y);
    in.left.raw = 1;
    in.left_clicks[0] = c;
    return in;
}

void ExpectNear(float got, float want, const char* what) {
    CHECK(std::fabs(got - want) <= 0.03f)
        << what << " got=" << got << " want≈" << want;
}

}  // namespace

void RunTintUiTest() {
    // 默认：Tint1 = 红，Tint2 = 绿（两个工具的默认）。
    BaseTint tint;
    tint.enabled = true;
    tint.color[0] = {1.0f, 0.0f, 0.0f, 1.0f};
    tint.color[1] = {0.0f, 1.0f, 0.0f, 1.0f};

    Ui ui;
    RunFrame(&ui, &tint, MakeInput(0.0f, 0.0f));  // 稳定一帧

    // 点 Tint2 的 R 滑条（row4, col0）→ 只有 color[1].r 变，color[0] 原样。
    RunFrame(&ui, &tint, ClickOn(SliderClick(4, 0)));
    ExpectNear(tint.color[1].r, 0.6f, "Tint2 R");
    ExpectNear(tint.color[0].r, 1.0f, "Tint1 R 不该被 Tint2 滑条改动");
    ExpectNear(tint.color[1].g, 1.0f, "Tint2 G 不该被 R 滑条改动");
    ExpectNear(tint.color[1].b, 0.0f, "Tint2 B 不该被 R 滑条改动");

    // 点 Tint1 的 G 滑条（row2, col1）→ 只有 color[0].g 变。
    RunFrame(&ui, &tint, ClickOn(SliderClick(2, 1)));
    ExpectNear(tint.color[0].g, 0.6f, "Tint1 G");
    ExpectNear(tint.color[0].r, 1.0f, "Tint1 R 不该被 G 滑条改动");
    ExpectNear(tint.color[1].r, 0.6f, "Tint2 R 保持上一步的值");
    ExpectNear(tint.color[1].g, 1.0f, "Tint2 G 不该被 Tint1 滑条改动");

    // 点 Tint2 的 B 滑条（row4, col2）→ 只有 color[1].b 变。
    RunFrame(&ui, &tint, ClickOn(SliderClick(4, 2)));
    ExpectNear(tint.color[1].b, 0.6f, "Tint2 B");
    ExpectNear(tint.color[0].b, 0.0f, "Tint1 B 不该被 Tint2 滑条改动");

    LOG(INFO) << "TEST PASSED: tint 调色板两通道 RGB 滑条各自绑定正确（无串线）";
}

}  // namespace jpov

int main() {
    jpov::RunTintUiTest();
    return 0;
}
