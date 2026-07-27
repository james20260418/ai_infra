// JPOV Text Color & Alignment Test
//
// 1280x720 视窗中央为原点，用半透明红/黄/绿，
// 在四个角渲染 40px 中文文本，验证颜色 blending + 四种对齐方式。

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

static constexpr float kResW = 1280.0f;
static constexpr float kResH = 720.0f;

class ColorAlignTest : public JPOV {
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

        // 深色背景
        cmds->DrawRect({0, 0}, {kResW, kResH}, {0.08f, 0.08f, 0.08f, 1.0f});

        float cx = kResW * 0.5f;
        float cy = kResH * 0.5f;

        // 辅助十字线 — 标记中心 (cx, cy)
        std::vector<jpov::Vec2f> hline = {{0.0f, cy}, {kResW, cy}};
        std::vector<jpov::Vec2f> vline = {{cx, 0.0f}, {cx, kResH}};
        cmds->DrawPolyline(hline, {0.5f, 0.5f, 0.5f, 0.5f}, 1.0f);
        cmds->DrawPolyline(vline, {0.5f, 0.5f, 0.5f, 0.5f}, 1.0f);
        cmds->DrawCircle({cx, cy}, 3.0f, {1.0f, 1.0f, 1.0f, 1.0f});

        jpov::Color kRed   = {1.0f, 0.2f, 0.2f, 0.7f};
        jpov::Color kYellow = {1.0f, 0.9f, 0.1f, 0.7f};
        jpov::Color kGreen  = {0.2f, 1.0f, 0.3f, 0.7f};

        // 左上角：黄色，kTopLeft（pos 为包围盒左上角）
        cmds->DrawText("左上", {cx, cy}, 40.0f, kYellow,
                       jpov::TextAlignment::kTopLeft);

        // 右上角：红色，kTopRight（pos 为包围盒右上角）
        cmds->DrawText("右上", {cx, cy}, 40.0f, kRed,
                       jpov::TextAlignment::kTopRight);

        // 左下角：绿色，kBottomLeft（pos 为包围盒左下角）
        cmds->DrawText("左下", {cx, cy}, 40.0f, kGreen,
                       jpov::TextAlignment::kBottomLeft);

        // 右下角：白色，kBottomRight（pos 为包围盒右下角）
        cmds->DrawText("右下", {cx, cy}, 40.0f, jpov::kColorWhite,
                       jpov::TextAlignment::kBottomRight);
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/color_align_test.png";

    JPOV::Config cfg;
    cfg.title = "Color & Alignment Test";
    cfg.headless = true;
    ColorAlignTest app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = kResW;
    winfo.height = kResH;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Color/align test: " << outpath;
    return 0;
}
