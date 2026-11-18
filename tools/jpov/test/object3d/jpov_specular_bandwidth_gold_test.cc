// JPOV Gold Test — 镜面高光的带宽（光源角半径 θ）
//
// 与 generator 共用场景头（jpov_specular_bandwidth_gold_common.h），渲同一组对照条件，
// 然后对渲染结果做**定量**断言（不比对像素：llvmpipe 下 PBR 光照有稳态抖动）。
//
// 三张图都是**正常观感**（地面映天光与散布方块，日盘的像叠在上面）—— 刻意不出
// "关掉一切其它光、只剩太阳镜面项"的诊断照（地面几乎全黑，检查时容易误读成渲染坏了）。
//
// 断言三件事：
//   ① r=0 的完美镜面：画面正中必须有一块**饱和**（≥250）的日盘像，位置就是几何预测点
//      （相机看向原点 ⇒ 原点在画面正中）。修之前这里 max 只有 231（被映上来的天光淹没），
//      一个 ≥250 的像素都没有。
//   ② r=0.02 与 r=0 的饱和面积差 ≤ 20%：带宽由 θ 主导（roughness 只贡献 3%）。
//      这条挡的是「只改 r=0」或「某条 pass 漏改」这类半吊子实现。
//   ③ r=0.30（roughness 主导）必须有光，不允许"修好光滑的、弄坏粗糙的"。
//
// 修复前实测（同一套场景，阈值 250）：①0 px、②0 px、③仍有光。

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
// 窗口取得比亮斑大一圈，既容得下掠射拉长，又不会把整片天光反射算进来。
constexpr int kWindowHalf = 90;

// 「日盘的像」的判据：饱和（≥250）。地面是镜子、映上来的天光本来就有 200 出头，
// 只有日盘本身会把峰值顶到饱和 —— 阈值取 250 正好把"有像/没像"分开。
constexpr int kSaturated = 250;

struct SpotStats {
    int max_value = 0;                       // 窗口内最大的通道值
    long long saturated = 0;                 // max(r,g,b) >= kSaturated 的像素数
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;    // 上述像素的 bbox（x1<0 = 空）
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

    for (int i = 0; i < jpov_specular_bandwidth::kShotCount; ++i) {
        const jpov_specular_bandwidth::Shot& shot =
            jpov_specular_bandwidth::kShots[i];
        jpov_specular_bandwidth::ApplyShot(&app, shot);
        const std::string path = out_dir +
            jpov_specular_bandwidth::ShotFileName(shot);
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
    constexpr int kR0 = 0;
    constexpr int kR002 = 1;
    constexpr int kRough = 2;

    // ① r=0 的完美镜面：正中必须有饱和的日盘像。
    CHECK_GE(stats[kR0].saturated, 300)
        << "r=0 的镜面地面上没有日盘的像（饱和像素=" << stats[kR0].saturated
        << "）。完美镜面 + 平行光必须有带宽（θ = 光源角半径）：否则 r=0 时 D ≡ 0，"
           "高光彻底消失 —— 见 docs/jpov_specular_bandwidth.md。";
    // 位置：像的中心必须在几何预测点（画面正中）附近。
    CHECK_LE(std::abs(stats[kR0].centerX() -
                      static_cast<double>(jpov_specular_bandwidth::kExpectedCenterX)), 30.0)
        << "日盘的像不在画面正中（横向中心=" << stats[kR0].centerX()
        << "，应为 " << jpov_specular_bandwidth::kExpectedCenterX
        << "）。机位/太阳方向或带宽口径被改坏了？";

    // ② θ 主导：r=0.02 的饱和面积与 r=0 几乎相同（a2 只多 3%）。
    {
        const double base = static_cast<double>(stats[kR0].saturated);
        const double diff = std::abs(static_cast<double>(stats[kR002].saturated) - base) /
                            std::max(base, 1.0);
        CHECK_LE(diff, 0.20)
            << "r=0 与 r=0.02 的日盘像面积相差 " << diff * 100.0
            << "%（应几乎相同：θ 主导，roughness 只贡献 3%）。"
               "两条路径的带宽口径不一致？";
    }

    // ③ 粗糙端（roughness 主导）不能被弄坏：必须有光（实测 ~1e4 px）。
    CHECK_GE(stats[kRough].saturated, 1000)
        << "r=0.30 的镜面地面几乎没有光了（饱和像素=" << stats[kRough].saturated
        << "）—— 带宽逻辑把粗糙端也影响掉了？";

    app.ClearSunOverride();
    app.ClearAmbientOverride();
    app.ClearFloorMaterialOverride();
    app.Finalize();
    LOG(INFO) << "specular bandwidth gold test passed";
    return 0;
}
