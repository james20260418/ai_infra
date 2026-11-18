// JPOV Gold Test — 镜面高光的带宽（光源角半径 θ）
//
// 与 generator 共用场景头（jpov_specular_bandwidth_gold_common.h），渲同一组对照条件，
// 然后对渲染结果做**定量**断言（不比对像素：llvmpipe 下 PBR 光照有稳态抖动）。
//
// 断言的四件事：
//   ① 钱图（只有太阳的镜面项、r=0）：画面正中必须有**一块亮斑**，且位置就是几何预测的
//      那个点（相机看向原点 ⇒ 原点在画面正中）。修之前这里是纯黑（D ≡ 0）。
//   ② 负向对照（同一张图，θ = 1e-6 = 等效 δ）：亮斑必须**消失**。
//      这一条把「亮斑是太阳的镜面项」钉死 —— 如果哪天有人把带宽去掉还"通过"了测试，
//      那说明断言的其实是别的东西。
//   ③ 镜子观感那张：带 θ 与不带 θ 的差**必须集中在画面正中一条窄带**上。
//      只断言"有差别"不够（关掉太阳会把方块和影子一起改掉），必须连**位置**一起钉住。
//   ④ r=0.02 与 r=0 的亮斑面积差 ≤ 20%：带宽由 θ 主导（roughness 只贡献 3%）。
//      这条挡的是「只修 r=0」或「某条 pass 漏改」这类半吊子实现。
//   ⑤ r=0.30（roughness 主导）必须有光，不允许"修好光滑的、弄坏粗糙的"。
//
// 修复前实测（同一套场景）：①亮斑 0 px（整幅地面一个高亮像素都没有）、
//   ②当然也是 0、④ r=0.02 只有一块 max≈40 的暗糊团。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_specular_bandwidth_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 统计窗口：画面正中 ±kWindowHalf 像素。太阳的像就在正中（几何算死），
// 窗口取得比亮斑大一圈，既容得下掠射拉长，又不会把背景天光算进来。
constexpr int kWindowHalf = 90;

// 一张图在窗口内的亮度统计（全部基于 LDR PNG，tone map 之后）。
struct SpotStats {
    int max_value = 0;                 // 窗口内最大的通道值
    long long bright = 0;              // max(r,g,b) >= 200 的像素数
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;   // 上述亮像素的 bbox（x1<0 = 空）
    int width() const { return x1 < 0 ? 0 : (x1 - x0 + 1); }
    int height() const { return y1 < 0 ? 0 : (y1 - y0 + 1); }
    double centerX() const { return 0.5 * (x0 + x1); }
};

SpotStats AnalyzeWindow(const unsigned char* rgba, int w, int h) {
    SpotStats s;
    const int cx = jpov_specular_bandwidth::kExpectedCenterX;
    const int cy = jpov_specular_bandwidth::kExpectedCenterY;
    for (int y = std::max(0, cy - kWindowHalf); y < std::min(h, cy + kWindowHalf); ++y) {
        for (int x = std::max(0, cx - kWindowHalf); x < std::min(w, cx + kWindowHalf); ++x) {
            const unsigned char* p = rgba + (static_cast<size_t>(y) * w + x) * 4;
            const unsigned char mx = std::max({p[0], p[1], p[2]});
            s.max_value = std::max(s.max_value, static_cast<int>(mx));
            if (mx < 200) {
                continue;
            }
            ++s.bright;
            if (s.x1 < 0) {
                s.x0 = s.x1 = x;
                s.y0 = s.y1 = y;
            } else {
                s.x0 = std::min(s.x0, x);
                s.x1 = std::max(s.x1, x);
                s.y0 = std::min(s.y0, y);
                s.y1 = std::max(s.y1, y);
            }
        }
    }
    return s;
}

// 两张图逐像素差（max 通道差 > 4 才算"不同"），只统计窗口内。
struct DiffStats {
    long long pixels = 0;     // 有差异的像素数
    double centroid_x = 0.0;  // 差异像素的横向质心
    int x0 = 0, x1 = -1;      // 横向 bbox
};

