// JPOV Gold Image Generator — RoundRect2D 圆角矩形
//
// 生成 roundrect gold test 参考图片：
//   - 在 640x360 窗口上绘制多种圆角半径的圆角矩形
//   - 覆盖：radius=0（退化为普通矩形）、小圆角、大圆角
//   - 纯色渲染，无 MVP 变换（直接像素坐标，主 FBO）
//
// 输出: tools/jpov/test/primitives2d/roundrect_640x360.png

#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class RoundRectGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- roundrect #1: radius=0 → 退化为普通矩形（绿色） ----
        cmds->DrawRoundRect({40.0f, 30.0f}, {120.0f, 90.0f}, 0.0f,
                            {0.0f, 0.8f, 0.0f, 1.0f});

        // ---- roundrect #2: 小圆角半径 12px（蓝色） ----
        cmds->DrawRoundRect({200.0f, 30.0f}, {160.0f, 90.0f}, 12.0f,
                            {0.0f, 0.3f, 0.9f, 1.0f});

        // ---- roundrect #3: 大圆角半径 40px，正方形（红色） ----
        cmds->DrawRoundRect({60.0f, 150.0f}, {160.0f, 160.0f}, 40.0f,
                            {0.9f, 0.2f, 0.2f, 1.0f});

        // ---- roundrect #4: 中等圆角，边框非正方形（黄色半透明） ----
        cmds->DrawRoundRect({300.0f, 150.0f}, {200.0f, 120.0f}, 20.0f,
                            {0.9f, 0.8f, 0.1f, 0.8f});
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/primitives2d/roundrect_640x360.png";

    JPOV::Config cfg;
    cfg.title = "RoundRect Gold Generator";
    cfg.headless = true;
    RoundRectGoldGenerator app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}
