// JPOV SSR gold test（11 个固定组合，端到端）
//
// 为什么需要它：SSR 原来只有 CPU↔GPU 对拍（几何/命中格）与 tile 等价性（并发粒度），
// **没有任何整帧基线**。于是"数学等价但舍入不同"的改动（例如把 SSR 深度纹理从
// 窗口深度换成 w = 1/z）没有验收手段 —— 它既不该让画面变，也无法证明没变。
// 本测试用天光查看器的镜面场景拍 11 张（组合见 jpov_ssr_gold_common.h）：
//   5 个基础（掠射 / 1:1 / 金属磨砂 / 四段拼接 / 低太阳 + 1:3）
// + 6 个「单项能力」（三个 A 淡出 + 补洞 / 离群 / 小波），后者每个还断言
//   「开关打开 vs 全关」必须有可观测差异（min_ab_ratio），防止 gold 变成空测。
//
// 通过条件（双闸门，与仓库其它光照 gold 一致）：
//   1) 粗粒度：8×8 ROI 均值最大通道差 <= 25（容忍 llvmpipe 整块明暗漂移）
//   2) 细粒度：逐像素显著变化（单通道差 > 24）占比 <= 0.05%
// 细粒度这道闸门才是真正保护 SSR 的：命中判据、取色、模糊 LOD 任何一处走样都会让
// 倒影整块偏移/消失，占比远超 0.05%（例如把 CellHit 的厚度项写反 ≈ 大面积变化）。

#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/ssr/jpov_ssr_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

constexpr double kLightMeanThreshold = 25.0;   // 与 pcf/其它光照 gold 同档
constexpr int    kPixelDiffThreshold = 24;
// A/B 自检用低阈值：能力的效果大多是"柔和变淡"，单通道差经常只有 5~20。
constexpr int    kAbPixelDiffThreshold = 4;
constexpr double kMaxChangedRatio    = 0.0005; // 0.05%

std::string GoldPath(const jpov_ssr_gold::SsrGoldCase& c) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir != nullptr) {
        std::string p(test_srcdir);
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        return p + "__main__/tools/jpov/test/ssr/" + jpov_ssr_gold::GoldFileName(c);
    }
    return jpov::GetTestDataDir() + "/ssr/" + jpov_ssr_gold::GoldFileName(c);
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_ssr_gold_test/";
    std::system(("mkdir -p " + outdir).c_str());

    // gold 先检查存在（缺了直接报"去跑 generator"，不要先渲十分钟再说）。
    for (const jpov_ssr_gold::SsrGoldCase& c : jpov_ssr_gold::kCases) {
        const std::string gold = GoldPath(c);
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 jpov_ssr_gold_generator: " << gold;
        std::fclose(f);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV SSR Gold Test";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.Init();
    jpov_ssr_gold::AssembleScene(app);

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    int failures = 0;
    for (const jpov_ssr_gold::SsrGoldCase& c : jpov_ssr_gold::kCases) {
        const std::string gold = GoldPath(c);
        const std::string rendered = outdir + "rendered_" + c.name + ".png";

        jpov_ssr_gold::ApplyCase(app, c);
        app.RunOnce(input, winfo, rendered.c_str());

        int w = 0;
        int h = 0;
        int comp = 0;
        unsigned char* px = stbi_load(rendered.c_str(), &w, &h, &comp, 4);
        CHECK(px != nullptr) << "Failed to load rendered PNG: " << rendered;
        int opaque = 0;
        for (int i = 0; i < w * h; ++i) {
            if (px[i * 4 + 3] > 0) {
                ++opaque;
            }
        }
        stbi_image_free(px);
        CHECK_GT(opaque, 0) << "空场景（无任何不透明像素）: " << c.name;

        const double mean_diff = jpov::CompareLightMeanRoiPng(gold, rendered, 8, 8);
        const jpov::PixelChangeReport pr =
            jpov::ComparePixelChangePng(gold, rendered, kPixelDiffThreshold);
        LOG(INFO) << "[" << c.name << "] roi-mean-max=" << mean_diff
                  << " (<= " << kLightMeanThreshold << ")  changed="
                  << (pr.changed_ratio * 100.0) << "% (<= "
                  << (kMaxChangedRatio * 100.0) << "%)  max_diff=" << pr.max_diff
                  << " p99.9=" << pr.p999_diff;

        if (mean_diff < 0 || pr.opaque == 0) {
            LOG(ERROR) << "[" << c.name << "] 无法比对（读取/尺寸错误）";
            ++failures;
            continue;
        }
        if (mean_diff > kLightMeanThreshold || pr.changed_ratio > kMaxChangedRatio) {
            LOG(ERROR) << "[" << c.name << "] SSR 回归（对比 gold " << gold << "）";
            ++failures;
        }

        // 自检（2026-11-13）：对这一项「能力」组合，ON 与「六项全关」必须真的有差别。
        // 没有这条，一个把开关接反/接漏的改动也能让 gold 全绿 —— 那等于什么都没测。
        if (c.min_ab_ratio > 0.0f) {
            const std::string off_png = outdir + "off_" + c.name + ".png";
            jpov_ssr_gold::DisableAllFeatures(app);
            app.RunOnce(input, winfo, off_png.c_str());
            const jpov::PixelChangeReport ab =
                jpov::ComparePixelChangePng(rendered, off_png, kAbPixelDiffThreshold);
            LOG(INFO) << "[" << c.name << "] A/B(开 vs 全关) changed="
                      << (ab.changed_ratio * 100.0) << "% (>= "
                      << (c.min_ab_ratio * 100.0) << "% required)";
            if (ab.changed_ratio < c.min_ab_ratio) {
                LOG(ERROR) << "[" << c.name << "] 这一项能力在当前机位下几乎不起作用"
                           << "（gold 会变成空测），检查开关是否真的接上了";
                ++failures;
            }
        }
    }

    app.Finalize();
    if (failures != 0) {
        LOG(ERROR) << failures << " / " << (sizeof(jpov_ssr_gold::kCases) /
                                            sizeof(jpov_ssr_gold::kCases[0]))
                   << " 个 SSR gold 组合不一致";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: "
              << (sizeof(jpov_ssr_gold::kCases) / sizeof(jpov_ssr_gold::kCases[0]))
              << " 个 SSR gold 组合全部一致";
    return 0;
}
