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

        // ---- 鼠标左键事件打印 ----
        if (input.left.IsClick()) {
            int idx = input.left.click_count() - 1;
            if (idx >= 0 && idx < jpov::kMaxClicksPerFrame) {
                auto& c = input.left_clicks[idx];
                std::printf("Click (%.0f, %.0f)\n", c.x, c.y);
            }
        }
        if (input.left.IsDrag()) {
            std::printf("Drag (%.0f, %.0f)\n", input.mouse_x, input.mouse_y);
        }

        // ---- 鼠标右键事件打印 ----
        if (input.right.IsClick()) {
            int idx = input.right.click_count() - 1;
            if (idx >= 0 && idx < jpov::kMaxClicksPerFrame) {
                auto& c = input.right_clicks[idx];
                std::printf("R-Click (%.0f, %.0f)\n", c.x, c.y);
            }
        }
        if (input.right.IsDrag()) {
            std::printf("R-Drag (%.0f, %.0f)\n", input.mouse_x, input.mouse_y);
        }

        if (frame_count % 60 == 0) {
            // 每秒打印一次帧号
            std::printf("[frame %ld]\n", static_cast<long>(frame_count));
        }

        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
