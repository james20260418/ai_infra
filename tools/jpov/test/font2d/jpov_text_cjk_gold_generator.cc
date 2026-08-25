// JPOV CJK Text Gold Image Generator
//
// 生成包含 CJK 字符的 gold image，验证 CJK 文本渲染正确：
//   - 渲染分辨率 640x360
//   - 显示中文 "你好 世界！" 和英文 "Hello World!" 混合排版
//   - 字号 36
//
// 输出: <test_data>/font2d/hello_cjk_36_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class GoldCjkTextDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- CJK 文字：白色 "你好 世界！Hello World!"，字号 36，居中 ----
        const char* text = reinterpret_cast<const char*>(u8"你好 世界！Hello World!");
        cmds->DrawText(text, {kResW * 0.5f, kResH * 0.5f}, 36.0f,
                       jpov::kColorWhite,
                       jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinCJK);
    }
};

int main() {
    std::string outpath = jpov::GetTestDataDir() + "/font2d/hello_cjk_36_640x360.png";

    JPOV::Config cfg;
    cfg.title = "CJK Text Gold Image Generator";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
    };
    GoldCjkTextDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "CJK gold image generated: " << outpath;
    return 0;
}
