// JPOV Circle Gold Image Generator
//
// 生成 circle 测试的 gold image：
//   - 渲染分辨率 640x360
//   - 窗口分辨率 640x360（无拉伸，便于像素精确比较）
//   - 绿色圆形，居中，半径 100
//
// 输出: <test_data>/primitives2d/circle_centered_green_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class GoldCircleDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // 渲染分辨率 640x360
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- 圆形：绿色，居中，半径 100 ----
        jpov::Vec2f center(kResW * 0.5f, kResH * 0.5f);
        cmds->DrawCircle(center, 100.0f, jpov::kColorGreen);
    }
};

int main() {
    std::string outpath =
        jpov::GetTestDataDir() + "/primitives2d/circle_centered_green_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Circle Gold Image Generator";
    cfg.headless = true;
    GoldCircleDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Circle gold image generated: " << outpath;
    return 0;
}
