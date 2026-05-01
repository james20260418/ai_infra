// JPOV 示例：带宽度的动态 Lissajous 曲线
//
// 演示 Renderer 的 polyline2D 绘制能力：
//   - 每帧生成大量顶点（~1000 个点）
//   - 曲线形状和线宽每帧变化
//   - 通过 geometry shader 展开为带厚度的四边形
//   - 所有顶点通过流式 VBO（orphan + subdata）上传
//
// 编译运行：
//   bazel run //tools/jpov:jpov_demo

#include <cmath>
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
        (void)input;
        (void)winfo;

        // Lissajous 曲线参数（每帧变化）
        double t = static_cast<double>(frame_count) * 0.02;  // 时间
        double a = 5.0 + 1.0 * std::sin(t * 0.3);            // x 频率
        double b = 4.0 + 1.0 * std::cos(t * 0.2);            // y 频率
        double delta = t * 0.5;                                // 相位差
        double scale = 200.0;                                  // 缩放（像素）
        double cx = 640.0, cy = 360.0;                         // 中心

        // 生成顶点（~1000 个点，每帧全部重建）
        const int kNumPoints = 1000;
        std::vector<jpov::Vec2f> vertices;
        vertices.reserve(kNumPoints);

        for (int i = 0; i < kNumPoints; ++i) {
            double theta = 2.0 * M_PI * i / (kNumPoints - 1);
            double x = cx + scale * std::cos(a * theta + delta) * std::cos(theta);
            double y = cy + scale * std::sin(b * theta) * std::sin(theta);
            vertices.emplace_back(static_cast<float>(x), static_cast<float>(y));
        }

        // 线宽随帧变化：3 ~ 15 像素
        float line_width = 3.0f + 12.0f * (0.5f + 0.5f * std::sin(t * 0.5f));

        // 颜色也渐变
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
    cfg.title    = "JPOV — Polyline2D with Width (Lissajous)";
    cfg.width    = 1280;
    cfg.height   = 720;
    cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
