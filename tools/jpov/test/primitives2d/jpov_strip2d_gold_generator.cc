// JPOV Gold Image Generator — Strip2D 三角形条带
//
// 生成 strip2d gold test 参考图片：
//   - 在 640x360 窗口上用 Strip2DCommand 画一个覆盖已知区域的三角形条带
//   - 三角形条带 = Z 字形跨越窗口上半部分
//   - 纯色渲染，无 MVP 变换（直接像素坐标，主 FBO）
//
// 输出: tools/jpov/test/primitives2d/strip2d_diagonal_640x360.png

#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class Strip2dGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- 构造 2D 三角形条带 ----
        // Z 字形跨越窗口上半部分：
        // p0=(80,40)  p1=(160,200)  p2=(240,40)  p3=(320,200)  p4=(400,40)  p5=(480,200)
        //
        // 生成三角形：
        //   (p0,p1,p2), (p1,p2,p3), (p2,p3,p4), (p3,p4,p5)
        //
        // 覆盖区域：x∈[80,480], y∈[40,200]，上面覆盖了大半个窗口宽度

        std::vector<jpov::Vec2f> verts = {
            { 80.0f,  40.0f},
            {160.0f, 200.0f},
            {240.0f,  40.0f},
            {320.0f, 200.0f},
            {400.0f,  40.0f},
            {480.0f, 200.0f},
        };

        cmds->DrawStrip2D(verts, {1.0f, 0.2f, 0.2f, 1.0f});  // 红色
    }
};

int main() {
    std::string outpath = jpov::GetTestDataDir() + "/primitives2d/strip2d_diagonal_640x360.png";

    JPOV::Config cfg;
    cfg.title = "Strip2D Gold Generator";
    cfg.headless = true;
    Strip2dGoldGenerator app(cfg);
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
