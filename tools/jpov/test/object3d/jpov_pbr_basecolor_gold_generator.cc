// JPOV Gold Image Generator — PBR baseColor 纹理渲染 gold image
//
// 生成 jpov_pbr_basecolor_gold_test 的参考图片：
//   - 从 OBJ 加载带 UV+normal 的立方体（cube_hand.obj）
//   - RegisterTexture 注册四象限彩色纹理（cube_tex_256x256.png）
//   - PBRMaterial.base_color_tex 指向纹理，走 GGX PBR 光照 + 逐像素纹理采样
//   - Camera (1,1,1) 看向原点
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/object3d/pbr_basecolor_cube_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class PbrBaseColorGoldGenerator : public JPOV {
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

        // Camera (1,1,1) 看向原点（与 cube3d gold test 一致）
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // 点光源：暖色主光（上方偏前，近且强）+ 冷色补光
        // 三点布光（key + fill + rim）用于 PBR 材质展示
        // key: 主光，右上方暖白
        cmds->point_lights.push_back({
            {1.5f, 1.5f, 0.8f},
            {25.0f, 23.0f, 20.0f, 1.0f},  // 暖白
            6.0f
        });
        // fill: 补光，左下方冷蓝，降低对比度
        cmds->point_lights.push_back({
            {-1.2f, -0.3f, -0.5f},
            {8.0f, 10.0f, 18.0f, 1.0f},   // 冷蓝
            5.0f
        });
        // rim: 背光，后方，勾勒边缘
        cmds->point_lights.push_back({
            {0.0f, 0.0f, -1.5f},
            {15.0f, 15.0f, 15.0f, 1.0f},  // 中性白
            8.0f
        });

        // 加载带 UV + normal 的立方体
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/object3d/cube_hand.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：baseColor 走纹理采样；metallic/roughness 用常值默认
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_id_;      // 纹理优先
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};  // fallback
        mat.metallic = 0.0f;
        mat.roughness = 1.0f;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center (cube centered at origin)
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

    void SetTextureId(uint32_t id) { tex_id_ = id; }

private:
    uint32_t tex_id_ = 0;
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/object3d/pbr_basecolor_cube_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "PBR baseColor Gold Generator";
    cfg.headless = true;
    PbrBaseColorGoldGenerator app(cfg);
    app.Init();

    // 注册纹理
    std::string tex_path = jpov::GetProjectRoot() +
                           "tools/jpov/test/object3d/cube_tex_256x256.png";
    uint32_t tex_id = app.RegisterTexture(tex_path);
    LOG(INFO) << "baseColor texture registered: " << tex_path << " → id=" << tex_id;
    app.SetTextureId(tex_id);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "PBR baseColor gold image generated: " << outpath;
    return 0;
}
