// JPOV 透明绘制 gold test —— 验证 AlphaMode::kBlend（加权混合 OIT）+ 体积雾联编的
// 「透明盒前后遮挡」经典用例（见 jpov_transparent_gold_common.h）：
//   玻璃前脸 → 染红盒；玻璃后脸 → 被红盒遮挡而丢弃；盒外红盒 → 对照。
//
// 门禁：渲染链路跑通 + 8x8 ROI 平均色与 gold 的 max-channel-mean-diff ≤ 阈值。
// gold image 由 jpov_transparent_gold_generator 生成（缺失=回归）。

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/object3d/jpov_transparent_gold_common.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_transparent_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/transparent_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/object3d/transparent_1280x720.png";
}

void RegisterSceneMeshes(jpov_transparent::TransparentApp* app) {
    app->ground_mesh_ = app->RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 4.0f, /*up*/ 0.1f, /*left*/ 4.0f));
    app->glass_mesh_ = app->RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 1.0f, /*up*/ 1.0f, /*left*/ 1.0f));
    app->solid_mesh_ = app->RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 0.4f, /*up*/ 0.4f, /*left*/ 0.4f));
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 gold image 存在（generator 生成；缺失=回归）。
    const std::string gold_path = GetGoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_transparent_gold_generator: " << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold_path;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Transparent Test";
    cfg.headless = true;
    jpov_transparent::TransparentApp app(cfg);
    app.Init();
    RegisterSceneMeshes(&app);

    jpov::WindowInfo winfo;
    winfo.width = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // smoke check: 效果图非空。
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;
    int nz = 0;
    for (int i = 0; i < w * h; ++i) {
        if (px[i * 4 + 3] > 0) ++nz;
    }
    stbi_image_free(px);
    LOG(INFO) << "Rendered: " << w << "x" << h
              << " coverage=" << (100.0f * nz / (w * h)) << "%";
    CHECK_GT(nz, 0) << "空场景";

    // 光照平均颜色对比（8×8 ROI 块内平均色）：透明叠加会放大误差，
    // 阈值给宽松些（这是「透明链路跑通 + 与 gold 一致」的回归门禁）。
    const double kLightThreshold = 30.0;
    const double max_diff =
        jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
    LOG(INFO) << "LIGHT COMPARE: gold vs rendered max-channel-mean-diff "
              << "(tile 8x8) = " << max_diff
              << " (threshold=" << kLightThreshold << ")";
    if (max_diff < 0) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold 或 rendered 读取/尺寸错误）";
        return 1;
    }
    if (max_diff > kLightThreshold) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: max-channel-mean-diff=" << max_diff
                   << " > threshold=" << kLightThreshold << " → 透明绘制回归！";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: 透明绘制链路跑通且与 gold 一致 (gold 见 "
              << "transparent_1280x720.png)";
    return 0;
}
