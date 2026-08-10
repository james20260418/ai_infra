// JPOV Gold Image Generator — PBR 多通道纹理渲染（metallic/roughness/emissive/AO）
//
// 生成 jpov_pbr_textures_gold_test 的参考图片：
//   - 从 OBJ 加载带 UV+normal 的立方体（cube_hand.obj）
//   - 注册 5 个通道纹理：baseColor / metallic / roughness / emissive / AO
//   - PBRMaterial 各 *_tex 指向对应纹理，走 GGX PBR 光照 + 逐像素纹理采样
//     （metallic/roughness/AO 采样 .r 通道；emissive 采样 .rgb）
//   - Camera (1.9, 1.3, 1.9) 看向立方体中心
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/pbr_textures_cube_1280x720.png
// 用途: 材质通道纹理链路验证 + 供 leader 肉眼判断材质观感。

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class PbrTexturesGoldGenerator : public JPOV {
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

        // Camera 从右前上方观察立方体中心（同 test 保持一致）
        cmds->camera.position = {1.9f, 1.3f, 1.9f};
        cmds->camera.target   = {0.5f, 0.5f, 0.5f};

        // 点光源：暖色主光（上方偏前，近且强）+ 冷色补光
        cmds->point_lights.push_back({
            {0.5f, 2.4f, 0.5f},          // position（立方体正上方稍近）
            {1.0f, 0.95f, 0.85f, 1.0f},  // color（暖白）
            3.5f                          // linear_radius
        });
        cmds->point_lights.push_back({
            {-0.3f, 0.5f, -0.6f},        // position（左前下方）
            {0.55f, 0.6f, 1.0f, 1.0f},   // color（冷蓝，稍强）
            3.0f                          // linear_radius
        });

        // 加载带 UV + normal 的立方体
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/cube_hand.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：5 个通道全部走纹理采样（逐像素材质参数场）
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};   // fallback（纹理优先）
        mat.has_metallic_tex = true;
        mat.metallic_tex = tex_metallic_;
        mat.metallic = 0.0f;                          // fallback
        mat.has_roughness_tex = true;
        mat.roughness_tex = tex_roughness_;
        mat.roughness = 1.0f;                         // fallback
        mat.emissive_tex = tex_emissive_;
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};      // fallback
        mat.ao_tex = tex_ao_;
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};            // fallback
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.5f, 0.5f, 0.5f},           // center
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

    void SetTextureIds(uint32_t base, uint32_t met, uint32_t rough,
                       uint32_t emissive, uint32_t ao) {
        tex_base_color_ = base;
        tex_metallic_ = met;
        tex_roughness_ = rough;
        tex_emissive_ = emissive;
        tex_ao_ = ao;
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_metallic_ = 0;
    uint32_t tex_roughness_ = 0;
    uint32_t tex_emissive_ = 0;
    uint32_t tex_ao_ = 0;
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/pbr_textures_cube_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "PBR Textures Gold Generator";
    cfg.headless = true;
    PbrTexturesGoldGenerator app(cfg);
    app.Init();

    // 注册 5 个通道纹理
    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/";
    uint32_t base  = app.RegisterTexture(root + "cube_tex_256x256.png");
    uint32_t met   = app.RegisterTexture(root + "pbr_metallic_256x256.png");
    uint32_t rough = app.RegisterTexture(root + "pbr_roughness_256x256.png");
    uint32_t emiss = app.RegisterTexture(root + "pbr_emissive_256x256.png");
    uint32_t ao    = app.RegisterTexture(root + "pbr_ao_256x256.png");
    LOG(INFO) << "textures registered: base=" << base << " metallic=" << met
              << " roughness=" << rough << " emissive=" << emiss << " ao=" << ao;
    app.SetTextureIds(base, met, rough, emiss, ao);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "PBR textures gold image generated: " << outpath;
    return 0;
}
