// JPOV Gold Test — 点光源的镜面带宽（逐像素角半径 θ = asin(R/dist)）
//
// 与 generator 共用场景头，渲同一组对照条件，然后做**定量**断言
// （不比对像素：llvmpipe 下 PBR 光照有稳态抖动）。
//
// 四张图都是**正常观感**（地面映天光与散布方块；太阳关掉、只有那盏球灯在打高光）——
// 刻意不出"关掉天光、只剩这盏灯"的诊断照（地面几乎全黑，检查时容易误读成渲染坏了）。
//
// 断言四件事：
//   ① R=0.02 / 0.10 / 0.50 三档都必须有**饱和**（≥250）的灯的像。
//      修之前这三档都是「一个点」：R=0.02/0.10 一个饱和像素都没有，R=0.50 只有一线。
//   ② 面积随 R **单调显著增长**（θ = asin(R/3.5 m)：0.33° / 3.3° / 16.4°）。
//      这条是本次修复的正题：代表点只挪方向、不加带宽 ⇒ 修之前面积与 R 无关。
//   ③ 位置：像的中心必须在几何预测点（画面正中）±30 px —— 把「像确实是这盏灯」
//      钉死，而不只是「画面里变亮了」。
//   ④ 粗糙端（roughness=0.30）不能被这次改动弄没（本该有光）。
//
// 修复前实测（同一套场景，阈值 250）：0 / 0 / ~0 px；修好后 80 / 512 / 3785 px。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_point_specular_gold_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 统计窗口：画面正中 ±kWindowHalf 像素（灯的像就在正中）。
constexpr int kWindowHalf = 110;

// 「灯的像」的判据：饱和（≥250）。地面映上来的天光本来就在 200 上下，
// 只有灯的像会把峰值顶到饱和 —— 阈值 250 正好把"有像/没像"分开。
constexpr int kSaturated = 250;

struct SpotStats {
    int max_value = 0;
    long long saturated = 0;                 // max(r,g,b) >= kSaturated 的像素数
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;    // 上述像素的 bbox（x1<0 = 空）
    int width() const { return x1 < 0 ? 0 : (x1 - x0 + 1); }
    int height() const { return y1 < 0 ? 0 : (y1 - y0 + 1); }
    double centerX() const { return 0.5 * (x0 + x1); }
};

SpotStats AnalyzeWindow(const unsigned char* rgba, int w, int h) {
    SpotStats s;
    const int cx = jpov_point_specular::kExpectedCenterX;
    const int cy = jpov_point_specular::kExpectedCenterY;
    for (int y = std::max(0, cy - kWindowHalf); y < std::min(h, cy + kWindowHalf); ++y) {
        for (int x = std::max(0, cx - kWindowHalf); x < std::min(w, cx + kWindowHalf); ++x) {
            const unsigned char* p = rgba + (static_cast<size_t>(y) * w + x) * 4;
            const int mx = std::max({p[0], p[1], p[2]});
            s.max_value = std::max(s.max_value, mx);
            if (mx < kSaturated) {
                continue;
            }
            ++s.saturated;
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
    cfg.title = "Point Specular Bandwidth (sphere light radius) Test";
    cfg.headless = true;

    jpov_skylight::SkylightApp app(cfg);
    app.SetShowPanel(false);
    app.Init();
    jpov_point_specular::InstallScene(&app);

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(jpov_point_specular::kImageWidth);
    winfo.height = static_cast<float>(jpov_point_specular::kImageHeight);
    jpov::InputSnapshot input{};

    const std::string out_dir = jpov::GetOutputDir() + "jpov_point_specular_test/";
    std::vector<SpotStats> stats(jpov_point_specular::kShotCount);

    for (int i = 0; i < jpov_point_specular::kShotCount; ++i) {
        const jpov_point_specular::Shot& shot = jpov_point_specular::kShots[i];
        jpov_point_specular::ApplyShot(&app, shot);
        const std::string path = out_dir + jpov_point_specular::ShotFileName(shot);
        app.RunOnce(input, winfo, path.c_str());

        int w = 0;
        int h = 0;
        const std::vector<unsigned char> img = LoadRgba(path, &w, &h);
        stats[i] = AnalyzeWindow(img.data(), w, h);
        LOG(INFO) << shot.file_tag
                  << "  饱和=" << stats[i].saturated
                  << "  max=" << stats[i].max_value
                  << "  bbox=" << stats[i].width() << "x" << stats[i].height()
                  << "  中心x=" << stats[i].centerX();
    }

    // 索引与 kShots 顺序一一对应（见 common 头）。
    constexpr int kSmall = 0;
    constexpr int kMedium = 1;
    constexpr int kLarge = 2;
    constexpr int kRough = 3;

    // ① 有尺寸的灯必须都有饱和的像。
    for (int i : {kSmall, kMedium, kLarge}) {
        CHECK_GE(stats[i].saturated, 40)
            << "R=" << jpov_point_specular::kShots[i].lamp_radius
            << " 的地面上没有灯的像（饱和像素=" << stats[i].saturated
            << "）。点光源的角半径 θ=asin(R/dist) 没接进 a2？";
    }

    // ② 面积随灯的物理半径单调显著增长（θ 从 0.33° → 3.3° → 16.4°）。
    const double small_to_medium =
        static_cast<double>(stats[kMedium].saturated) / std::max<long long>(stats[kSmall].saturated, 1);
    const double medium_to_large =
        static_cast<double>(stats[kLarge].saturated) / std::max<long long>(stats[kMedium].saturated, 1);
    CHECK_GE(small_to_medium, 1.8)
        << "R=0.02 → 0.10 的灯的像只长了 " << small_to_medium
        << " 倍（应 ≥1.8）。代表点改方向、不改 a2？";
    CHECK_GE(medium_to_large, 1.8)
        << "R=0.10 → 0.50 的灯的像只长了 " << medium_to_large
        << " 倍（应 ≥1.8）。";

    // ③ 位置：像的中心必须在画面正中（几何预测点）附近。
    CHECK_LE(std::abs(stats[kMedium].centerX() -
                      static_cast<double>(jpov_point_specular::kExpectedCenterX)), 30.0)
        << "灯的像不在画面正中（横向中心=" << stats[kMedium].centerX()
        << "，应为 " << jpov_point_specular::kExpectedCenterX
        << "）。机位/灯位推导或带宽口径被改坏了？";

    // ④ 粗糙端（roughness 主导）必须有光。
    CHECK_GE(stats[kRough].saturated, 200)
        << "roughness=0.30 的那张几乎没有光了（饱和像素=" << stats[kRough].saturated
        << "）—— 点光的带宽逻辑把粗糙端也影响掉了？";

    app.ClearPointLamp();
    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "point specular gold test passed";
    return 0;
}
