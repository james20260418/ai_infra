// JPOV Gold Image Generator — Arc2D 圆弧/扇形
//
// 生成 arc2d gold test 参考图片：
//   - 在 640x360 窗口上绘制多种圆弧/扇形变体
//   - 覆盖：半圆、扇形、完整圆、负角度（顺时针）
//   - 纯色渲染，直接像素坐标主 FBO
//
// 输出: tools/jpov/test/arc2d_640x360.png

#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class Arc2dGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- arc #1: 半圆 (180度，绿色) ----
        // 圆心 (120,120)，半径 80，从 0 到 180 度（逆时针）
        cmds->DrawArc2D({120.0f, 120.0f}, 80.0f, 0.0f, 180.0f,
                        {0.0f, 0.8f, 0.0f, 1.0f});

        // ---- arc #2: 扇形 (90度，蓝色) ----
        // 圆心 (320,120)，半径 80，从 45 到 135 度（逆时针）
        cmds->DrawArc2D({320.0f, 120.0f}, 80.0f, 45.0f, 90.0f,
                        {0.0f, 0.3f, 0.9f, 1.0f});

        // ---- arc #3: 完整圆 (360度，红色) ----
        // 圆心 (520,120)，半径 70，起始角度任意
        cmds->DrawArc2D({520.0f, 120.0f}, 70.0f, 0.0f, 360.0f,
                        {0.9f, 0.2f, 0.2f, 1.0f});

        // ---- arc #4: 负角度扇形（顺时针，黄色半透明） ----
        // 圆心 (160,280)，半径 90，从 0 到 -120 度（顺时针）
        cmds->DrawArc2D({160.0f, 280.0f}, 90.0f, 0.0f, -120.0f,
                        {0.9f, 0.8f, 0.1f, 0.8f});

        // ---- arc #5: 小扇形 (60度，青色) ----
        // 圆心 (380,280)，半径 70，从 30 到 90 度
        cmds->DrawArc2D({380.0f, 280.0f}, 70.0f, 30.0f, 60.0f,
                        {0.0f, 0.8f, 0.8f, 1.0f});

        // ---- arc #6: 大扇形 (270度，紫色) ----
        // 圆心 (530,280)，半径 60，从 0 到 270 度
        cmds->DrawArc2D({530.0f, 280.0f}, 60.0f, 0.0f, 270.0f,
                        {0.7f, 0.2f, 0.8f, 1.0f});
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/arc2d_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Arc2D Gold Generator";
    cfg.headless = true;
    Arc2dGoldGenerator app(cfg);
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
