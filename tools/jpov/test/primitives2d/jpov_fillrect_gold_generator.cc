// JPOV Gold Image Generator — FillRect2D 复合矩形
//
// 生成 fillrect gold test 参考图片：
//   - 在 640x360 窗口上绘制多种 FillRect 组合
//   - 覆盖：无圆角+纯填充、带圆角+纯填充、带边框+无圆角、带边框+圆角
//   - 纯色渲染，直接像素坐标，主 FBO
//
// 输出: tools/jpov/test/primitives2d/fillrect_640x360.png

#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class FillRectGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- #1: 无圆角+纯填充（绿色矩形，border_width=0） ----
        // 等价于普通 DrawRect，验证 fill_color 正确
        cmds->DrawFillRect({30.0f, 30.0f}, {130.0f, 80.0f},
                           {0.0f, 0.8f, 0.0f, 1.0f},  // fill: green
                           {0.0f, 0.0f, 0.0f, 0.0f},  // border: transparent
                           0.0f, 0.0f);                // bw=0, r=0

        // ---- #2: 带圆角+纯填充（蓝色，border_width=0） ----
        cmds->DrawFillRect({200.0f, 30.0f}, {150.0f, 80.0f},
                           {0.0f, 0.3f, 0.9f, 1.0f},  // fill: blue
                           {0.0f, 0.0f, 0.0f, 0.0f},  // border: transparent
                           0.0f, 15.0f);               // bw=0, r=15

        // ---- #3: 无圆角+带边框（红色填充+黄色边框） ----
        cmds->DrawFillRect({30.0f, 140.0f}, {160.0f, 80.0f},
                           {0.9f, 0.2f, 0.2f, 1.0f},  // fill: red
                           {0.9f, 0.8f, 0.1f, 1.0f},  // border: yellow
                           6.0f, 0.0f);                // bw=6, r=0

        // ---- #4: 带边框+圆角（青色填充+白色边框） ----
        cmds->DrawFillRect({230.0f, 140.0f}, {160.0f, 90.0f},
                           {0.0f, 0.7f, 0.7f, 1.0f},  // fill: cyan
                           {1.0f, 1.0f, 1.0f, 1.0f},  // border: white
                           4.0f, 12.0f);               // bw=4, r=12

        // ---- #5: 正方形+大圆角+细边框（紫色填充+红色边框） ----
        cmds->DrawFillRect({70.0f, 260.0f}, {130.0f, 130.0f},
                           {0.6f, 0.2f, 0.8f, 1.0f},  // fill: purple
                           {0.9f, 0.1f, 0.1f, 1.0f},  // border: red
                           3.0f, 30.0f);               // bw=3, r=30
    }
};

int main() {
    std::string outpath = jpov::GetTestDataDir() + "/primitives2d/fillrect_640x360.png";

    JPOV::Config cfg;
    cfg.title = "FillRect Gold Generator";
    cfg.headless = true;
    FillRectGoldGenerator app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}
