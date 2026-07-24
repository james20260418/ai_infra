// JPOV Chinese Text Gold Image Generator
//
// 生成包含中文字符的 gold image，验证中文文本渲染正确：
//   - 渲染分辨率 640x360
//   - 显示中文 "你好 JPOV!"，字号 48，居中
//
// 输出: /james_pm/ai_infra/tools/jpov/test/hello_chinese_jpov_48_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class GoldChineseDemo : public JPOV {
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
        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // ---- 中文文字：白色 "你好 JPOV!"，字号 48，居中 ----
        const char* text = reinterpret_cast<const char*>(u8"你好 JPOV!");
        cmds->DrawText(text, {kResW * 0.5f, kResH * 0.5f}, 48.0f,
                       jpov::kColorWhite,
                       jpov::TextAlignment::kCenter);
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/hello_chinese_jpov_48_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Chinese Text Gold Image Generator";
    cfg.headless = true;
    GoldChineseDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Chinese gold image generated: " << outpath;
    return 0;
}
