// JPOV postprocess — 最终亮度/对比度微调 gold test（双图 + 像素范围检查）。
//
// 以 pliers 场景为基底，验证 final_contrast / final_brightness 接口：
//   - 高对比低亮图（contrast=1.8, brightness=-0.08）
//   - 低对比图     （contrast=0.4, brightness=0.0）
//
// 用**像素范围/统计**验证（非逐像素，容忍 llvmpipe PBR 抖动）：
//   物体区域 mask 取自高对比图的非黑像素（背景 lin=0→srgb=0，几何不随
//   调整参数变 → mask 对两图有效）。两图用同一 mask 统计，彻底排除背景。
//
// 断言：
//   1. 两图存在、mask 非空（场景渲染出物体）
//   2. 对比度（std / 范围）：高对比图 > 低对比图
//   3. 亮度（mean）：高对比低亮图 < 低对比图
//   4. 像素范围：高对比图暗部更低（min 更小）、范围更宽 → 对比拉伸生效
//
// gold 图由 final_adjust_gold_generator 生成，本 test 用同一 common 场景
// 渲染并做范围检查（验证调整行为成立，不比对 gold 像素本身）。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/final_adjust_common.h"

namespace {

// 读取 PNG，返回灰度亮度数组（[0,255]，含背景）与尺寸。
// 返回 false 表示读取失败。
bool LoadLum(const std::string& path, std::vector<float>* lum /*output*/,
             int* width /*output*/, int* height /*output*/) {
    int w = 0, h = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    if (!px) {
        LOG(ERROR) << "Failed to load " << path
                   << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?")
                   << ")";
        return false;
    }
    lum->clear();
    lum->reserve(w * h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* q = &px[i * 4];
        lum->push_back((q[0] + q[1] + q[2]) / 3.0f);
    }
    stbi_image_free(px);
    *width = w;
    *height = h;
    return true;
}

// 在高对比图上构建物体 mask：非黑像素（背景 lin=0→srgb(0)=0）。
// 几何不随调整参数变，mask 对低对比图同样有效。
std::vector<bool> BuildObjectMask(const std::string& path,
                                  int w, int h) {
    unsigned char* px = stbi_load(path.c_str(), &w, &h, nullptr, 4);
    CHECK(px != nullptr) << "Failed to load for mask: " << path;
    std::vector<bool> mask(w * h, false);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* q = &px[i * 4];
        if (q[0] > 0 || q[1] > 0 || q[2] > 0) mask[i] = true;
    }
    stbi_image_free(px);
    return mask;
}

struct Stats {
    double min_v;
    double max_v;
    double mean;
    double std;
};

// 对 mask 内的像素（物体区域）统计 min/max/mean/std。
Stats ComputeMaskedStats(const std::vector<float>& lum,
                         const std::vector<bool>& mask) {
    CHECK_EQ(lum.size(), mask.size());
    double sum = 0.0;
    double sq_sum = 0.0;
    double mins = 256.0;
    double maxs = -1.0;
    long n = 0;
    for (size_t i = 0; i < lum.size(); ++i) {
        if (!mask[i]) continue;
        const float v = lum[i];
        sum += v;
        sq_sum += static_cast<double>(v) * v;
        if (v < mins) mins = v;
        if (v > maxs) maxs = v;
        ++n;
    }
    Stats s;
    s.min_v = mins;
    s.max_v = maxs;
    s.mean = (n > 0) ? sum / n : 0.0;
    const double var = (n > 0) ? (sq_sum / n - s.mean * s.mean) : 0.0;
    s.std = std::sqrt(var > 0.0 ? var : 0.0);
    return s;
}

}  // namespace

