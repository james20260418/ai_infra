// JPOV Text Gold Image Generator — 生成 text 测试的 gold image
//
// 生成 text 测试的 gold image：
//   - 渲染分辨率 640x360
//   - 窗口分辨率 640x360（无拉伸，便于像素精确比较）
//   - 白色文字 "Hello JPOV!"，字号 48，居中显示
//
// 输出: <test_data>/font2d/hello_jpov_48_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class GoldTextDemo : public JPOV {
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

        // ---- 文字：白色 "Hello JPOV!"，字号 48，居中 ----
        const char* text = "Hello JPOV!";
        cmds->DrawText(text, {kResW * 0.5f, kResH * 0.5f}, 48.0f,
                       jpov::kColorWhite,
                       jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinLatin);
    }
};

int main() {
    std::string outpath = jpov::GetTestDataDir() + "/font2d/hello_jpov_48_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Text Gold Image Generator";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
    };
    GoldTextDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}
