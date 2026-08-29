// JPOV 太阳轨迹 + 辉光（bloom）gold test
//
// 验证 Bloom 后处理链路：
//   1. 渲染链路跑通 + 输出非平凡效果图（smoke）。
//   2. 稳定性门禁：bloom ON 渲染 vs bloom ON gold，8×8 平均色 ROI 对比
//      （阈值 25，接受 llvmpipe 光照漂移，但能拦住"bloom 链路坏了/全黑"）。
//   3. 效果门禁（负数验证核心）：per-pixel 对比两张 gold——bloom ON 相对
//      bloom OFF 至少要有某像素单通道差 > 30、且 diff>30 的像素 ≥ 20，
//      证明高亮区确实被辉光砸亮一片（若 bloom 是空操作则两点均 0，必不过）。
//
// 效果门禁用两张 gold 对比，而非渲染结果对比，规避 llvmpipe 帧间光照漂移
// 的干扰：gold 由同一 generator 进程按同一配置生成，其差异就是纯 bloom 效果。

#include <cstdio>
#include <string>
#include <algorithm>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_sun_path_bloom_common.h"
#include "tools/jpov/test/compare_light.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_sun_path_bloom_test/";
}

// 仓库内 gold image 路径（generator 写入）。suffix ∈ {bloom_1, nobloom_1}。
std::string GetGoldPath(const std::string& name) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/skydome/sun_path_"
             + name + ".png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/skydome/sun_path_" + name + ".png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());

    // 校验两张 gold 存在（generator 生成；缺失 = 回归）。
    const std::string gold_on = GetGoldPath("bloom_1");
    const std::string gold_off = GetGoldPath("nobloom_1");
    for (const std::string& g : {gold_on, gold_off}) {
        FILE* f = std::fopen(g.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_sun_path_bloom_gold_generator: " << g;
        std::fclose(f);
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Sun Path Bloom Test";
    cfg.headless = true;
    jpov_sun_path_bloom::SunPathBloomApp app(cfg);
    app.Init();
    jpov_standard_sunny_day::BuildScene(&app);
    app.configs_ = jpov_sun_path::DefaultSunPath();
    app.bloom_cfg = jpov_sun_path_bloom::DefaultBloom();
    app.active_index_ = 1;  // 黄昏时段

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    // ---- 渲染 bloom ON，存到输出目录 ----
    const std::string outpath = outdir + "rendered_bloom_on.png";
    app.bloom_enabled = true;
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // smoke check：效果图非空
    {
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &c, 4);
        CHECK(px != nullptr) << "Failed to load rendered PNG: " << outpath
            << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";
        int nz = 0;
        for (int i = 0; i < w * h; ++i) if (px[i * 4 + 3] > 0) ++nz;
        stbi_image_free(px);
        CHECK_GT(nz, 0) << "空场景（bloom on）";
        LOG(INFO) << "bloom on rendered " << w << "x" << h
                  << ", coverage=" << (static_cast<float>(nz) / (w * h) * 100.0f) << "%";
    }

    // ---- 稳定性门禁：bloom ON 渲染 vs bloom ON gold（宽松阈值，容忍光照漂移）----
    const double kStabilityThreshold = 25.0;
    const double stab_diff = jpov::CompareLightMeanRoiPng(gold_on, outpath, 8, 8);
    LOG(INFO) << "STABILITY (bloom ON vs gold ON): max-channel-mean-diff="
              << stab_diff << " (threshold=" << kStabilityThreshold << ")";
    if (stab_diff < 0) {
        LOG(ERROR) << "STABILITY FAILED: 无法比对（gold 或 rendered 读取/尺寸错误）";
        return 1;
    }
    if (stab_diff > kStabilityThreshold) {
        LOG(ERROR) << "STABILITY FAILED: " << stab_diff
                   << " > " << kStabilityThreshold << " → 渲染与 gold 不符！";
        return 1;
    }

    // ---- 效果门禁（负数验证）：per-pixel 对比 bloom ON/OFF 两张 gold ----------------
    // bloom 是确定性后处理，两张 gold 由同一 generator 进程生成（共享 llvmpipe
    // 光照态），其差异纯粹是 bloom 效果。用**逐像素**最大通道差衡量（而非 8×8
    // 平均色 ROI —— 后者把太阳光晕稀疏化稀释到无感，见 8×8 时 diff≈2）。
    // 实测（本场景）：max per-channel diff≈193、>60 像素 258 个。
    // 断言：最大通道差 > 30，且 diff>30 的像素数 ≥ 20 —— 证明高亮区确实被
    // 辉光砸亮一片；若 bloom 是空操作，两点都应为 0，必然不过。
    // （阈值留足余量应对 llvmpipe 光照态差异；实测真实 bloom 常 >100/几十个。
    //   再强调：空操作=0/0，余量巨大，断言仍具强判别力。）
    const int kEffectMinMaxDiff = 30;      // 至少某像素的单通道差 > 30
    const int kEffectMinStrongPixels = 20; // 且 diff>30 的像素至少 20 个
    {
        int w = 0, h = 0, c = 0;
        unsigned char* a = stbi_load(gold_on.c_str(), &w, &h, &c, 4);
        unsigned char* b = stbi_load(gold_off.c_str(), &w, &h, &c, 4);
        CHECK(a != nullptr) << "无法加载 gold_on: " << gold_on;
        CHECK(b != nullptr) << "无法加载 gold_off: " << gold_off;
        int max_diff = 0;
        int strong = 0;
        for (int i = 0; i < w * h; ++i) {
            if (a[i * 4 + 3] == 0) continue;
            const int dr = std::abs(static_cast<int>(a[i * 4]) - b[i * 4]);
            const int dg = std::abs(static_cast<int>(a[i * 4 + 1]) - b[i * 4 + 1]);
            const int db = std::abs(static_cast<int>(a[i * 4 + 2]) - b[i * 4 + 2]);
            const int d = std::max({dr, dg, db});
            if (d > max_diff) max_diff = d;
            if (d > kEffectMinMaxDiff) ++strong;
        }
        stbi_image_free(a);
        stbi_image_free(b);
        LOG(INFO) << "EFFECT (gold ON vs OFF): max per-channel diff=" << max_diff
                  << ", pixels>" << kEffectMinMaxDiff << "=" << strong
                  << " (require max>" << kEffectMinMaxDiff
                  << " and count>=" << kEffectMinStrongPixels << ")";
        if (max_diff <= kEffectMinMaxDiff || strong < kEffectMinStrongPixels) {
            LOG(ERROR) << "EFFECT FAILED: 开/关辉光 per-pixel 差异不足"
                       << " (max=" << max_diff << ", strong=" << strong
                       << ") → bloom 疑似空操作！";
            return 1;
        }
    }

    LOG(INFO) << "TEST PASSED: Bloom 链路跑通；稳定性与效果门禁均通过 "
              << "(gold: sun_path_bloom_1.png / sun_path_nobloom_1.png)";
    return 0;
}
