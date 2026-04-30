// JPOV 示例：空窗口应用
//
// 编译：
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
        (void)winfo;
        (void)cmds;

        // ---- 鼠标事件打印（在 5fps 下观察 Click/Hold/Drag 判定） ----
        // 统一输出前缀：没有 R/M 前缀 = 左键

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

        // ---- 键盘事件打印 ----
        // GLFW keycode → 显示字符
        auto print_key = [&input](jpov::KeyCode code, const char* name) {
            const auto& k = input.GetKey(code);
            if (k.IsClick()) {
                std::printf("Key %s Click\n", name);
            } else if (k.IsHold()) {
                std::printf("Key %s Hold\n", name);
            }
        };

        print_key(jpov::KeyCode::A, "A");
        print_key(jpov::KeyCode::S, "S");
        print_key(jpov::KeyCode::D, "D");
        print_key(jpov::KeyCode::W, "W");
        print_key(jpov::KeyCode::Escape, "Esc");
        print_key(jpov::KeyCode::Space, "Space");

        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
