// JPOV Rect Demo — 演示渲染分辨率与窗口尺寸的区分
//
// 渲染分辨率 < 窗口尺寸 → 输出被拉伸贴合窗口。
// 2D 绘制以窗口坐标系为参照（像素坐标），
// 实际显示效果由 Present 时的拉伸决定。
//
// 编译运行：
//   bazel run //tools/jpov:jpov_demo

#include <cstdio>
#include <glog/logging.h>

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

        // 声明渲染分辨率（像素）：640x360 — 小于窗口尺寸 1280x720
        // Present 时会将 640x360 的内容拉伸到窗口大小显示。
        // 2D 坐标以 640x360 为空间（左上角原点）：
        //   x ∈ [0, 640)，y ∈ [0, 360)
        // 矩形在渲染分辨率空间居中：100x100
        cmds->render_width  = 640;
        cmds->render_height = 360;

        float rx = (640.0f - 100.0f) * 0.5f;
        float ry = (360.0f - 100.0f) * 0.5f;
        cmds->DrawRect({rx, ry}, {100.0f, 100.0f}, jpov::kColorBlue);

        // 鼠标事件打印
        auto print = [](const char* pre, const jpov::MouseState& s,
                        const jpov::ClickEvent* cl, float mx, float my) {
            if (s.IsClick()) {
                for (int i = 0; i < s.click_count(); ++i) {
                    std::printf("%sClick[%d] (%.0f,%.0f)\n", pre, i, cl[i].x, cl[i].y);
                }
            } else if (s.IsHold()) {
                std::printf("%sHold (%.0f,%.0f)\n", pre, mx, my);
            } else if (s.IsDrag()) {
                std::printf("%sDrag (%.0f,%.0f)\n", pre, mx, my);
            }
        };
        print("", input.left, input.left_clicks, input.mouse_x, input.mouse_y);
        std::fflush(stdout);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title = "JPOV — Rect Demo (640x360 → 1280x720)";
    cfg.width = 1280;
    cfg.height = 720;
    cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
