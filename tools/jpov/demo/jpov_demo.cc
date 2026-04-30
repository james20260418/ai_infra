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
                      jpov::RenderCommandList& cmds) override {
        (void)winfo;
        (void)cmds;

        // 鼠标左键点击时打印
        if (input.left.IsClick()) {
            std::printf(".");
            std::fflush(stdout);
        }

        if (frame_count % 60 == 0) {
            // 每秒打印一次（60fps 假设下）
        }
    }
};

int main() {
    JPOV::Config cfg;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
