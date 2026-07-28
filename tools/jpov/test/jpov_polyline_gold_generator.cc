// JPOV Polyline Gold Image Generator
//
// 生成 polyline 测试的 gold image：
//   - 渲染分辨率 640x360
//   - 窗口分辨率 640x360（无拉伸，便于像素精确比较）
//   - 红色折线，10px 线宽，在多个方向画出折线
//
// 输出: /james_pm/ai_infra/tools/jpov/test/polyline_zigzag_red_640x360.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

class GoldPolylineDemo : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // 渲染分辨率 640x360
        const float kResW = 640.0f;
        const float kResH = 360.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- 折线：红 色，10px 线宽，形成锯齿/之星形 ----
        // 点 1: (60, 180)  左侧中间
        // 点 2: (160, 60)  左上
        // 点 3: (320, 180) 中间
        // 点 4: (480, 300) 右下
        // 点 5: (580, 180) 右侧中间
        // 形成一条 V 形折线，覆盖水平和垂直方向及尖角
        float line_width = 10.0f;
        std::vector<jpov::Vec2f> pts = {
            { 60.0f, 180.0f },
            { 160.0f, 60.0f },
            { 320.0f, 180.0f },
            { 480.0f, 300.0f },
            { 580.0f, 180.0f },
        };
        cmds->DrawPolyline(pts, jpov::kColorRed, line_width);
    }
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/polyline_zigzag_red_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Polyline Gold Image Generator";
    cfg.headless = true;
    GoldPolylineDemo app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Polyline gold image generated: " << outpath;
    return 0;
}
