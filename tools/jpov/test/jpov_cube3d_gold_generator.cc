// JPOV Gold Image Generator — 3D 正方体 gold image
//
// 生成 cube3d gold test 的参考图片：
//   - Camera (1,1,1) 看向原点
//   - 边长 0.5 的正方体，6 面颜色不同
//   - 纯色渲染，无光照
//   - 渲染分辨率 1280x720（主 FBO 的 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/cube3d_6faces_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// 正方体顶点（边长 0.5，中心在原点）
// 8 个顶点，右手系：x→右，y→上，z→后
// v0: front-bottom-left,  v1: front-bottom-right
// v2: front-top-right,    v3: front-top-left
// v4: back-bottom-left,   v5: back-bottom-right
// v6: back-top-right,     v7: back-top-left
static const jpov::Vec3f kVerts[8] = {
    {-0.25f, -0.25f, 0.25f},   // v0
    { 0.25f, -0.25f, 0.25f},   // v1
    { 0.25f,  0.25f, 0.25f},   // v2
    {-0.25f,  0.25f, 0.25f},   // v3
    {-0.25f, -0.25f, -0.25f},  // v4
    { 0.25f, -0.25f, -0.25f},  // v5
    { 0.25f,  0.25f, -0.25f},  // v6
    {-0.25f,  0.25f, -0.25f},  // v7
};

// 辅助宏：用两个三角形绘制一个矩形面
// 使用特定顶点的 CCW 顺序，确保面向外侧时法线指向外面
static void AddQuad(jpov::RenderCommandList* cmds,
                    int i0, int i1, int i2, int i3,
                    const jpov::Color& color) {
    // 两个三角形：(v0,v1,v3) + (v1,v2,v3)
    cmds->DrawTriangle3D(kVerts[i0], kVerts[i1], kVerts[i3], color);
    cmds->DrawTriangle3D(kVerts[i1], kVerts[i2], kVerts[i3], color);
}

class Cube3dGoldDemo : public JPOV {
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

        // ---- 绘制正方体 6 面 ----
        // 前面 (z=0.25): Red
        AddQuad(cmds, 0, 1, 2, 3, jpov::kColorRed);
        // 后面 (z=-0.25): Green (CCW from back view)
        AddQuad(cmds, 4, 5, 6, 7, jpov::kColorGreen);
        // 右面 (x=0.25): Blue
        AddQuad(cmds, 1, 5, 6, 2, jpov::kColorBlue);
        // 左面 (x=-0.25): Yellow (= Red+Green)
        AddQuad(cmds, 4, 0, 3, 7, {1.0f, 1.0f, 0.0f, 1.0f});
        // 上面 (y=0.25): Cyan (= Green+Blue)
        AddQuad(cmds, 3, 2, 6, 7, {0.0f, 1.0f, 1.0f, 1.0f});
        // 下面 (y=-0.25): Magenta (= Red+Blue)
        AddQuad(cmds, 4, 5, 1, 0, {1.0f, 0.0f, 1.0f, 1.0f});
    }
};

int main() {
    const char* outpath = "/james_pm/ai_infra/tools/jpov/test/cube3d_6faces_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "3D Cube Gold Generator";
    cfg.headless = true;
    Cube3dGoldDemo app(cfg);
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