DiffStats AnalyzeDiff(const unsigned char* a, const unsigned char* b, int w, int h) {
    DiffStats s;
    const int cx = jpov_specular_bandwidth::kExpectedCenterX;
    const int cy = jpov_specular_bandwidth::kExpectedCenterY;
    double sum_x = 0.0;
    for (int y = std::max(0, cy - kWindowHalf); y < std::min(h, cy + kWindowHalf); ++y) {
        for (int x = std::max(0, cx - kWindowHalf); x < std::min(w, cx + kWindowHalf); ++x) {
            const size_t off = (static_cast<size_t>(y) * w + x) * 4;
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                d = std::max(d, std::abs(static_cast<int>(a[off + c]) -
                                         static_cast<int>(b[off + c])));
            }
            if (d <= 4) {
                continue;
            }
            ++s.pixels;
            sum_x += x;
            if (s.x1 < 0) {
                s.x0 = s.x1 = x;
            } else {
                s.x0 = std::min(s.x0, x);
                s.x1 = std::max(s.x1, x);
            }
        }
    }
    s.centroid_x = (s.pixels > 0) ? sum_x / static_cast<double>(s.pixels) : 0.0;
    return s;
}

std::vector<unsigned char> LoadRgba(const std::string& path, int* out_w, int* out_h) {
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    CHECK(pixels != nullptr) << "无法读取渲染结果: " << path;
    std::vector<unsigned char> data(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    *out_w = w;
    *out_h = h;
    return data;
}

}  // namespace

