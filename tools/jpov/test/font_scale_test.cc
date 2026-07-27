// FontManager scaling quality test
//
// 测试 base=24px 时不同缩放倍率的渲染效果：
//   0.5x (12px) 1.0x (24px) 1.5x (36px) 2.0x (48px)
//
// 输出: tools/jpov/test/font_scale_test_*.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

static constexpr float kResW = 1280.0f;
static constexpr float kResH = 720.0f;
static constexpr float kBaseFontSize = 24.0f;

class ScaleTestApp : public JPOV {
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

        float cx = kResW * 0.5f;
        float y = 60.0f;

        struct Row {
            float scale;
            float fs;
            const char* label;
        };

        Row rows[] = {
            {0.5f,  kBaseFontSize * 0.5f,  reinterpret_cast<const char*>(u8"0.5x (12px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {1.0f,  kBaseFontSize * 1.0f,  reinterpret_cast<const char*>(u8"1.0x (24px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {1.5f,  kBaseFontSize * 1.5f,  reinterpret_cast<const char*>(u8"1.5x (36px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {2.0f,  kBaseFontSize * 2.0f,  reinterpret_cast<const char*>(u8"2.0x (48px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {2.5f,  kBaseFontSize * 2.5f,  reinterpret_cast<const char*>(u8"2.5x (60px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {3.0f,  kBaseFontSize * 3.0f,  reinterpret_cast<const char*>(u8"3.0x (72px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
            {4.0f,  kBaseFontSize * 4.0f,  reinterpret_cast<const char*>(u8"4.0x (96px)  你好 JPOV! 敏捷的棕色狐狸跳过懒狗")},
        };

        for (const auto& row : rows) {
            cmds->DrawText(row.label, {cx, y}, row.fs,
                           jpov::kColorWhite, jpov::TextAlignment::kCenter);
            y += row.fs * 1.8f;
        }
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/font_scale_24px.png";

    JPOV::Config cfg;
    cfg.title = "Font Scale Test";
    cfg.headless = true;
    ScaleTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Scale test image: " << outpath;
    return 0;
}