int main() {
    // ---- 用当前渲染器代码渲染两图（验证接口回归，而非仅读旧 gold）----
    // 若接口被改坏，此处渲染出的结果会与 gold 不同，统计断言将 FAIL。
    const std::string outdir = jpov::GetOutputDir() +
                                  "jpov_final_adjust_test/";
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "Final Adjust Test";
    cfg.headless = true;
    jpov::test::FinalAdjustApp app(cfg);
    app.Init();

    jpov::GltfObject gltf =
        app.LoadGltf(jpov::test::FinalAdjustApp::GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    app.SetGltfObject(std::move(gltf));

    const std::string hc_rnd = outdir + "hc.png";
    jpov::test::RenderFinalAdjust(
        &app, hc_rnd, jpov::test::FinalAdjustParams{1.8f, -0.08f});

    const std::string lc_rnd = outdir + "lc.png";
    jpov::test::RenderFinalAdjust(
        &app, lc_rnd, jpov::test::FinalAdjustParams{0.4f, 0.0f});
    app.Finalize();

    // ---- 读取渲染结果亮度 ----
    std::vector<float> hc_lum, lc_lum;
    int hc_w = 0, hc_h = 0, lc_w = 0, lc_h = 0;
    CHECK(LoadLum(hc_rnd, &hc_lum, &hc_w, &hc_h));
    CHECK(LoadLum(lc_rnd, &lc_lum, &lc_w, &lc_h));
    CHECK_EQ(hc_w, lc_w);
    CHECK_EQ(hc_h, lc_h);
    const int w = hc_w, h = hc_h;

    // ---- 物体 mask（来自高对比渲染图非黑，几何不随调整参数变）----
    const std::vector<bool> obj_mask = BuildObjectMask(hc_rnd, w, h);
    long obj_count = 0;
    for (bool b : obj_mask) if (b) ++obj_count;
    CHECK_GT(obj_count, 0) << "场景未渲染出物体（mask 为空？）";
    LOG(INFO) << "物体 mask 像素数=" << obj_count << "/" << (w * h);

    const Stats hc = ComputeMaskedStats(hc_lum, obj_mask);
    const Stats lc = ComputeMaskedStats(lc_lum, obj_mask);

    LOG(INFO) << "高对比低亮[物体]: mean=" << hc.mean
              << " std=" << hc.std
              << " min=" << hc.min_v << " max=" << hc.max_v;
    LOG(INFO) << "低对比[物体]:     mean=" << lc.mean
              << " std=" << lc.std
              << " min=" << lc.min_v << " max=" << lc.max_v;

    // ---- 断言 1：场景渲染出物体（mask 非空）----
    LOG(INFO) << "PASS: 场景渲染出物体（mask 非空）";

    // ---- 断言 2（对比度）：高对比图 std > 低对比图 ----
    CHECK_GT(hc.std, lc.std)
        << "高对比图对比度(std)应更大，实际 hc.std=" << hc.std
        << " lc.std=" << lc.std;
    LOG(INFO) << "PASS: 高对比 std(" << hc.std << ") > 低对比 std("
              << lc.std << ")";

    // ---- 断言 3（亮度）：高对比低亮图 mean < 低对比图 ----
    CHECK_LT(hc.mean, lc.mean)
        << "高对比低亮图应更暗(mean 更小)，实际 hc.mean=" << hc.mean
        << " lc.mean=" << lc.mean;
    LOG(INFO) << "PASS: 高对比低亮 mean(" << hc.mean << ") < 低对比 mean("
              << lc.mean << ")";

    // ---- 断言 4（像素范围）：高对比图暗部更低、范围更宽 ----
    const double hc_range = hc.max_v - hc.min_v;
    const double lc_range = lc.max_v - lc.min_v;
    LOG(INFO) << "范围: 高对比=[" << hc.min_v << "," << hc.max_v << "] ("
              << hc_range << ")  低对比=[" << lc.min_v << "," << lc.max_v
              << "] (" << lc_range << ")";
    CHECK_LT(hc.min_v, lc.min_v)
        << "高对比图暗部应更低(min 更小)，实际 hc.min=" << hc.min_v
        << " lc.min=" << lc.min_v;
    CHECK_GT(hc_range, lc_range)
        << "高对比图亮度范围应更宽，实际 hc_range=" << hc_range
        << " lc_range=" << lc_range;
    LOG(INFO) << "PASS: 高对比 min(" << hc.min_v << ") < 低对比 min("
              << lc.min_v << ")，且范围更宽";

    LOG(INFO) << "TEST PASSED: 最终亮度/对比度微调接口生效（对比+亮度方向正确）";
    return 0;
}
