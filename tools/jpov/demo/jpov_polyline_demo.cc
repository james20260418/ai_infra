// JPOV Polyline Demo — Lissajous 曲线单帧截图
//
// 使用 RunOnce 接口：headless 模式，无窗口，仅输出 PNG。
//
// 绘制内容：
//   1. Lissajous 曲线（窗坐标，1000 点）
//   2. 渐变色，8px 线宽
//
// 输出：ai_infra/output/jpov_polyline_demo/frame_0000.png (1280x720)
//
// 编译运行：
//   bazel run //tools/jpov:jpov_polyline_demo

#include <cmath>
#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class PolylineDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;

        // 声明渲染分辨率 640x360
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // ---- Lissajous 曲线（窗坐标） ----
        // 参数取 frame_count=0 时的值
        double t = 0.0;
        double a = 5.0 + 1.0 * std::sin(t * 0.3);
        double b = 4.0 + 1.0 * std::cos(t * 0.2);
        double delta = t * 0.5;

        double scale = 300.0;
        double cx = winfo.width * 0.5;
        double cy = winfo.height * 0.5;

        const int kNumPoints = 1000;
        std::vector<jpov::Vec2f> vertices;
        vertices.reserve(kNumPoints);

        for (int i = 0; i < kNumPoints; ++i) {
            double theta = 2.0 * M_PI * i / (kNumPoints - 1);
            double x = cx + scale * std::cos(a * theta + delta) * std::cos(theta);
            double y = cy + scale * std::sin(b * theta) * std::sin(theta);
            vertices.emplace_back(static_cast<float>(x), static_cast<float>(y));
        }

        jpov::Color color = {0.0f, 0.8f, 1.0f, 1.0f};  // 浅蓝色
        cmds->DrawPolyline(vertices, color, 8.0f);
    }
};

int main() {
    std::string outdir = jpov::GetOutputDir() + "jpov_polyline_demo/";
    std::string outpath = outdir + "frame_0000.png";

    // 模拟窗口尺寸 1280x720
    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;

    // 模拟空输入
    jpov::InputSnapshot input{};

    // 初始化（headless 模式）
    JPOV::Config cfg;
    cfg.title = "JPOV Polyline Demo";
    cfg.headless = true;
    PolylineDemo app(cfg);
    app.Init();
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Done: " << outpath;
    return 0;
}
