// JPOV 太阳轨迹（sun path）gold test —— smoke check
//
// 渲染太阳轨迹配置序列，验证渲染链路跑通 + 输出非平凡效果图。PBR 光照在
// llvmpipe 下三稳态非确定，故不做逐像素颜色比对；效果正确性由 Danis 肉眼
// 查看 generator 产出的 gold image（sun_path_<index>.png）判断。本 test
// 校验 gold image 存在 + smoke check。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_sun_path_common.h"
#include "tools/jpov/test/compare_light.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_sun_path_test/";
}

// 仓库内第 index 张 gold image 路径（generator 写入）。
std::string GetGoldPath(int index) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/skydome/sun_path_"
             + std::to_string(index) + ".png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/skydome/sun_path_" + std::to_string(index) + ".png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "JPOV Sun Path Test";
    cfg.headless = true;
    jpov_sun_path::SunPathApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);

    std::vector<jpov_sun_path::SkyDirectionAmbient> path =
        jpov_sun_path::DefaultSunPath();
    app.configs_ = std::move(path);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    CHECK(!app.configs_.empty()) << "太阳轨迹配置序列为空，先往 DefaultSunPath 加配置";

    // 逐时段渲染 + smoke check。gold image 必须已由 generator 生成（缺失 = 回归）。
    for (int i = 0; i < static_cast<int>(app.configs_.size()); ++i) {
        const std::string gold = GetGoldPath(i);
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_sun_path_gold_generator: " << gold;
        std::fclose(f);

        app.active_index_ = i;
        const std::string outpath = outdir + "rendered_" + std::to_string(i) + ".png";
        app.RunOnce(input, winfo, outpath.c_str());

        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath
            << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";

        int nz = 0;
        for (int p = 0; p < w * h; ++p) if (px[p * 4 + 3] > 0) ++nz;
        stbi_image_free(px);
        CHECK_GT(nz, 0) << "空场景 (index=" << i << ")";
        LOG(INFO) << "sun path [" << i << "]: rendered " << w << "x" << h
                  << ", coverage=" << (static_cast<float>(nz) / (w * h) * 100.0f) << "%";

        // ---- 光照平均颜色对比（Danis 决策）：8×8 平铺 ROI 块内平均色对比，
        //      输出最大的 RGB 通道差异，作为该时段光照 gold 的硬性门禁。
        const double kLightThreshold = 25.0;
        const double max_diff = jpov::CompareLightMeanRoiPng(gold, outpath, 8, 8);
        LOG(INFO) << "LIGHT COMPARE [" << i << "]: gold vs rendered "
                  << "max-channel-mean-diff (tile 8x8) = " << max_diff
                  << " (threshold=" << kLightThreshold << ")";
        if (max_diff < 0) {
            LOG(ERROR) << "LIGHT COMPARE FAILED [" << i
                       << "]: 无法比对（gold 或 rendered 读取/尺寸错误）";
            return 1;
        }
        if (max_diff > kLightThreshold) {
            LOG(ERROR) << "LIGHT COMPARE FAILED [" << i
                       << "]: max-channel-mean-diff=" << max_diff
                       << " > threshold=" << kLightThreshold
                       << " → 光照回归！";
            return 1;
        }
        LOG(INFO) << "LIGHT COMPARE PASSED [" << i << "]: max-channel-mean-diff="
                  << max_diff << " <= threshold=" << kLightThreshold;
    }

    app.Finalize();
    LOG(INFO) << "TEST PASSED: 太阳轨迹渲染链路跑通，共 " << app.configs_.size()
              << " 个时段 (gold 见 sun_path_<index>.png)";
    return 0;
}
