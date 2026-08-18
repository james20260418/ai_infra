// JPOV Gold Image Generator — 平放 quad + pbr_cube_normal_mr 金属材质
//
// 目标：把 pbr_cube_normal_mr 那套材质（baseColor 树皮 + 石头法线 +
// metallic/roughness 纹理，normal_scale 2.0）放到一个**手写平放 quad** 上，
// 用同样的三光源 (+X,+Y,+Z) 打光，看法线凹凸在平放面上的效果。
//
// 与 repeated_wall 的区别：
//   - repeated_wall 用顶光 (0,3,0)：光 ∥ 平放板法线，法线凹凸被洗平。
//   - 本测试用三光源 (+X,+Y,+Z)（同 pbr cube），斜向光源能照亮法线凹凸
//     （石头/树皮的沟槽在斜射下投影/受光），法线细节可观察。
//
// 平放 quad：手写 MeshData，kPosition+kNormal+kUV+kTangent，
//   XZ 平面 10×10，UV 0~1（采贴图第一段；cube_hand 的 UV 就是 u∈[0,1]）。
//   法线 +Y（朝上），tangent +X（u 沿 +X），v 沿 -Z（TBN: B=cross(N,T)）。
// 材质：与 pbr_cube_normal_mr 一致（baseColor/normal/metallic/roughness 纹理，
//   normal_scale=2.0，metallic/roughness 走纹理）。
//
// 输出: tools/jpov/test/object3d/repeated_mrquad_1280x720.png

#include <cstdio>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/common/utils.h"

namespace {

std::string GetTexPath(const char* fname) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/";
        p += fname;
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/object3d/" + fname;
}

// 构造 XZ 平面平放 quad（法线 +Y），UV 0~uv_max 平铺。
jpov::MeshData BuildGroundQuad(float half_x, float half_z, float repeat) {
    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));

    const jpov::Vec3f N(0.0f, 1.0f, 0.0f);
    const jpov::Vec3f T(1.0f, 0.0f, 0.0f);
    const float xp = half_x, xm = -half_x;
    const float zp = half_z, zm = -half_z;

    struct Vtx { jpov::Vec3f pos; jpov::Vec3f n; jpov::Vec2f uv; jpov::Vec3f t; };
    const Vtx v[4] = {
        {{ xp, 0.0f, zp}, N, {repeat, 0.0f}, T},  // A +X,+Z
        {{ xm, 0.0f, zp}, N, {0.0f, 0.0f}, T},    // B -X,+Z
        {{ xm, 0.0f, zm}, N, {0.0f, repeat}, T},  // C -X,-Z
        {{ xp, 0.0f, zm}, N, {repeat, repeat}, T},// D +X,-Z
    };
    for (int i = 0; i < 4; ++i) {
        mesh.positions.push_back(v[i].pos);
        mesh.normals.push_back(v[i].n);
        mesh.uvs.push_back(v[i].uv);
        mesh.tangents.push_back(v[i].t);
    }
    // CCW（从 +Y 看），法线 +Y
    mesh.indices = {0, 2, 1, 0, 3, 2};
    mesh.Validate();
    return mesh;
}

}  // namespace

class RepeatedMRQuadGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base, uint32_t normal,
                       uint32_t metallic, uint32_t roughness) {
        tex_base_color_ = base; tex_normal_ = normal;
        tex_metallic_ = metallic; tex_roughness_ = roughness;
    }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机：较低角度（更平视）观察平放 quad，法线扰动在斜视角下更易显现
        cmds->camera.position = {4.0f, 1.8f, 4.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 低角度掠射光（raking light）凸显平放面法线凹凸：
        // 光方向接近贴表面掠射，法线微扰引起的明暗/投影最明显。
        // 强度适中（color 3）避免过曝冲刷纹理。
        cmds->point_lights.push_back({{4.0f, 1.2f, 0.0f}, {2.2f,2.2f,2.2f,1}, 10.0f, 0.5f});
        cmds->point_lights.push_back({{-3.0f, 1.2f, -3.0f}, {1.5f,1.5f,1.5f,1}, 10.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 4.0f, 0.0f}, {1.2f,1.2f,1.2f,1}, 12.0f, 0.5f});

        // 平放 quad 10×10，UV 0~1（采贴图第一段，同 cube_hand.obj）
        jpov::MeshData mesh = BuildGroundQuad(5.0f, 5.0f, 1.0f);
        uint32_t mesh_id = RegisterMesh(mesh);

        // 材质 = pbr_cube_normal_mr 同款
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
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

        cmds->DrawObject3D(mesh_id, mat,
                           {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});
    }

private:
    uint32_t tex_base_color_ = 0, tex_normal_ = 0;
    uint32_t tex_metallic_ = 0, tex_roughness_ = 0;
};

int main() {
    const std::string outpath =
        "/james_pm/ai_infra_2/tools/jpov/test/object3d/repeated_mrquad_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "Repeated MR-Quad (flat, pbr_cube_normal_mr material) Generator";
    cfg.headless = true;
    RepeatedMRQuadGenerator app(cfg);
    app.Init();

    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/object3d/";
    // 用正常的 RegisterTexture（贴图为 768 宽 3-in-1，quad UV 0~1 采第一段）
    uint32_t base = app.RegisterTexture(root + "cube_tex_256x256.png");
    uint32_t normal = app.RegisterTexture(root + "pbr_normal_256x256.png");
    uint32_t metallic = app.RegisterTexture(root + "pbr_metallic_256x256.png");
    uint32_t roughness = app.RegisterTexture(root + "pbr_roughness_256x256.png");
    LOG(INFO) << "textures: base=" << base << " normal=" << normal
              << " metallic=" << metallic << " roughness=" << roughness;
    app.SetTextureIds(base, normal, metallic, roughness);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Repeated (flat) MR-quad gold image generated: " << outpath;
    return 0;
}
