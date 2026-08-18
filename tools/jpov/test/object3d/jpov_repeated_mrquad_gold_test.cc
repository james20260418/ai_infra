// JPOV Gold Image Test — 平放 quad + pbr_cube_normal_mr 金属材质
//
// 用手写平放 quad 复刻 pbr_cube_normal_mr 的材质+光照（树皮 baseColor +
// 石头法线 + metallic/roughness 纹理，三光源 +X/+Y/+Z），看法线凹凸在
// 平放面上的效果。与 repeated_wall（顶光洗平法线）对照。
//
// 测试通过条件：渲染链路跑通 + 输出非平凡效果图（leader #16 决策：不做
// 逐像素颜色比对，效果由 leader/Danis 肉眼查看 generator 产出的 gold image
// repeated_mrquad_1280x720.png）。

#include <cstdio>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

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

std::string GetOutputDir() { return jpov::GetOutputDir() + "jpov_repeated_mrquad_test/"; }

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/repeated_mrquad_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/repeated_mrquad_1280x720.png";
}

}  // namespace

// ============ 测试应用 ============

class RepeatedMRQuadTestApp : public JPOV {
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
        cmds->camera.position = {4.0f, 1.8f, 4.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 低角度掠射光（raking light）凸显平放面法线凹凸 + 顶光补充明度
        cmds->point_lights.push_back({{4.0f, 1.2f, 0.0f}, {2.2f,2.2f,2.2f,1}, 10.0f, 0.5f});
        cmds->point_lights.push_back({{-3.0f, 1.2f, -3.0f}, {1.5f,1.5f,1.5f,1}, 10.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 4.0f, 0.0f}, {1.2f,1.2f,1.2f,1}, 12.0f, 0.5f});

        // 平放 quad 10×10，UV 0~1
        jpov::MeshData mesh;
        mesh.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
        const jpov::Vec3f N(0.0f, 1.0f, 0.0f);
        const jpov::Vec3f T(1.0f, 0.0f, 0.0f);
        const float xp = 5.0f, xm = -5.0f, zp = 5.0f, zm = -5.0f;
        struct Vtx { jpov::Vec3f pos, n; jpov::Vec2f uv; jpov::Vec3f t; };
        const Vtx v[4] = {
            {{ xp,0,zp}, N, {1,0}, T}, {{ xm,0,zp}, N, {0,0}, T},
            {{ xm,0,zm}, N, {0,1}, T}, {{ xp,0,zm}, N, {1,1}, T},
        };
        for (auto& q : v) {
            mesh.positions.push_back(q.pos); mesh.normals.push_back(q.n);
            mesh.uvs.push_back(q.uv); mesh.tangents.push_back(q.t);
        }
        mesh.indices = {0, 2, 1, 0, 3, 2};
        mesh.Validate();
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

// ============ 测试入口 ============

int main() {
    const std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 gold image 存在
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_repeated_mrquad_gold_generator: " << gold;
        std::fclose(f);
    }

    JPOV::Config cfg;
    cfg.title = "Repeated MR-Quad (flat) Gold Test";
    cfg.headless = true;
    RepeatedMRQuadTestApp app(cfg);
    app.Init();

    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/object3d/";
    uint32_t base = app.RegisterTexture(root + "cube_tex_256x256.png");
    uint32_t normal = app.RegisterTexture(root + "pbr_normal_256x256.png");
    uint32_t metallic = app.RegisterTexture(root + "pbr_metallic_256x256.png");
    uint32_t roughness = app.RegisterTexture(root + "pbr_roughness_256x256.png");
    app.SetTextureIds(base, normal, metallic, roughness);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // Smoke check
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                          &rnd_w, &rnd_h, &rnd_comp, 4);
    CHECK(rnd_pixels != nullptr) << "Failed to load rendered PNG: " << outpath
        << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";

    const int total_pixels = rnd_w * rnd_h;
    int nz = 0, mr = 0, mg = 0, mb = 0;
    for (int i = 0; i < total_pixels; ++i) {
        if (rnd_pixels[i * 4 + 3] > 0) ++nz;
        if (rnd_pixels[i * 4 + 0] > mr) mr = rnd_pixels[i * 4 + 0];
        if (rnd_pixels[i * 4 + 1] > mg) mg = rnd_pixels[i * 4 + 1];
        if (rnd_pixels[i * 4 + 2] > mb) mb = rnd_pixels[i * 4 + 2];
    }
    stbi_image_free(rnd_pixels);
    float cov = static_cast<float>(nz) / total_pixels;
    LOG(INFO) << "coverage=" << (cov * 100.0f) << "%, max RGB=(" << mr << "," << mg << "," << mb << ")";
    CHECK_GT(nz, 0) << "empty render (quad not drawn)";

    LOG(INFO) << "TEST PASSED: repeated MR-quad (flat) render pipeline ran "
              << "and produced a non-trivial effect image "
              << "(color check skipped per leader #16)";
    return 0;
}
