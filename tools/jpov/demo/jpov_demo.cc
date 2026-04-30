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

        // ---- 鼠标事件打印（在 5fps 下观察 Click/Drag 判定） ----
        // 统一输出前缀：没有 R/M 前缀 = 左键
        if (input.left.IsClick()) {
            for (int i = 0; i < input.left.click_count(); ++i) {
                std::printf("Click[%d] (%.0f, %.0f)\n", i, input.left_clicks[i].x, input.left_clicks[i].y);
            }
        }
        if (input.left.IsDrag()) {
            std::printf("Drag (%.0f, %.0f)\n", input.mouse_x, input.mouse_y);
        }

        if (input.right.IsClick()) {
            for (int i = 0; i < input.right.click_count(); ++i) {
                std::printf("R-Click[%d] (%.0f, %.0f)\n", i, input.right_clicks[i].x, input.right_clicks[i].y);
            }
        }
        if (input.right.IsDrag()) {
            std::printf("R-Drag (%.0f, %.0f)\n", input.mouse_x, input.mouse_y);
        }

        if (input.middle.IsClick()) {
            for (int i = 0; i < input.middle.click_count(); ++i) {
                std::printf("M-Click[%d] (%.0f, %.0f)\n", i, input.middle_clicks[i].x, input.middle_clicks[i].y);
            }
        }
        if (input.middle.IsDrag()) {
            std::printf("M-Drag (%.0f, %.0f)\n", input.mouse_x, input.mouse_y);
        }

        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.target_fps = 5;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
