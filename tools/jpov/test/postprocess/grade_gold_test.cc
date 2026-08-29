// JPOV postprocess — 色彩分级（ASC-CDL）gold test（真实像素检查）。
//
// 以 pliers 场景为基底，验证 ColorGrade（per-channel lift/gamma/gain）接口
// 确实按预设方向改变了物体像素的颜色。用**相对 neutral 基准做差分**断言
//（pliers 本身偏暖 R>B，故风格是否成立看"相对基准的改变"而非绝对值）：
//
//   1. 萧瑟秋日(autumn)     应显著更暖：autumn R-B > neutral R-B + 阈值
//   2. 光亮春天(spring)     应更亮更冷：spring mean > neutral mean，且
//                           spring R-B < neutral R-B（退暖向冷）
//   3. 青橙电影感(teal_orange)  暗部应强青：teal 暗部像素 B > R（R-B 明显为负）
//
// 所有图用当前渲染器**重新渲染**（非读旧 gold），可捕获接口回归。物体 mask
// 来自 neutral（无分级,黑背景纯黑）的非黑像素,几何不随分级变,排除背景。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/final_adjust_common.h"

namespace {

struct ChannelStats {
    double mean_r, mean_g, mean_b, mean_lum;
    double dark_rb;  // 暗部(lum<90)像素的 mean(R) - mean(B)
    long dark_count;
};

// 分通道统计 mask 内物体的 R/G/B 均值、亮度均值、暗部(lum<90)的 R-B。
// mask 来自 neutral（黑背景）。Pre-condition: lum.size()==mask.size()
ChannelStats ComputeChannelStats(const std::vector<float>& lum_r,
                                 const std::vector<float>& lum_g,
                                 const std::vector<float>& lum_b,
                                 const std::vector<bool>& mask) {
    CHECK_EQ(lum_r.size(), mask.size());
    double sr = 0, sg = 0, sb = 0, sl = 0;
    double dark_r = 0, dark_b = 0;
    long n = 0, dn = 0;
    for (size_t i = 0; i < lum_r.size(); ++i) {
        if (!mask[i]) continue;
        const double r = lum_r[i], g = lum_g[i], b = lum_b[i];
        const double lum = (r + g + b) / 3.0;
        sr += r; sg += g; sb += b; sl += lum; ++n;
        if (lum < 90.0) { dark_r += r; dark_b += b; ++dn; }
    }
    ChannelStats s;
    s.mean_r = (n > 0) ? sr / n : 0.0;
    s.mean_g = (n > 0) ? sg / n : 0.0;
    s.mean_b = (n > 0) ? sb / n : 0.0;
    s.mean_lum = (n > 0) ? sl / n : 0.0;
    s.dark_rb = (dn > 0) ? (dark_r - dark_b) / dn : 0.0;
    s.dark_count = dn;
    return s;
}

// 读 RGBA 的单个通道为 float 数组。pre-condition: stbi 成功。
bool LoadChannel(const std::string& path, std::vector<float>* r /*out*/,
                 std::vector<float>* g /*out*/, std::vector<float>* b /*out*/,
                 int* w, int* h) {
    unsigned char* px = stbi_load(path.c_str(), w, h, nullptr, 4);
    if (!px) return false;
    const int n = (*w) * (*h);
    r->resize(n); g->resize(n); b->resize(n);
    for (int i = 0; i < n; ++i) {
        (*r)[i] = px[i * 4 + 0];
        (*g)[i] = px[i * 4 + 1];
        (*b)[i] = px[i * 4 + 2];
    }
    stbi_image_free(px);
    return true;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "jpov_grade_test/";
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "Color Grade Test";
    cfg.headless = true;
    jpov::test::FinalAdjustApp app(cfg);
    app.Init();
    jpov::GltfObject gltf =
        app.LoadGltf(jpov::test::FinalAdjustApp::GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    app.SetGltfObject(std::move(gltf));

    // ---- 渲染基准 + 3 风格（当前代码）----
    const jpov::test::FinalAdjustParams base_params{1.0f, 0.0f, 1.0f};
    jpov::ColorGrade g;

    const std::string neutral_p = outdir + "neutral.png";
    app.SetGrade(jpov::ColorGrade{});  // disabled
    jpov::test::RenderFinalAdjust(&app, neutral_p, base_params);

    const std::string autumn_p = outdir + "autumn.png";
    g = jpov::ColorGrade{{1.18f,1.04f,0.78f},{0.05f,0.03f,0.0f},{0.95f,1.0f,1.1f}};
    g.enabled = true;
    app.SetGrade(g);
    jpov::test::RenderFinalAdjust(&app, autumn_p, base_params);

    const std::string spring_p = outdir + "spring.png";
    g = jpov::ColorGrade{{1.02f,1.06f,1.18f},{0.04f,0.05f,0.08f},{0.80f,0.84f,0.90f}};
    g.enabled = true;
    app.SetGrade(g);
    jpov::test::RenderFinalAdjust(&app, spring_p, base_params);

    const std::string teal_p = outdir + "teal.png";
    g = jpov::ColorGrade{{1.22f,0.98f,0.80f},{0.0f,0.03f,0.12f},{1.20f,1.12f,0.92f}};
    g.enabled = true;
    app.SetGrade(g);
    jpov::test::RenderFinalAdjust(&app, teal_p, base_params);
    app.Finalize();

    // ---- 读取各图通道 + 构建物体 mask（来自 neutral 非黑）----
    std::vector<float> nr, ng, nb;
    int w = 0, h = 0;
    CHECK(LoadChannel(neutral_p, &nr, &ng, &nb, &w, &h));
    const std::vector<bool> obj_mask =
        jpov::test::BuildMaskFromPng(neutral_p, w, h);
    long obj_n = 0;
    for (bool b : obj_mask) if (b) ++obj_n;
    CHECK_GT(obj_n, 0) << "场景未渲染出物体（mask 空？)";
    LOG(INFO) << "物体 mask 像素数=" << obj_n << "/" << (w * h);

    auto load3 = [&](const std::string& p, std::vector<float>* a,
                     std::vector<float>* b_, std::vector<float>* c) {
        int w2=0, h2=0;
        CHECK(LoadChannel(p, a, b_, c, &w2, &h2));
        CHECK_EQ(w2, w); CHECK_EQ(h2, h);
    };
    std::vector<float> ar, ag, ab, spr, spg, spb, tr, tg, tb;
    load3(autumn_p, &ar, &ag, &ab);
    load3(spring_p, &spr, &spg, &spb);
    load3(teal_p, &tr, &tg, &tb);

    const ChannelStats n  = ComputeChannelStats(nr, ng, nb, obj_mask);
    const ChannelStats au = ComputeChannelStats(ar, ag, ab, obj_mask);
    const ChannelStats sp = ComputeChannelStats(spr, spg, spb, obj_mask);
    const ChannelStats te = ComputeChannelStats(tr, tg, tb, obj_mask);

    LOG(INFO) << "neutral:   meanRGB=(" << (int)n.mean_r << ","
              << (int)n.mean_g << "," << (int)n.mean_b << ") R-B="
              << (int)(n.mean_r - n.mean_b);
    LOG(INFO) << "autumn:    meanRGB=(" << (int)au.mean_r << ","
              << (int)au.mean_g << "," << (int)au.mean_b << ") R-B="
              << (int)(au.mean_r - au.mean_b);
    LOG(INFO) << "spring:    meanRGB=(" << (int)sp.mean_r << ","
              << (int)sp.mean_g << "," << (int)sp.mean_b << ") R-B="
              << (int)(sp.mean_r - sp.mean_b) << " 暗部n=" << sp.dark_count;
    LOG(INFO) << "teal:      meanRGB=(" << (int)te.mean_r << ","
              << (int)te.mean_g << "," << (int)te.mean_b << ") R-B="
              << (int)(te.mean_r - te.mean_b)
              << " 暗部n=" << te.dark_count << " 暗部R-B="
              << (int)te.dark_rb;

    // ---- 断言 1：autumn 显著更暖（R-B 相对 neutral 明显增大）----
    CHECK_GT(au.mean_r - au.mean_b, n.mean_r - n.mean_b + 30.0)
        << "秋日应比基准明显更暖(R-B 增大>30)，actual autumn R-B="
        << (au.mean_r - au.mean_b) << " neutral R-B=" << (n.mean_r - n.mean_b);
    LOG(INFO) << "PASS: autumn 更暖(R-B +" << (int)((au.mean_r-au.mean_b)-(n.mean_r-n.mean_b)) << ")";

    // ---- 断言 2：spring 更亮 + 更冷 ----
    CHECK_GT(sp.mean_lum, n.mean_lum)
        << "春天应比基准更亮，actual spring lum=" << sp.mean_lum
        << " neutral lum=" << n.mean_lum;
    CHECK_LT(sp.mean_r - sp.mean_b, n.mean_r - n.mean_b)
        << "春天应比基准更冷(R-B 减小)，actual spring R-B="
        << (sp.mean_r - sp.mean_b) << " neutral R-B=" << (n.mean_r - n.mean_b);
    LOG(INFO) << "PASS: spring 更亮且更冷";

    // ---- 断言 3：teal 暗部强青（split-tone 暗部青蓝）----
    CHECK_LT(te.dark_rb, -20.0)
        << "青橙电影感暗部应偏青(R-B 明显为负)，actual 暗部R-B="
        << te.dark_rb;
    LOG(INFO) << "PASS: teal 暗部偏青(暗部R-B=" << (int)te.dark_rb << ")";

    LOG(INFO) << "TEST PASSED: 色彩分级(ASC-CDL)接口生效，风格方向正确";
    return 0;
}
