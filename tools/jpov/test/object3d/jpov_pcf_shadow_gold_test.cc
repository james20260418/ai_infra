// JPOV 阴影 PCF 采样核 gold test
//
// 保护「阴影 PCF 采样核」：渲染「PCF 对比」场景（天光查看器同款：地面 + 三方块 +
// 桌子 + 橡树 + 三个蓝人，机位/太阳见 jpov_pcf_shadow_common.h），采样核用
// **软化态**（黄金角螺旋 N=25 / R=4 ≈ 9 纹素），与 gold
// `pcf_golden_spiral_1280x720.png` 比对。
//
// 通过条件（双闸门）：
//   1) 粗粒度：8×8 ROI 均值最大通道差 <= 25（对齐其它光照 gold 的龙头阈值，
//      容忍 llvmpipe 整块明暗漂移）。
//   2) 细粒度：逐像素「显著变化像素」占比 <= 0.05%（单像素通道差阈值 24）。
//      细粒度闸门是**保护黄金角螺旋**的关键：ROI 均值会把只在影缘出现的差异摊平
//      （实测 ≤0.5），而若 PCF 退回固定 3×3（硬边），影缘几十像素宽会显著改变，
//      占比远超 0.05% → 失败（本地负向验证过：3×3 时 ~0.5%）。
//
// 注：llvmpipe 逐像素对**不同渲染状态**可能离散，故细粒度闸门只用于「同核同环境」
// 回归；gold 与 rendered 均由同一 generator/test 通路产出，环境一致时差异≈0。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/object3d/jpov_pcf_shadow_common.h"
#include "tools/jpov/test/compare_light.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_pcf_shadow_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/pcf_golden_spiral_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/pcf_golden_spiral_1280x720.png";
}

// 粗闸门（ROI 均值）。
constexpr double kLightMeanThreshold = 25.0;
// 细闸门（逐像素）：单像素「显著变化」通道差阈值 + 显著变化像素占比上限。
constexpr int    kPixelDiffThreshold = 24;
constexpr double kMaxChangedRatio    = 0.0005;   // 0.05%

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    const std::string gold_path = GetGoldPath();
    {
        FILE* f = std::fopen(gold_path.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_pcf_shadow_gold_generator: " << gold_path;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold_path;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV PCF Shadow Test";
    cfg.headless = true;
    jpov_pcf_shadow::PcfShadowApp app(cfg);
    app.Init();
    jpov_pcf_shadow::AssembleScene(app);

    // 渲染态 = gold 态：黄金角螺旋 N=25 / R=4（软化态）。
    app.SetShadowPcf(jpov::ShadowPcfConfig{
        /*mode*/ jpov::ShadowPcfConfig::Mode::kGoldenSpiral,
        /*tap_count*/ 25, /*radius_texels*/ 4.0f});

    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 输出非空校验。
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
    CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath;
    LOG(INFO) << "Rendered: " << w << "x" << h;
    int nz = 0;
    for (int i = 0; i < w * h; ++i) {
        if (px[i * 4 + 3] > 0) ++nz;
    }
    stbi_image_free(px);
    CHECK_GT(nz, 0) << "空场景（无任何不透明像素）";

    int rc = 0;

    // ---- 闸门 1：粗粒度 ROI 均值 ----
    const double mean_diff = jpov::CompareLightMeanRoiPng(gold_path, outpath, 8, 8);
    LOG(INFO) << "LIGHT COMPARE: max-channel-mean-diff (tile 8x8) = " << mean_diff
              << " (threshold=" << kLightMeanThreshold << ")";
    if (mean_diff < 0) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: 无法比对（读取/尺寸错误）";
        return 1;
    }
    if (mean_diff > kLightMeanThreshold) {
        LOG(ERROR) << "LIGHT COMPARE FAILED: " << mean_diff
                   << " > " << kLightMeanThreshold << " → 光照回归！";
        rc = 1;
    }

    // ---- 闸门 2：细粒度逐像素（保护黄金角螺旋软化）----
    const jpov::PixelChangeReport pr =
        jpov::ComparePixelChangePng(gold_path, outpath, kPixelDiffThreshold);
    LOG(INFO) << "PIXEL COMPARE: changed-ratio = " << pr.changed_ratio
              << " (" << (pr.changed_ratio * 100.0) << "%)"
              << " max_diff=" << pr.max_diff << " p99.9=" << pr.p999_diff
              << " (threshold=" << kMaxChangedRatio << ")";
    if (pr.opaque == 0) {
        LOG(ERROR) << "PIXEL COMPARE FAILED: 无可比对像素";
        rc = 1;
    } else if (pr.changed_ratio > kMaxChangedRatio) {
        LOG(ERROR) << "PIXEL COMPARE FAILED: 显著变化像素占比 "
                   << (pr.changed_ratio * 100.0) << "% > "
                   << (kMaxChangedRatio * 100.0)
                   << "% → PCF 采样核回归（疑似退回硬边 3×3 / 黄金角螺旋被改坏）！";
        rc = 1;
    } else {
        LOG(INFO) << "PIXEL COMPARE PASSED: 影缘软化特征与 gold 一致";
    }

    if (rc != 0) {
        return rc;
    }
    LOG(INFO) << "TEST PASSED: 阴影 PCF 黄金角螺旋采样与 gold 一致 "
                 "(gold 见 pcf_golden_spiral_1280x720.png)";
    return 0;
}
