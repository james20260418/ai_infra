// JPOV Rect Demo — 验证 FBO → 窗口坐标映射
//
// 在窗口坐标空间绘制一个实心矩形。
// Rect 的 pos/size 以窗口像素为单位，用于验证 2D 坐标在 FBO 后端中
// 是否正确映射到窗口显示。
//
// 编译运行：
//   bazel run //tools/jpov:jpov_demo_rect

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "tools/jpov/include/jpov/jpov.h"

class RectApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;

        // 在窗口中央画一个矩形
        // 窗口尺寸 winfo.width x winfo.height，坐标以窗口像素为单位
        float rw = 300.0f;
        float rh = 200.0f;
        float rx = (winfo.width - rw) * 0.5f;
        float ry = (winfo.height - rh) * 0.5f;

        jpov::Color color = jpov::kColorBlue;
        cmds->DrawRect({rx, ry}, {rw, rh}, color);
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title    = "JPOV — Rect Demo (窗口坐标)";
    cfg.width    = 1280;
    cfg.height   = 720;
    cfg.target_fps = 30;
    RectApp app(cfg);
    app.Run();
    return 0;
}
