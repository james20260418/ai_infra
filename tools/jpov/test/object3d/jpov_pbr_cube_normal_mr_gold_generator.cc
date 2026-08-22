// JPOV Gold Image Generator — PBR Cube Normal + MR 纹理映射
//
// 生成 jpov_pbr_normal_gold_test 的参考图片：
//   - 从 OBJ 加载带 UV+normal 的立方体（cube_hand.obj），
//     OBJ 加载器已自动从三角形几何 + UV 推导逐顶点 tangent（kTangent 标志）
//   - 注册 baseColor 纹理 + 法线贴图（pbr_normal_256x256.png，4 个凸起半球）
//   - PBRMaterial.normal_tex 指向法线贴图，走 GGX PBR 光照 + TBN 法线扰动
//   - 单点光源从前上方照亮，让法线扰动的明暗变化清晰可见
//   - Camera (1.9, 1.3, 1.9) 看向立方体中心
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/object3d/pbr_cube_normal_mr_1280x720.png
// 用途: 法线映射 TBN 链路验证 + 供 leader 肉眼判断材质观感。

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

class PbrCubeNormalMRGenerator : public JPOV {
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

        // Camera 从右前上方观察立方体中心（同其它 PBR test 保持一致）
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // 三光源对称：+X, +Y, +Z 各一个，等强度
        cmds->point_lights.push_back({
            {2.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            6.0f,
            0.5f,
            2.0f
        });
        cmds->point_lights.push_back({
            {0.0f, 2.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            6.0f,
            0.5f,
            2.0f
        });
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 2.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            6.0f,
            0.5f,
            2.0f
        });

        // Ambient: 适中
        cmds->ambient= jpov::AmbientLight{
            .color = {1,1,1,1},
            .intensity = 0.5f
        };
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/object3d/cube_hand.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";
        // 确认 OBJ loader 已推导 kTangent（法线映射 TBN 前提）
        CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent))
            << "LoadObj 未推导 kTangent（法线映射需要 tangent）";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：baseColor 走纹理，叠加法线贴图扰动
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};   // fallback（纹理优先）
        mat.metallic = 0.0f;
        mat.roughness = 0.35f;
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 2.0f;

        mat.has_metallic_tex = true;
        mat.metallic_tex = tex_metallic_;
        mat.has_roughness_tex = true;
        mat.roughness_tex = tex_roughness_;

        // metallic / roughness 纹理
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center = 原点
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

    void SetTextureIds(uint32_t base, uint32_t normal, uint32_t metallic, uint32_t roughness) {
        tex_base_color_ = base;
        tex_normal_ = normal;
        tex_metallic_ = metallic;
        tex_roughness_ = roughness;
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_normal_ = 0;
    uint32_t tex_metallic_ = 0;
    uint32_t tex_roughness_ = 0;
};

int main() {
    std::string outpath =
        jpov::GetTestDataDir() + "/object3d/pbr_cube_normal_mr_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "PBR Cube Normal + MR Gold Generator";
    cfg.headless = true;
    PbrCubeNormalMRGenerator app(cfg);
    app.Init();

    // 注册 baseColor + 法线贴图
    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/object3d/";
    uint32_t base   = app.RegisterTexture(root + "cube_tex_256x256.png");
    uint32_t normal = app.RegisterTexture(root + "pbr_normal_256x256.png");
    uint32_t metallic  = app.RegisterTexture(root + "pbr_metallic_256x256.png");
    uint32_t roughness = app.RegisterTexture(root + "pbr_roughness_256x256.png");
    LOG(INFO) << "textures: base=" << base << " normal=" << normal << " metallic=" << metallic << " roughness=" << roughness;
    app.SetTextureIds(base, normal, metallic, roughness);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "PBR normal gold image generated: " << outpath;
    return 0;
}
