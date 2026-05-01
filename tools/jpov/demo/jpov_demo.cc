// JPOV 示例：带宽度的动态 Lissajous 曲线
//
// 演示 Renderer 的 polyline2D 绘制能力（CPU 端 miter join + triangle strip）：
//   - 每帧生成大量顶点（~1000 个点）
//   - 曲线形状和线宽每帧变化
//   - CPU 端展开为带宽度的 triangle strip，无 geometry shader
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
        (void)winfo;

        // ---- 键盘事件打印（保留调试能力） ----
        auto print_key = [&input](jpov::KeyCode code, const char* name) {
            const auto& k = input.GetKey(code);
            if (k.IsClick()) {
                std::printf("Key %s Click\n", name);
            } else if (k.IsHold()) {
                std::printf("Key %s Hold\n", name);
            }
        };
        print_key(jpov::KeyCode::Escape, "Esc");
        print_key(jpov::KeyCode::Space, "Space");

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
        print_events("", input.left, input.left_clicks, input.mouse_x, input.mouse_y);
        std::fflush(stdout);

        // ---- Lissajous 曲线（适配 1280×720 窗口） ----
        double t = static_cast<double>(frame_count) * 0.02;
        double a = 5.0 + 1.0 * std::sin(t * 0.3);
        double b = 4.0 + 1.0 * std::cos(t * 0.2);
        double delta = t * 0.5;

        // 保证在窗口内：中心 (640, 360)，max 幅度 = 300（窗口最短边 360，留 60 像素边距）
        float scale = 300.0f;
        float cx = 640.0f, cy = 360.0f;

        const int kNumPoints = 1000;
        std::vector<jpov::Vec2f> vertices;
        vertices.reserve(kNumPoints);

        for (int i = 0; i < kNumPoints; ++i) {
            double theta = 2.0 * M_PI * i / (kNumPoints - 1);
            float x = cx + scale * std::cos(a * theta + delta) * std::cos(theta);
            float y = cy + scale * std::sin(b * theta) * std::sin(theta);
            vertices.emplace_back(x, y);
        }

        // 线宽 3 ~ 45 像素（放大展示 miter join 效果）
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
    cfg.title    = "JPOV — Polyline2D with Width (CPU Miter Join)";
    cfg.width    = 1280;
    cfg.height   = 720;
    cfg.target_fps = 30;
    DemoApp app(cfg);
    app.Run();
    return 0;
}
