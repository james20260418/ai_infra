// JPOV Rect Demo — 验证 FBO 空间坐标 + Present 裁剪
//
// 所有 2D 坐标在 FBO 空间（4096×4096）中。
// 矩形位置居中在 4096×4096 中，窗口 1280×720 只显示左上角区域。
// 调整矩形坐标到窗口可见区域才能看到它。
//
// 编译运行：
//   bazel run //tools/jpov:jpov_demo

#include <cstdint>
#include <cstdio>

#include "tools/jpov/include/jpov/jpov.h"

class DemoApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;

        // 窗口 1280×720，在 FBO 空间（4096×4096）中，左上角 1280×720 区域是可见的
        // 画一个蓝色矩形在窗口可见区域的中央
        float rx = (1280.0f - 300.0f) * 0.5f;  // 490
        float ry = (720.0f - 200.0f) * 0.5f;   // 260

        cmds->DrawRect({rx, ry}, {300.0f, 200.0f}, jpov::kColorBlue);

        // ---- 鼠标事件打印 ----
        auto print_events = [](const char* prefix,
                               const jpov::MouseState& state,
                               const jpov::ClickEvent* clicks,
                               float mx, float my) {
            if (state.IsClick()) {
                for (int i = 0; i < state.click_count(); ++i) {
                    std::printf("%sClick[%d] (%.0f, %.0f)\n", prefix, i, clicks[i].x, clicks[i].y);
                }
            } else if (state.IsHold()) {
                std::printf("%sHold (%.0f, %.0f)\n", prefix, mx, my);
            } else if (state.IsDrag()) {
                std::printf("%sDrag (%.0f, %.0f)\n", prefix, mx, my);
            }
        };

        print_events("",   input.left,   input.left_clicks,   input.mouse_x, input.mouse_y);
        print_events("R-", input.right,  input.right_clicks,  input.mouse_x, input.mouse_y);
        print_events("M-", input.middle, input.middle_clicks, input.mouse_x, input.mouse_y);

        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title    = "JPOV — Rect Demo (FBO 坐标)";
    cfg.width    = 1280;
    cfg.height   = 720;
    cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
