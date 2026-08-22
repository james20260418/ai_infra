// JPOV Chinese Text Gold Image Generator
//
// 生成包含中文字符的 gold image，验证中文文本渲染正确：
//   - 渲染分辨率 1280x720
//   - 显示中文 "你好 JPOV!" 三行，字号分别是 16/32/48
//   - 字号 16 匹配 kBaseFontSize (16px)，scale=1 不拉伸不模糊
//
// 输出: <test_data>/font2d/hello_chinese_jpov_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

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

        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 三行文字，行间距 yoff = 2x font_size（手工布局，简化）
        // 文字水平居中
        float center_x = kResW * 0.5f;

        // 行 1: 字号 16 (16px atlas, scale=1)
        const char* line1 = reinterpret_cast<const char*>(u8"16px: 你好 JPOV! (16px atlas)");
        cmds->DrawText(line1, {center_x, 120.0f}, kFontSizeSmall,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinCJK);

        // 行 2: 字号 32 (32px atlas, scale=1)
        const char* line2 = reinterpret_cast<const char*>(u8"32px: 你好 JPOV! (32px atlas)");
        cmds->DrawText(line2, {center_x, 360.0f}, kFontSizeMedium,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       jpov::kFontBuiltinCJK);

        // 行 3: 字号 48 (48px atlas, scale=1)
        const char* line3 = reinterpret_cast<const char*>(u8"48px: 你好 楷体! (48px atlas, Kai)");
        cmds->DrawText(line3, {center_x, 600.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "Kai");

        // 行 4: 刀隶体（阿里妈妈刀隶体）
        const char* line4 = reinterpret_cast<const char*>(u8"刀隶体: 你好 刀隶! (48px, DaoLi)");
        cmds->DrawText(line4, {center_x, 60.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "DaoLi");

        // 行 5: 麦圆体（荆南麦圆体）
        const char* line5 = reinterpret_cast<const char*>(u8"麦圆体: 你好 麦圆! (48px, MaiYuan)");
        cmds->DrawText(line5, {center_x, 680.0f}, kFontSizeLarge,
                       jpov::kColorWhite, jpov::TextAlignment::kCenter,
                       "MaiYuan");
    }
};

int main() {
    std::string outpath = jpov::GetTestDataDir() + "/font2d/hello_chinese_jpov_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "Chinese Text Gold Image Generator";
    cfg.headless = true;
    cfg.fonts = {
        {"tools/jpov/fonts/LxgwWenKai-Regular.ttf", 0, "Kai"},
        {"tools/jpov/fonts/AlimamaDaoLiTi.ttf",      0, "DaoLi"},
        {"tools/jpov/fonts/KNMaiyuan-Regular.ttf",   0, "MaiYuan"},
    };
    GoldChineseDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Chinese gold image generated: " << outpath;
    return 0;
}
