// JPOV Gold Image Generator — 3D 环状条带 gold image
//
// 生成 strip3d gold test 的参考图片：
//   - Camera (1,1,1) 看向原点
//   - Strip3DCommand 构造的环状条带，半径 0.5，宽度 0.2，中心原点，不封口
//   - 纯色渲染，无光照
//   - 渲染分辨率 1280x720（主 FBO 的 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/strip3d_ring_1280x720.png

#include <cmath>
#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Strip3dGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // ---- 设置 Camera ----
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // ---- 构造环状条带 ----
        // 环中心在原点，半径 0.5，宽度 0.2（内径 0.4，外径 0.6）
        // 沿圆周均匀取 60 段，用 Strip3DCommand 不封口条带化渲染
        const float kRingRadius = 0.5f;
        const float kRingWidth  = 0.2f;
        const float kInnerRadius = kRingRadius - kRingWidth * 0.5f;  // 0.4
        const float kOuterRadius = kRingRadius + kRingWidth * 0.5f;  // 0.6
        const int kSegments = 60;

        std::vector<jpov::Vec3f> verts;
        verts.reserve(kSegments * 2);

        for (int i = 0; i < kSegments; ++i) {
            float angle = 2.0f * static_cast<float>(M_PI) * i / kSegments;
            float cx = std::cos(angle);
            float cz = std::sin(angle);  // z-axis, 环面在 xz 平面

            // 外点（先外后内，使条带三角形法线朝上，配合 GL_CULL_FACE(GL_BACK) + CCW）
            verts.push_back({cx * kOuterRadius, 0.0f, cz * kOuterRadius});
            // 内点
            verts.push_back({cx * kInnerRadius, 0.0f, cz * kInnerRadius});
        }

        // 闭合环：首部重复前 2 个顶点，使末段与首段连接
        verts.push_back(verts[0]);
        verts.push_back(verts[1]);

        // ---- 绘制环状条带 ----
        cmds->DrawStrip3D(verts, {0.0f, 0.6f, 1.0f, 1.0f});  // 浅蓝色
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/strip3d_ring_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "3D Strip Ring Gold Generator";
    cfg.headless = true;
    Strip3dGoldGenerator app(cfg);
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
