// JPOV Polyline Demo — 动态 Lissajous 曲线
//
// 使用 Run() 连续播放，30fps。
// 窗口 640x360（与渲染分辨率一致，无拉伸）。
// 曲线参数每帧变化，产生动态效果。

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "tools/jpov/include/jpov/jpov.h"

class PolylineDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)input;
        (void)winfo;

        // 渲染分辨率 640x360，与窗口尺寸一致
        cmds->camera.fbo_3d_width_  = 640.0f;
        cmds->camera.fbo_3d_height_ = 360.0f;

        // ---- Lissajous 曲线 ----
        double t = static_cast<double>(frame_count) * 0.02;
        double a = 5.0 + 1.0 * std::sin(t * 0.3);
        double b = 4.0 + 1.0 * std::cos(t * 0.2);
        double delta = t * 0.5;

        double scale = 150.0;
        double cx = 320.0, cy = 180.0;

        const int kNumPoints = 1000;
        std::vector<jpov::Vec2f> vertices;
        vertices.reserve(kNumPoints);

        for (int i = 0; i < kNumPoints; ++i) {
            double theta = 2.0 * M_PI * i / (kNumPoints - 1);
            double x = cx + scale * std::cos(a * theta + delta) * std::cos(theta);
            double y = cy + scale * std::sin(b * theta) * std::sin(theta);
            vertices.emplace_back(static_cast<float>(x), static_cast<float>(y));
        }

        // 颜色和线宽也随时间变化
        float line_width = 9.0f + 36.0f * (0.5f + 0.5f * std::sin(t * 0.5f));
        jpov::Color color;
        color.r = 0.5f + 0.5f * std::sin(t * 0.7f);
        color.g = 0.5f + 0.5f * std::sin(t * 0.5f + 2.1f);
        color.b = 0.5f + 0.5f * std::sin(t * 0.3f + 4.2f);
        color.a = 1.0f;

        cmds->DrawPolyline(vertices, color, line_width);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title     = "JPOV — Lissajous Polyline2D";
    cfg.width     = 640;
    cfg.height    = 360;
    cfg.headless  = false;
    cfg.target_fps = 30;
    PolylineDemo app(cfg);
    app.Init();
    app.Run();
    app.Finalize();
    return 0;
}
