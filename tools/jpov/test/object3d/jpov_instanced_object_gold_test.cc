// JPOV 静态批量实例（DrawInstancedObject）gold image test
//
// 验证新 API RenderCommandList::DrawInstancedObject(mesh_id, material, instances)：
//   同一 mesh 按逐实例摆放(center/up/front/scale)摆 N 份 = 一次 instanced draw，
//   复用 Object3D 的 PBR + alpha cutout(MASK) + 双面 + 阴影链路。
//
// 场景见 jpov_instanced_object_gold_common.h：地面 + 太阳 + 3 份 lace_skirt 实例。
// gold 由 jpov_instanced_object_gold_generator 生成（同场景，8×8 平铺平均色比对）。
//
// 测试通过条件：渲染链路跑通并输出非平凡效果图（参照 leader #16：跳过逐像素颜色
// 校验，用 8×8 平铺平均色做光照门禁）。

#include <cstdint>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/jpov_instanced_object_gold_common.h"
#include "tools/jpov/test/compare_light.h"

namespace {

std::string GetGltfPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/assets/models/clothing/lace_skirt/"
             "lace_skirt_skinned_sample.glb";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/assets/models/clothing/lace_skirt/lace_skirt_skinned_sample.glb";
}

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_instanced_object_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/instanced_object_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/instanced_object_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Instanced Object Test";
    cfg.headless = true;
    jpov_instanced_object::InstancedObjectApp app(cfg);
    app.Init();

    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 60.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 60.0f));

    jpov::GltfObject gltf = app.LoadGltf(GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    CHECK_EQ(gltf.size(), 1u) << "lace_skirt 应为单 primitive，got " << gltf.size();
    app.cloth_mesh_ = gltf.primitives[0].mesh_id;
    app.cloth_material_ = gltf.primitives[0].material;

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // Smoke check：效果图存在且非空。
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
        << "Rendered image is entirely empty (no instance drawn)";

    // 光照平均色比对（8×8 平铺 ROI），作为 gold 硬性门禁。
    const double kLightThreshold = 55.0;
    const std::string gold_path = GetGoldPath();
    if (FILE* f = std::fopen(gold_path.c_str(), "rb")) {
        std::fclose(f);
        const double max_diff = jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
        LOG(INFO) << "LIGHT COMPARE: gold vs rendered "
                  << "max-channel-mean-diff (tile 8x8) = " << max_diff
                  << " (threshold=" << kLightThreshold << ")";
        if (max_diff < 0) {
            LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold 或 rendered 读取/尺寸错误）";
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

    LOG(INFO) << "TEST PASSED: instanced object render pipeline ran and produced "
              << "a non-trivial effect image";
    return 0;
}
