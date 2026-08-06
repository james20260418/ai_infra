// JPOV Gold Image Generator — 3D 点光源光照渲染 gold image
//
// 生成 lighting gold test 的参考图片：
//   - 从 OBJ 文件加载 beetle 模型 → LoadObj → MeshData → RegisterMesh
//   - 3 个点光源（红/绿/蓝）从不同方向照射
//   - Blinn-Phong 光照（diffuse + specular + ambient）
//   - Camera (1, 1, 1) 看向原点，透视投影
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/lighting_beetle_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class LightingGoldGenerator : public JPOV {
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

        // ---- 设置点光源（3 个，红/绿/蓝，从不同方向照射）----
        // 红色光源：从上方照亮
        cmds->point_lights.push_back({
            {0.0f, 2.0f, 0.0f},         // position
            {1.0f, 0.3f, 0.2f, 1.0f},  // color (偏暖红)
            3.0f                         // linear_radius
        });
        // 绿色光源：从左前方照亮
        cmds->point_lights.push_back({
            {-1.5f, 0.5f, 0.8f},        // position
            {0.2f, 0.9f, 0.3f, 1.0f},  // color (翠绿)
            2.5f                         // linear_radius
        });
        // 蓝色光源：从右后方照亮（产生轮廓光/高光）
        cmds->point_lights.push_back({
            {1.2f, 0.3f, -0.5f},        // position
            {0.2f, 0.4f, 1.0f, 1.0f},  // color (偏蓝)
            2.5f                         // linear_radius
        });

        // ---- 加载 OBJ 模型 → 注册 GPU mesh ----
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/beetle.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load beetle.obj";

        uint32_t mesh_id = RegisterMesh(mesh);
        uint32_t texture_id = 0;  // 纯色 + 光照渲染

        // ---- 渲染模型（中心在原点，自然朝向，默认走光照着色）----
        cmds->DrawObject3D(
            mesh_id, texture_id,
            {0.8f, 0.8f, 0.8f, 1.0f},  // default_color (漫反射基础色，偏白)
            {0.0f, 0.0f, 0.0f},          // center = 原点
            {0.0f, 1.0f, 0.0f},          // up = +Y
            {0.0f, 0.0f, 1.0f});         // front = +Z
    }
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/lighting_beetle_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "3D Lighting Gold Generator";
    cfg.headless = true;
    LightingGoldGenerator app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Lighting gold image generated: " << outpath;
    return 0;
}
