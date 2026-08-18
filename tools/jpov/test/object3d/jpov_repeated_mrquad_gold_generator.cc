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

    void SetTextureIds(uint32_t base, uint32_t normal) {
        tex_base_color_ = base; tex_normal_ = normal;
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

        // 相机：同 pbr_cube_normal_mr 风格，3/4 视角看平放 quad
        cmds->camera.position = {5.5f, 4.0f, 5.5f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 光照与 pbr_cube_normal_mr 完全一致：三光源对称 +X/+Y/+Z
        // （位置 2,0,0 / 0,2,0 / 0,0,2，线性半径 6.0，color 3，physical 0.5）
        cmds->point_lights.push_back({{2.0f, 0.0f, 0.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 2.0f, 0.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 0.0f, 2.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});

        // 平放 quad 10×10，UV 0~5（重复 5 次 → 每次周期覆盖 2m）
        jpov::MeshData mesh = BuildGroundQuad(5.0f, 5.0f, 5.0f);
        uint32_t mesh_id = RegisterMesh(mesh);

        // 材质 = 砖墙 baseColor + sobel 法线；砖为无非金属、偏糙（matte）
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.metallic = 0.0f;
        mat.roughness = 0.9f;
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 2.0f;

        cmds->DrawObject3D(mesh_id, mat,
                           {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});
    }

private:
    uint32_t tex_base_color_ = 0, tex_normal_ = 0;
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
    // 砖墙 baseColor（seamless 周期化）+ sobel 法线；砖为 matte 非金属，MR 用纯参数。
    // repeat=true：GL_REPEAT 平铺（UV 0~5 → 重复 5 次，否则 CLAMP 拉边缘彩带）。
    // mipmap=true：三线性，大透视平铺面防摩尔纹。
    jpov::TextureOptions tex_opt;
    tex_opt.repeat = true;
    tex_opt.mipmap = true;
    uint32_t base   = app.RegisterTexture(root + "brick_seamless_512x512.png", tex_opt);
    uint32_t normal = app.RegisterTexture(root + "brick_normal_512x512.png", tex_opt);
    LOG(INFO) << "textures: base=" << base << " normal=" << normal;
    app.SetTextureIds(base, normal);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Repeated (flat) MR-quad gold image generated: " << outpath;
    return 0;
}
