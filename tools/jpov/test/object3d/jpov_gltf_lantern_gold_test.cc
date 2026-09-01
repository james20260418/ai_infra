// JPOV glTF Lantern (GLB) PBR Gold Image Test
//
// 用 Khronos 官方 Lantern.glb（贴图内嵌单文件 GLB）端到端验证：
//   1. JPOV::LoadGltf(path) → GltfObject（GLB 内嵌 bufferView 贴图自动解出）
//   2. RenderCommandList::DrawGltfObject(obj, ...) 渲染整个对象
//   3. JPOV::ReleaseGltf(obj) 整体释放
//
// 覆盖的加载链路（相对 pliers 的外部贴图 .gltf，本测试主打内嵌贴图 GLB）：
//   - GLB 单文件内嵌贴图（baseColor/normal/metallicRoughness/emissive，均
//     bufferView 内嵌、无外部 uri）
//   - ORM 拆包（metallic/roughness）
//   - AO 来源规范：Lantern 无独立 occlusionTexture → ao 应为 1（无遮蔽），
//     不把 ORM.R 当 AO（避免环境光被误灭导致模型黑）。
//   - emissive 贴图加载（Lantern emissiveFactor=白，但 emissive 贴图基本黑，
//     正确应采样贴图而非用白常值，否则模型被冲到发白）。
//
// 测试通过条件：渲染链路跑通并输出非平凡效果图（参照 pliers/cube_normal test
// 的 leader #16 决策：光照平均色比对 + leader 肉眼判断效果）。

#include <cstdint>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/compare_light.h"

namespace {

std::string GetGltfPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/lantern_glb/Lantern.glb";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/lantern_glb/Lantern.glb";
}

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_gltf_lantern_test/";
}

// 光照对比用的 gold 图（generator 输出）。
std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/lantern_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/lantern_1280x720.png";
}

}  // namespace

// ============ 测试应用 ============

class GltfLanternTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetGltfObject(jpov::GltfObject obj) { gltf_ = std::move(obj); }

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

        // 相机与 generator 一致（适配 lantern 大包围盒）。
        cmds->camera.position = {0.0f, 18.0f, 30.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.near     = 0.01f;

        // 三光源对称照明（同 generator）
        cmds->point_lights.push_back({
            {8.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 8.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 8.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f});

        cmds->ambient = jpov::AmbientLight{
            .color = {1, 1, 1, 1}, .intensity = 0.5f};

        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

private:
    jpov::GltfObject gltf_;
};

// ============ 测试入口 ============

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "glTF Lantern (GLB) PBR Test";
    cfg.headless = true;
    GltfLanternTestApp app(cfg);
    app.Init();

    jpov::GltfObject gltf = app.LoadGltf(GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    CHECK_GT(gltf.size(), 0u) << "Lantern.glb 应至少有一个 primitive";
    LOG(INFO) << "Loaded " << gltf.size() << " glTF primitives";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // Smoke check：效果图存在且非空
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                           &rnd_w, &rnd_h, &rnd_comp, 4);
    CHECK(rnd_pixels != nullptr)
        << "Failed to load rendered PNG: " << outpath
        << " (" << stbi_failure_reason() << ")";
    LOG(INFO) << "Rendered effect image: " << rnd_w << "x" << rnd_h;

    CHECK_GT(rnd_w, 0);
    CHECK_GT(rnd_h, 0);

    const int total_pixels = rnd_w * rnd_h;
    int non_transparent = 0;
    int max_r = 0, max_g = 0, max_b = 0;
    for (int i = 0; i < total_pixels; ++i) {
        if (rnd_pixels[i * 4 + 3] > 0) ++non_transparent;
        if (rnd_pixels[i * 4 + 0] > max_r) max_r = rnd_pixels[i * 4 + 0];
        if (rnd_pixels[i * 4 + 1] > max_g) max_g = rnd_pixels[i * 4 + 1];
        if (rnd_pixels[i * 4 + 2] > max_b) max_b = rnd_pixels[i * 4 + 2];
    }
    stbi_image_free(rnd_pixels);

    const float coverage = static_cast<float>(non_transparent) / total_pixels;
    LOG(INFO) << "Non-transparent coverage=" << (coverage * 100.0f)
              << "%, max RGB=(" << max_r << "," << max_g << "," << max_b << ")";

    CHECK_GT(non_transparent, 0)
        << "Rendered image is entirely empty (no object drawn)";

    // ---- 光照平均颜色对比（同 pliers test 的 Danis 决策）----
    const double kLightThreshold = 55.0;
    const std::string gold_path = GetGoldPath();
    if (FILE* f = std::fopen(gold_path.c_str(), "rb")) {
        std::fclose(f);
        const double max_diff =
            jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
        LOG(INFO) << "LIGHT COMPARE: gold vs rendered "
                  << "max-channel-mean-diff (tile 8x8) = " << max_diff
                  << " (threshold=" << kLightThreshold << ")";
        if (max_diff < 0) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对";
            return 1;
        }
        if (max_diff > kLightThreshold) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: max-channel-mean-diff="
                       << max_diff << " > threshold=" << kLightThreshold;
            return 1;
        }
        LOG(INFO) << "LIGHT COMPARE PASSED: max-channel-mean-diff="
                  << max_diff << " <= threshold=" << kLightThreshold;
    }

    LOG(INFO) << "TEST PASSED: glTF Lantern (GLB) PBR render pipeline ran "
              << "and produced a non-trivial effect image";
    return 0;
}
