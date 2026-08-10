// JPOV Gold Image Generator — PBR 自发光（AO map TBN）
//
// 生成 jpov_pbr_normal_gold_test 的参考图片：
//   - 从 OBJ 加载带 UV+normal 的立方体（cube_hand.obj），
//     OBJ 加载器已自动从三角形几何 + UV 推导逐顶点 tangent（kTangent 标志）
//   - 注册 baseColor 纹理 + 自发光贴图（pbr_ao_256x256.png，4 个凸起半球）
//   - PBRMaterial.normal_tex 指向自发光贴图，走 GGX PBR 光照 + TBN 法线扰动
//   - 单点光源从前上方照亮，让法线扰动的明暗变化清晰可见
//   - Camera (1.9, 1.3, 1.9) 看向立方体中心
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/pbr_ao_cube_1280x720.png
// 用途: 自发光 TBN 链路验证 + 供 leader 肉眼判断材质观感。

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class PbrAoGoldGenerator : public JPOV {
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

        // 单点光源：前上方暖色主光，让法线扰动产生清晰明暗
        // 自发光贴图展示灯（grazing key + 正面 fill，凸显凹凸细节）
        // key: 右上方主光，产生 grazing 高光
        cmds->point_lights.push_back({
            {2.0f, 1.8f, 0.5f},
            {35.0f, 32.0f, 28.0f, 1.0f},
            8.0f
        });
        // fill: 正面稍低，降低对比度
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 1.5f},
            {12.0f, 14.0f, 22.0f, 1.0f},
            6.0f
        });
        // rim: 左侧 grazing，产生边缘高光
        cmds->point_lights.push_back({
            {-1.8f, 0.5f, 0.2f},
            {20.0f, 20.0f, 20.0f, 1.0f},
            8.0f
        });        // 加载带 UV + normal 的立方体（OBJ loader 自动推导 tangent）
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/cube_hand.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";
        // 确认 OBJ loader 已推导 kTangent（自发光 TBN 前提）
        CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent))
            << "LoadObj 未推导 kTangent（自发光需要 tangent）";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：baseColor 走纹理，叠加自发光贴图扰动
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};   // fallback（纹理优先）
        mat.metallic = 0.0f;
        mat.roughness = 0.5f;  // moderate roughness, emissive should be visible
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 2.0f;
        mat.emissive_tex = tex_ao_;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center = 原点
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

    void SetTextureIds(uint32_t base, uint32_t normal, uint32_t emissive, uint32_t ao) {
        tex_base_color_ = base;
        tex_normal_ = normal;
        tex_ao_ = emissive;
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_normal_ = 0;
    uint32_t tex_ao_ = 0;
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/pbr_ao_cube_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "PBR AO Gold Generator";
    cfg.headless = true;
    PbrAoGoldGenerator app(cfg);
    app.Init();

    // 注册 baseColor + 自发光贴图
    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/";
    uint32_t base   = app.RegisterTexture(root + "cube_tex_256x256.png");
    uint32_t normal = app.RegisterTexture(root + "pbr_normal_256x256.png");
    uint32_t emissive = app.RegisterTexture(root + "pbr_emissive_256x256.png");
    uint32_t ao_var = app.RegisterTexture(root + "pbr_ao_256x256.png");
    LOG(INFO) << "textures registered: base=" << base << " normal=" << normal;
    app.SetTextureIds(base, normal, emissive, ao_var);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "PBR AO gold image generated: " << outpath;
    return 0;
}
