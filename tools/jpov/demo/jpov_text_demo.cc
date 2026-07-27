// JPOV Text Demo — 验证 Text2D 渲染
//
// 编译运行：
//   bazel run //tools/jpov:jpov_text_demo
//
// 输出：ai_infra/output/jpov_text_demo/rendered.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class TextDemo : public JPOV {
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
        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // ---- 蓝色矩形背景 ----
        cmds->DrawRect({0.0f, 0.0f}, {kResW, kResH}, jpov::kColorBlue);

        // ---- 白色文本 "Hello JPOV!" ----
        // 文本基线位置 (在渲染分辨率空间中)
        float text_x = kResW * 0.1f;
        float text_y = kResH * 0.5f;
        cmds->DrawText("Hello JPOV!", {text_x, text_y}, 32.0f, jpov::kColorWhite,
                       jpov::TextAlignment::kTopLeft,
                       jpov::kFontBuiltinLatin);

        // ---- 稍小的文本 "Text2D Test" ----
        cmds->DrawText("Text2D Test", {kResW * 0.1f, kResH * 0.7f},
                       24.0f, jpov::kColorGreen,
                       jpov::TextAlignment::kTopLeft,
                       jpov::kFontBuiltinLatin);

        // ---- 顶部大标题 "jpov" ----
        cmds->DrawText("JPOV", {kResW * 0.3f, kResH * 0.15f},
                       48.0f, jpov::kColorRed,
                       jpov::TextAlignment::kTopLeft,
                       jpov::kFontBuiltinLatin);
    }
};

int main() {
    std::string outdir = jpov::GetOutputDir() + "jpov_text_demo/";
    std::string outpath = outdir + "rendered.png";

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};

    JPOV::Config cfg;
    cfg.title = "JPOV Text Demo";
    cfg.headless = true;

    TextDemo app(cfg);
    app.Init();
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Text demo saved: " << outpath;
    return 0;
}
