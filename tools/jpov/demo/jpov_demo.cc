// JPOV Rect Demo — 声明分辨率 + 绘制 Rect
//
// 编译运行：
//   bazel run //tools/jpov:jpov_demo

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
        (void)winfo;

        // 声明渲染分辨率：1280x720
        cmds->render_width  = 1280;
        cmds->render_height = 720;

        // 矩形在渲染分辨率的空间居中
        float rx = (1280.0f - 300.0f) * 0.5f;
        float ry = (720.0f - 200.0f) * 0.5f;
        cmds->DrawRect({rx, ry}, {300.0f, 200.0f}, jpov::kColorBlue);

        // 鼠标事件打印
        auto print = [](const char* pre, const jpov::MouseState& s,
                        const jpov::ClickEvent* cl, float mx, float my) {
            if (s.IsClick()) {
                for (int i = 0; i < s.click_count(); ++i)
                    std::printf("%sClick[%d] (%.0f,%.0f)\n", pre, i, cl[i].x, cl[i].y);
            } else if (s.IsHold()) std::printf("%sHold (%.0f,%.0f)\n", pre, mx, my);
            else if (s.IsDrag()) std::printf("%sDrag (%.0f,%.0f)\n", pre, mx, my);
        };
        print("", input.left, input.left_clicks, input.mouse_x, input.mouse_y);
        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV — Rect Demo";
    cfg.width = 1280; cfg.height = 720; cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
