// JPOV Rect Demo — 演示渲染分辨率、blend、窗口相对坐标
//
// 设计：
//   渲染分辨率 640x360 < 窗口 1280x720 → Present 拉伸贴合
//   2D 坐标以渲染分辨率的空间为参照（像素坐标）
//   所有尺寸和位置按分辨率比例计算，不 hard code
//
// 绘制内容：
//   1. 蓝色矩形（不透明），居中，大小为分辨率的 1/2
//   2. 红色矩形（50% alpha），覆盖蓝色矩形的左上 1/4
//      验证 blend 效果：重叠区域应显示红蓝混合色
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

        // 声明渲染分辨率 640x360（小于窗口 1280x720）
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- 矩形1：蓝色，居中，大小为分辨率的 1/2 ----
        float rect1_w = kResW * 0.5f;
        float rect1_h = kResH * 0.5f;
        float rect1_x = (kResW - rect1_w) * 0.5f;
        float rect1_y = (kResH - rect1_h) * 0.5f;
        cmds->DrawRect({rect1_x, rect1_y}, {rect1_w, rect1_h}, jpov::kColorBlue);

        // ---- 矩形2：红色 50% alpha，从窗口左上角 (0,0) 开始，覆盖蓝色矩形
        float rect2_w = rect1_w;
        float rect2_h = rect1_h;
        jpov::Color red_alpha = {1.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect({0.0f, 0.0f}, {rect2_w, rect2_h}, red_alpha);

        // ---- 鼠标事件打印 ----
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
    app.Init();
    app.Run();
    app.Finalize();
    return 0;
}
