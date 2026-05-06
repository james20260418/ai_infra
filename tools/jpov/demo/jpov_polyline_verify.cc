// JPOV Polyline Verify — 多帧截图验证
//
// 用 RunOnce 对 frame_count = 50, 150, 300 分别截图。

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class PolylineDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)input;
        (void)winfo;

        cmds->render_width  = 640;
        cmds->render_height = 360;

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
    cfg.title    = "JPOV Polyline Verify";
    cfg.headless = true;

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};

    int64_t frames[] = {50, 150, 300};
    for (int64_t f : frames) {
        // 用一个匿名子类劫持 frame_count
        class OverrideDemo : public PolylineDemo {
        public:
            int64_t override_frame = 0;
            using PolylineDemo::PolylineDemo;
            void OneIteration(int64_t /*fc*/,
                              const jpov::InputSnapshot& inp,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds) override {
                PolylineDemo::OneIteration(override_frame, inp, winfo, cmds);
            }
        };
        OverrideDemo app(cfg);
        app.override_frame = f;
        app.Init();

        char label[32];
        std::snprintf(label, sizeof(label), "f%03lld", static_cast<long long>(f));
        std::string outpath = jpov::GetOutputDir() + "jpov_polyline_verify/frame_" + label + ".png";
        app.RunOnce(input, winfo, outpath.c_str());
        app.Finalize();
        LOG(INFO) << "Captured frame " << f << " → " << outpath;
    }

    LOG(INFO) << "All captures done.";
    return 0;
}
