// JPOV Gold Image Generator — 生成 gold image 用作单元测试参考
//
// 生成 rect 测试的 gold image：
//   - 渲染分辨率 640x360
//   - 窗口分辨率 640x360（无拉伸，便于像素精确比较）
//   - 蓝色矩形，居中，大小为渲染分辨率的 1/2
//
// 输出: /james_pm/ai_infra/tools/jpov/test/rect_centered_blue_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class GoldRectDemo : public JPOV {
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

        // ---- 矩形：蓝色，居中，大小为分辨率的 1/2 ----
        float rect_w = kResW * 0.5f;
        float rect_h = kResH * 0.5f;
        float rect_x = (kResW - rect_w) * 0.5f;
        float rect_y = (kResH - rect_h) * 0.5f;
        cmds->DrawRect({rect_x, rect_y}, {rect_w, rect_h}, jpov::kColorBlue);
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/rect_centered_blue_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Gold Image Generator";
    cfg.headless = true;
    GoldRectDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}
