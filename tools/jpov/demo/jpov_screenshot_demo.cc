// JPOV Screenshot Demo — 单帧绘制后保存 PNG 截图
//
// 使用 RunOnce 接口：无窗口环境（如 CI）下直接输出 PNG。
//
// 绘制内容：
//   1. 蓝色矩形（不透明），居中，大小为分辨率的 1/2
//   2. 红色矩形（50% alpha），覆盖蓝色矩形的左上 1/4
//
// 输出：ai_infra/output/jpov_screenshot_demo/frame_0000.png (1280x720)
//
// 编译运行：
//   DISPLAY=<display> bazel run //tools/jpov:jpov_screenshot_demo

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class ScreenshotDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // 声明渲染分辨率 640x360
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->render_width  = static_cast<int>(kResW);
        cmds->render_height = static_cast<int>(kResH);

        // ---- 矩形1：蓝色，居中，大小为分辨率的 1/2 ----
        float rect1_w = kResW * 0.5f;
        float rect1_h = kResH * 0.5f;
        float rect1_x = (kResW - rect1_w) * 0.5f;
        float rect1_y = (kResH - rect1_h) * 0.5f;
        cmds->DrawRect({rect1_x, rect1_y}, {rect1_w, rect1_h}, jpov::kColorBlue);

        // ---- 矩形2：红色 50% alpha，覆盖蓝色矩形的左上 1/4 ----
        float rect2_w = rect1_w;
        float rect2_h = rect1_h;
        jpov::Color red_alpha = {1.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect({0.0f, 0.0f}, {rect2_w, rect2_h}, red_alpha);
    }
};

int main() {
    std::string outdir = jpov::GetOutputDir() + "jpov_screenshot_demo/";
    std::string outpath = outdir + "frame_0000.png";

    // 模拟窗口尺寸 1280x720
    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;

    // 模拟空输入
    jpov::InputSnapshot input{};

    // 单帧绘制 + 截图输出（无需窗口）
    ScreenshotDemo app(JPOV::Config{});
    app.RunOnce(input, winfo, outpath.c_str());

    LOG(INFO) << "Done: " << outpath;
    return 0;
}
