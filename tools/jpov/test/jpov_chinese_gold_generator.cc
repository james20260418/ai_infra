// JPOV Chinese Text Gold Image Generator
//
// 生成包含中文字符的 gold image，验证中文文本渲染正确：
//   - 渲染分辨率 1280x720
//   - 显示中文 "你好 JPOV!" 三行，字号分别是 16/32/48
//   - 字号 16 匹配 kBaseFontSize (16px)，scale=1 不拉伸不模糊
//
// 输出: /james_pm/ai_infra/tools/jpov/test/hello_chinese_jpov_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// 字号常量 (只读)
static constexpr float kFontSizeSmall = 16.0f;   // 选 16px atlas, scale=1
static constexpr float kFontSizeMedium = 32.0f;  // 选 32px atlas, scale=1
static constexpr float kFontSizeLarge = 48.0f;   // 选 48px atlas, scale=1

// 渲染分辨率
static constexpr float kResW = 1280.0f;
static constexpr float kResH = 720.0f;

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

        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // 三行文字，行间距 yoff = 2x font_size（手工布局，简化）
        // 文字水平居中
        float center_x = kResW * 0.5f;

        // 行 1: 字号 16 (16px atlas, scale=1)
        const char* line1 = reinterpret_cast<const char*>(u8"16px: 你好 JPOV! (16px atlas)");
        cmds->DrawText(line1, {center_x, 120.0f}, kFontSizeSmall,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter);

        // 行 2: 字号 32 (32px atlas, scale=1)
        const char* line2 = reinterpret_cast<const char*>(u8"32px: 你好 JPOV! (32px atlas)");
        cmds->DrawText(line2, {center_x, 360.0f}, kFontSizeMedium,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter);

        // 行 3: 字号 48 (48px atlas, scale=1)
        const char* line3 = reinterpret_cast<const char*>(u8"48px: 你好 JPOV! (48px atlas)");
        cmds->DrawText(line3, {center_x, 600.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter);
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/hello_chinese_jpov_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "Chinese Text Gold Image Generator";
    cfg.headless = true;
    GoldChineseDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Chinese gold image generated: " << outpath;
    return 0;
}
