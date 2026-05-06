// JPOV Polyline Demo — 单帧绘制折线后保存 PNG 截图
//
// 使用 RunOnce 接口：headless 模式，无窗口，仅输出 PNG。
//
// 绘制内容：
//   1. 白色正三角形边框（窗口坐标）
//      - 三角形顶点在窗口内均匀分布
//      - 线宽 4px
//
// 输出：ai_infra/output/jpov_polyline_demo/frame_0000.png (1280x720)
//
// 编译运行：
//   bazel run //tools/jpov:jpov_polyline_demo

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

        // ---- 正三角形边框（窗口坐标） ----
        // 三角形顶点在窗口坐标系中均匀分布
        // 窗口 1280x720 → 三角形外接圆半径取 300px
        float cx = winfo.width * 0.5f;
        float cy = winfo.height * 0.5f;
        float r = 300.0f;

        std::vector<jpov::Vec2f> triangle;
        for (int i = 0; i < 3; ++i) {
            float angle = static_cast<float>(i) * 2.0f * 3.14159265f / 3.0f - 3.14159265f / 2.0f;
            float x = cx + r * std::cos(angle);
            float y = cy + r * std::sin(angle);
            triangle.emplace_back(x, y);
        }
        // 闭合三角形：首尾相连
        triangle.push_back(triangle[0]);

        cmds->DrawPolyline(triangle, jpov::kColorWhite, 4.0f);
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
