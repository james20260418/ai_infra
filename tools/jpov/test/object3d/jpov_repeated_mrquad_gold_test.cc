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
#include "tools/jpov/test/compare_light.h"
#include "tools/common/utils.h"

namespace {

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
        cmds->camera.position = {5.5f, 4.0f, 5.5f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 光照与 pbr_cube_normal_mr 完全一致：三光源对称 +X/+Y/+Z
        cmds->point_lights.push_back({{2.0f, 0.0f, 0.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 2.0f, 0.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});
        cmds->point_lights.push_back({{0.0f, 0.0f, 2.0f}, {3.0f,3.0f,3.0f,1}, 6.0f, 0.5f});

        // 平放 quad 10×10，UV 0~5（重复 5 次 → 每次周期覆盖 2m）
        jpov::MeshData mesh;
        mesh.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
        const jpov::Vec3f N(0.0f, 1.0f, 0.0f);
        const jpov::Vec3f T(1.0f, 0.0f, 0.0f);
        const float xp = 5.0f, xm = -5.0f, zp = 5.0f, zm = -5.0f;
        const float rep = 5.0f;
        struct Vtx { jpov::Vec3f pos, n; jpov::Vec2f uv; jpov::Vec3f t; };
        const Vtx v[4] = {
            {{ xp,0,zp}, N, {rep,0}, T}, {{ xm,0,zp}, N, {0,0}, T},
            {{ xm,0,zm}, N, {0,rep}, T}, {{ xp,0,zm}, N, {rep,rep}, T},
        };
        for (auto& q : v) {
            mesh.positions.push_back(q.pos); mesh.normals.push_back(q.n);
            mesh.uvs.push_back(q.uv); mesh.tangents.push_back(q.t);
        }
        mesh.indices = {0, 2, 1, 0, 3, 2};
        mesh.Validate();
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
    // 砖墙 baseColor（seamless）+ sobel 法线；砖为 matte 非金属
    jpov::TextureOptions tex_opt;
    tex_opt.repeat = true;
    tex_opt.mipmap = true;
    uint32_t base   = app.RegisterTexture(root + "brick_seamless_512x512.png", tex_opt);
    uint32_t normal = app.RegisterTexture(root + "brick_normal_512x512.png", tex_opt);
    app.SetTextureIds(base, normal);

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

    // ---- 光照平均颜色对比（Danis 决策）：自动平铺 ROI，块内平均色对比，
    //      输出最大的 RGB 通道差异，作为光照 gold 的硬性门禁。
    //      基线：本地与 CI 流水线 8×8 平铺均稳定测到 max-channel-mean-diff
    //      = 17.68（跨环境可复现）。设阈值 25（留有 ~7 余量），超过即判回归。
    const double kLightThreshold = 25.0;
    const std::string gold_path = GetGoldPath();
    if (FILE* f = std::fopen(gold_path.c_str(), "rb")) {
        std::fclose(f);
        // 8×8 平铺 64 块 ROI，每块对不透明像素求 RGB 均值，输出最大通道差。
        const double max_diff =
            jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
        LOG(INFO) << "LIGHT COMPARE: repeated_mrquad gold vs rendered "
                  << "max-channel-mean-diff (tile 8x8) = " << max_diff
                  << " (threshold=" << kLightThreshold << ")";
        if (max_diff < 0) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold 或 rendered "
                          "读取/尺寸错误）";
            return 1;
        }
        if (max_diff > kLightThreshold) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: max-channel-mean-diff="
                       << max_diff << " > threshold=" << kLightThreshold
                       << " → 光照回归！";
            return 1;
        }
        LOG(INFO) << "LIGHT COMPARE PASSED: max-channel-mean-diff="
                  << max_diff << " <= threshold=" << kLightThreshold;
    } else {
        LOG(WARNING) << "gold image not found; skipped light compare: "
                     << gold_path;
    }

    LOG(INFO) << "TEST PASSED: repeated MR-quad (flat) render pipeline ran "
              << "and produced a non-trivial effect image";
    return 0;
}
