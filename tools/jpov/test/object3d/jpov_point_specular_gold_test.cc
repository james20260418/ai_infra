// JPOV Gold Test — 点光源的镜面带宽（逐像素角半径 θ = asin(R/dist)）
//
// 与 generator 共用场景头，渲同一组对照条件，然后做**定量**断言（不比对像素：
// llvmpipe 下 PBR 光照有稳态抖动）。
//
// 断言五件事：
//   ① 等效 δ（R=0、roughness=0）的镜面地面上**没有**灯的像（窗口内没有亮像素）。
//      这就是修之前的行为；哪天有人把带宽去掉，这条会立刻变绿 —— 但 ② 会红。
//   ② R=0.02 / 0.10 / 0.50 三档都必须有饱和的像，且面积**随 R 单调显著增长**
//      （θ = asin(R/3.5m)：0.33° / 3.3° / 16.4°）。这条是本次修复的正题：
//      代表点只挪方向，不加带宽 ⇒ 修之前三档面积几乎一样（都是「一个点」）。
//   ③ 位置：像的中心必须在几何预测点（画面正中）±30 px —— 把「像确实是这盏灯的」
//      钉死，而不只是「画面里变亮了」。
//   ④ 峰值仍饱和：带宽只摊开能量、不压暗（D 归一化）—— 大球灯的那张也得有 max=255。
//   ⑤ 粗糙端（roughness=0.30）不能被这次改动弄没（本该有光）。
//
// 修复前实测（同一套场景）：R=0.02 / 0.10 / 0.50 的亮斑都是同一小块（点高光），
//   面积与 R 无关；R=0 当然是全黑。

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

struct SpotStats {
    int max_value = 0;
    long long bright = 0;                    // max(r,g,b) >= 200 的像素数
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;    // 亮像素 bbox（x1<0 = 空）
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
                  << "  亮斑=" << stats[i].bright
                  << "  max=" << stats[i].max_value
                  << "  bbox=" << stats[i].width() << "x" << stats[i].height()
                  << "  中心x=" << stats[i].centerX();
    }

    // 索引与 kShots 顺序一一对应（见 common 头）。
    constexpr int kDelta = 0;
    constexpr int kSmall = 1;
    constexpr int kMedium = 2;
    constexpr int kLarge = 3;
    constexpr int kRough = 4;

    // ① 等效 δ（R=0）：镜面地面上没有灯的像。
    CHECK_EQ(stats[kDelta].bright, 0)
        << "R=0（等效 δ）的点光源在镜面地面上居然有 " << stats[kDelta].bright
        << " 个亮像素 —— 这条对照本来是「修之前的样子」，用于证明 ② 的亮斑来自灯的角半径。";
    CHECK_LE(stats[kDelta].max_value, 10)
        << "R=0 时窗口内仍有 max=" << stats[kDelta].max_value << " 的亮像素（应 ≈ 全黑）。";

    // ② 有尺寸的灯必须都有饱和的像，且面积随 R 单调显著增长。
    for (int i : {kSmall, kMedium, kLarge}) {
        CHECK_GE(stats[i].bright, 100)
            << "R=" << jpov_point_specular::kShots[i].lamp_radius
            << " 的地面上没有灯的像（亮斑=" << stats[i].bright
            << "）。点光源的角半径 θ=asin(R/dist) 没接进 a2？";
        CHECK_EQ(stats[i].max_value, 255)
            << "R=" << jpov_point_specular::kShots[i].lamp_radius
            << " 的像不饱和（max=" << stats[i].max_value
            << "）—— 带宽只该把能量摊开、不该压暗峰值（D 归一化）。";
    }
    const double small_to_medium =
        static_cast<double>(stats[kMedium].bright) / std::max<long long>(stats[kSmall].bright, 1);
    const double medium_to_large =
        static_cast<double>(stats[kLarge].bright) / std::max<long long>(stats[kMedium].bright, 1);
    CHECK_GE(small_to_medium, 1.8)
        << "R=0.02 → 0.10 的亮斑只长了 " << small_to_medium
        << " 倍（应 ≥1.8；θ 从 0.33° 到 3.3°）。代表点改方向、不改 a2？";
    CHECK_GE(medium_to_large, 1.8)
        << "R=0.10 → 0.50 的亮斑只长了 " << medium_to_large
        << " 倍（应 ≥1.8；θ 从 3.3° 到 16.4°）。";

    // ③ 位置：像的中心必须在画面正中（几何预测点）附近。
    CHECK_LE(std::abs(stats[kMedium].centerX() -
                      static_cast<double>(jpov_point_specular::kExpectedCenterX)), 30.0)
        << "灯的像不在画面正中（横向中心=" << stats[kMedium].centerX()
        << "，应为 " << jpov_point_specular::kExpectedCenterX
        << "）。机位/灯位推导或带宽口径被改坏了？";

    // ⑤ 粗糙端（roughness 主导）必须有光。
    CHECK_GE(stats[kRough].bright, 100)
        << "roughness=0.30 的那张几乎没有光了（亮斑=" << stats[kRough].bright
        << "）—— 点光的带宽逻辑把粗糙端也影响掉了？";

    app.ClearPointLamp();
    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "point specular gold test passed";
    return 0;
}
