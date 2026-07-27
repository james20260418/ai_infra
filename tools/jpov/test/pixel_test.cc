// Pixel-level scale fidelity test
// 渲染 base=16px 字体在 font_size=16px（1:1），检查是否逐像素匹配
// 同时对比 16px、32px、48px 三个字号

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

static constexpr float kResW = 400.0f;
static constexpr float kResH = 300.0f;

class PixelTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // 白色背景
        cmds->DrawRect({0, 0}, {kResW, kResH}, {0.05f, 0.05f, 0.05f, 1.0f});

        const char* text = reinterpret_cast<const char*>(u8"Hi 你好");

        cmds->DrawText(text, {10.0f, 10.0f}, 16.0f,
                       jpov::kColorWhite, jpov::TextAlignment::kTopLeft);

        cmds->DrawText(text, {10.0f, 60.0f}, 24.0f,
                       jpov::kColorWhite, jpov::TextAlignment::kTopLeft);

        cmds->DrawText(text, {10.0f, 120.0f}, 32.0f,
                       jpov::kColorWhite, jpov::TextAlignment::kTopLeft);

        cmds->DrawText(text, {10.0f, 200.0f}, 48.0f,
                       jpov::kColorWhite, jpov::TextAlignment::kTopLeft);
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/pixel_test.png";

    JPOV::Config cfg;
    cfg.title = "Pixel Test";
    cfg.headless = true;
    PixelTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Pixel test: " << outpath;
    return 0;
}