int main() {
    JPOV::Config cfg;
    cfg.title = "Specular Bandwidth (sun angular radius) Test";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    app.InstallMirrorFloorScene();
    app.SetMirrorSpecularView();

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(jpov_specular_bandwidth::kImageWidth);
    winfo.height = static_cast<float>(jpov_specular_bandwidth::kImageHeight);
    jpov::InputSnapshot input{};

    const std::string out_dir = jpov::GetOutputDir() + "jpov_specular_bandwidth_test/";
    std::vector<SpotStats> stats(jpov_specular_bandwidth::kShotCount);
    std::vector<std::vector<unsigned char>> images(jpov_specular_bandwidth::kShotCount);
    int img_w = 0;
    int img_h = 0;

    for (int i = 0; i < jpov_specular_bandwidth::kShotCount; ++i) {
        const jpov_specular_bandwidth::Shot& shot =
            jpov_specular_bandwidth::kShots[i];
        jpov_specular_bandwidth::ApplyShot(&app, shot);
        const std::string path = out_dir +
            jpov_specular_bandwidth::ShotFileName(shot);
        app.RunOnce(input, winfo, path.c_str());

        int w = 0;
        int h = 0;
        images[i] = LoadRgba(path, &w, &h);
        img_w = w;
        img_h = h;
        stats[i] = AnalyzeWindow(images[i].data(), w, h);
        LOG(INFO) << shot.file_tag
                  << "  亮斑=" << stats[i].bright
                  << "  max=" << stats[i].max_value
                  << "  bbox=" << stats[i].width() << "x" << stats[i].height()
                  << "  中心x=" << stats[i].centerX();
    }

    // 索引与 kShots 顺序一一对应（见 common 头）。
    constexpr int kPureWithBandwidth = 0;
    constexpr int kPureNoBandwidth = 1;
    constexpr int kMirrorWithBandwidth = 2;
    constexpr int kMirrorNoBandwidth = 3;
    constexpr int kPureR002 = 4;
    constexpr int kMirrorRough = 5;

    // ① r=0 的纯太阳镜面项：正中必须有一块饱和的亮斑。
    CHECK_GE(stats[kPureWithBandwidth].bright, 300)
        << "r=0 的镜面地面上没有太阳的像（亮斑像素=" << stats[kPureWithBandwidth].bright
        << "）。完美镜面 + 平行光必须有带宽（θ = 光源角半径）：否则 r=0 时 D ≡ 0，"
           "高光彻底消失 —— 见 docs/jpov_specular_bandwidth.md。";
    // 位置：亮斑中心必须在几何预测点（画面正中）附近。
    CHECK_LE(std::abs(stats[kPureWithBandwidth].centerX() -
                      static_cast<double>(jpov_specular_bandwidth::kExpectedCenterX)), 30.0)
        << "太阳的像不在画面正中（横向中心=" << stats[kPureWithBandwidth].centerX()
        << "，应为 " << jpov_specular_bandwidth::kExpectedCenterX
        << "）。机位/太阳方向或带宽口径被改坏了？";

    // ② 负向对照：θ → 1e-6（等效 δ）⇒ 亮斑必须消失。
    CHECK_EQ(stats[kPureNoBandwidth].bright, 0)
        << "把太阳角半径缩到 1e-6（等效 δ）后仍有 " << stats[kPureNoBandwidth].bright
        << " 个饱和像素 —— 说明这个亮斑不是「带宽带来的太阳的像」，测试断错了对象。";
    CHECK_LE(stats[kPureNoBandwidth].max_value, 20)
        << "等效 δ 的光源下窗口内仍有 max=" << stats[kPureNoBandwidth].max_value
        << " 的亮像素（应 ≈ 地面底色）。";

    // ③ 镜子观感那张：带 θ 与不带 θ 的差别必须**集中在正中一条窄带**上。
    {
        const DiffStats diff = AnalyzeDiff(images[kMirrorWithBandwidth].data(),
                                          images[kMirrorNoBandwidth].data(), img_w, img_h);
        LOG(INFO) << "mirror: θ vs 等效δ 差异像素=" << diff.pixels
                  << " 质心x=" << diff.centroid_x
                  << " 横向宽度=" << (diff.x1 < 0 ? 0 : diff.x1 - diff.x0 + 1);
        CHECK_GE(diff.pixels, 1000)
            << "开着天光反射时，θ 与等效 δ 的差别只有 " << diff.pixels
            << " px —— 太阳的像太弱/根本没画出来（天光消费量被调高到把它淹没？）";
        CHECK_LE(std::abs(diff.centroid_x -
                          static_cast<double>(jpov_specular_bandwidth::kExpectedCenterX)), 30.0)
            << "θ 与等效 δ 的差异不在画面正中（质心x=" << diff.centroid_x
            << "）—— 那不是太阳的像，是被这次改动波及的别的东西。";
        CHECK_LE(diff.x1 < 0 ? 0 : diff.x1 - diff.x0 + 1, 120)
            << "差异横向铺开 " << (diff.x1 - diff.x0 + 1)
            << " px（> 120）—— 太阳的像应当是一条窄带，铺满说明改的是整体光照。";
    }

    // ④ θ 主导：r=0.02 的亮斑面积与 r=0 几乎相同（a2 只多 3%）。
    {
        const double base = static_cast<double>(stats[kPureWithBandwidth].bright);
        const double diff = std::abs(static_cast<double>(stats[kPureR002].bright) - base) /
                            std::max(base, 1.0);
        CHECK_LE(diff, 0.20)
            << "r=0 与 r=0.02 的亮斑面积相差 " << diff * 100.0
            << "%（应几乎相同：θ 主导，roughness 只贡献 3%）。"
               "两条路径的带宽口径不一致？";
    }

    // ⑤ 粗糙端（roughness 主导）不能被弄坏：必须有光。
    CHECK_GE(stats[kMirrorRough].max_value, 100)
        << "r=0.30 的镜面地面几乎没光了（窗口 max=" << stats[kMirrorRough].max_value
        << "）—— 带宽逻辑把粗糙端也影响掉了？";

    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "specular bandwidth gold test passed";
    return 0;
}
