// JPOV Text Color & Alignment Test
//
// 1280x720 视窗中央为原点，用 9 种对齐 + 不同半透明颜色
// 在中央重叠渲染 "你好 JPOV"，验证对齐 + 颜色 blending。

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

        // 深色背景 + 辅助十字
        cmds->DrawRect({0, 0}, {kResW, kResH}, {0.08f, 0.08f, 0.08f, 1.0f});
        float cx = kResW * 0.5f;
        float cy = kResH * 0.5f;
        cmds->DrawPolyline({{0.0f, cy}, {kResW, cy}}, {0.3f, 0.3f, 0.3f, 0.5f}, 1.0f);
        cmds->DrawPolyline({{cx, 0.0f}, {cx, kResH}}, {0.3f, 0.3f, 0.3f, 0.5f}, 1.0f);
        cmds->DrawCircle({cx, cy}, 3.0f, {0.5f, 0.5f, 0.5f, 1.0f});

        // 9 种对齐模式，每行用不同颜色，都以 (cx, cy) 为对齐点
        // 注意：所有调用都使用同一文本和同一位置，靠 alignment 区分锚点
        cmds->DrawText("左上", {cx, cy}, 50.0f, {1.0f, 0.2f, 0.2f, 0.5f}, jpov::TextAlignment::kTopLeft);
        cmds->DrawText("右上", {cx, cy}, 50.0f, {1.0f, 0.9f, 0.1f, 0.5f}, jpov::TextAlignment::kTopRight);
        cmds->DrawText("左下", {cx, cy}, 50.0f, {0.2f, 0.4f, 1.0f, 0.5f}, jpov::TextAlignment::kBottomLeft);
        cmds->DrawText("右下", {cx, cy}, 50.0f, {1.0f, 0.5f, 0.8f, 0.5f}, jpov::TextAlignment::kBottomRight);
        cmds->DrawText("中心", {cx, cy}, 50.0f, {0.2f, 1.0f, 0.3f, 0.5f}, jpov::TextAlignment::kCenter);
        cmds->DrawText("左中", {cx, cy}, 50.0f, {0.5f, 0.8f, 1.0f, 0.5f}, jpov::TextAlignment::kMidLeft);
        cmds->DrawText("右中", {cx, cy}, 50.0f, {1.0f, 0.6f, 0.0f, 0.5f}, jpov::TextAlignment::kMidRight);
        cmds->DrawText("上中", {cx, cy}, 50.0f, {0.9f, 0.4f, 0.9f, 0.5f}, jpov::TextAlignment::kMidTop);
        cmds->DrawText("下中", {cx, cy}, 50.0f, {0.4f, 0.9f, 0.4f, 0.5f}, jpov::TextAlignment::kMidBottom);
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
