// JPOV Fire-Fog froxel gold image test
//
// 用固定场景渲染（见 fire_fog_gold_common.h），与仓库 gold
//   tools/jpov/test/fire_fog/fire_fog_godray_1280x720.png
// 比对，验证 froxel 体积雾（inject/scatter/composite）+ CSM god ray 链路没退化。
//
// 比对方式：与其它光照 gold 一致，用 **8×8 平铺 ROI 的块内平均色** 比对
//（llvmpipe 软渲染下逐像素不可靠，见 compare_light.h），门禁 = 最大通道均值差。
//
// 运行：
//   bazel test //tools/jpov/test/fire_fog:jpov_fire_fog_gold_test

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/fire_fog/fire_fog_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string GoldPath() {
    return jpov::GetTestDataDir() + "/fire_fog/" +
           jpov_fire_fog_gold::kGoldPngName;
}

}  // namespace

int main() {
    const std::string gold_path = GoldPath();

    // 0. gold 必须存在（缺失=回归）。
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr)
            << "gold image 缺失，请先跑 jpov_fire_fog_gold_generator: " << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold_path;
    }

    // 1. 渲染到 output。
    const std::string outdir = jpov::GetOutputDir() + "jpov_fire_fog_gold_test/";
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Fire-Fog Gold Test";
    cfg.headless = true;
    jpov_fire_fog_gold::FireFogGoldApp app(cfg);
    app.Init();
    app.Install();

    jpov::WindowInfo winfo;
    winfo.width  = static_cast<float>(jpov_fire_fog_gold::kResW);
    winfo.height = static_cast<float>(jpov_fire_fog_gold::kResH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 2. smoke：渲染非空。
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;
    LOG(INFO) << "Rendered: " << w << "x" << h;
    int nz = 0;
    for (int i = 0; i < w * h; ++i) {
        if (px[i * 4 + 3] > 0) {
            ++nz;
        }
    }
    stbi_image_free(px);
    CHECK_GT(nz, 0) << "空渲染";
    LOG(INFO) << "coverage=" << (100.0f * static_cast<float>(nz) /
                                 static_cast<float>(w * h)) << "%";

    // 3. 光照平均色比对（8×8 ROI）。
    constexpr double kThreshold = 25.0;
    const double max_diff =
        jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
    LOG(INFO) << "LIGHT COMPARE: max-channel-mean-diff (tile 8x8) = " << max_diff
              << " (threshold=" << kThreshold << ")";
    if (max_diff < 0.0) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（gold/rendered 读取或尺寸错误）";
        return 1;
    }
    if (max_diff > kThreshold) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: max-channel-mean-diff=" << max_diff
                   << " > threshold=" << kThreshold << " → fire_fog 回归！";
        return 1;
    }

    LOG(INFO) << "TEST PASSED: fire_fog gold 匹配 (max-channel-mean-diff="
              << max_diff << ")";
    return 0;
}
