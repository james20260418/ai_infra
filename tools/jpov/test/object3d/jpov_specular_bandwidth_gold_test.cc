// JPOV Gold Test — 镜面高光的带宽（光源角半径 θ）
//
// 与 generator 共用场景头（jpov_specular_bandwidth_gold_common.h），渲同一组对照条件，
// 然后对渲染结果做**定量**断言（不比对像素：llvmpipe 下 PBR 光照有稳态抖动）。
//
// 断言四件事：
//   ① 钱图（只有太阳的镜面项、roughness=0）：画面正中必须有一块**饱和的亮斑**，位置就是
//      几何预测的那个点（相机看向原点 ⇒ 原点在画面正中）。修之前这里是纯黑（D ≡ 0）。
//   ② 开天光反射那张：日盘的像仍然饱和（max=255）—— 修之前它被映上来的天光完全淹没
//      （max≈231，看不出那块像）。
//   ③ r=0.02 与 r=0 的亮斑面积差 ≤ 20%：带宽由 θ 主导（roughness 只贡献 3%）。
//      这条挡的是「只改 r=0」或「某条 pass 漏改」这类半吊子实现。
//   ④ r=0.30（roughness 主导）必须有光，不允许"修好光滑的、弄坏粗糙的"。
//
// 修复前实测（同一套场景）：①亮斑 0 px（整幅地面一个亮像素都没有）、③ r=0.02 只有
//   一块 max≈238 的暗糊团、② max≈231。

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
    int max_value = 0;                       // 窗口内最大的通道值
    long long bright = 0;                    // max(r,g,b) >= 200 的像素数
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;    // 上述亮像素的 bbox（x1<0 = 空）
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
                  << "  亮斑=" << stats[i].bright
                  << "  max=" << stats[i].max_value
                  << "  中心x=" << stats[i].centerX();
    }

    // 索引与 kShots 顺序一一对应（见 common 头）。
    constexpr int kPureR0 = 0;
    constexpr int kMirrorR0 = 1;
    constexpr int kPureR002 = 2;
    constexpr int kMirrorRough = 3;

    // ① r=0 的纯太阳镜面项：正中必须有一块饱和的亮斑。
    CHECK_GE(stats[kPureR0].bright, 300)
        << "r=0 的镜面地面上没有太阳的像（亮斑像素=" << stats[kPureR0].bright
        << "）。完美镜面 + 平行光必须有带宽（θ = 光源角半径）：否则 r=0 时 D ≡ 0，"
           "高光彻底消失 —— 见 docs/jpov_specular_bandwidth.md。";
    CHECK_GE(stats[kPureR0].max_value, 250)
        << "太阳的像不饱和（max=" << stats[kPureR0].max_value
        << "）—— D 的峰值被护栏或带宽口径压掉了？";
    // 位置：亮斑中心必须在几何预测点（画面正中）附近。
    CHECK_LE(std::abs(stats[kPureR0].centerX() -
                      static_cast<double>(jpov_specular_bandwidth::kExpectedCenterX)), 30.0)
        << "太阳的像不在画面正中（横向中心=" << stats[kPureR0].centerX()
        << "，应为 " << jpov_specular_bandwidth::kExpectedCenterX
        << "）。机位/太阳方向或带宽口径被改坏了？";

    // ② 开天光反射那张：日盘的像仍然饱和（修之前被映上来的天光淹没，max≈231）。
    CHECK_GE(stats[kMirrorR0].max_value, 250)
        << "开着天光反射时看不到饱和的太阳的像（max=" << stats[kMirrorR0].max_value
        << "，修好时应 ≥250）—— 亮的像被映上来的天光盖掉了（天光消费量被调太高？）";

    // ③ θ 主导：r=0.02 的亮斑面积与 r=0 几乎相同（a2 只多 3%）。
    {
        const double base = static_cast<double>(stats[kPureR0].bright);
        const double diff = std::abs(static_cast<double>(stats[kPureR002].bright) - base) /
                            std::max(base, 1.0);
        CHECK_LE(diff, 0.20)
            << "r=0 与 r=0.02 的亮斑面积相差 " << diff * 100.0
            << "%（应几乎相同：θ 主导，roughness 只贡献 3%）。"
               "两条路径的带宽口径不一致？";
    }

    // ④ 粗糙端（roughness 主导）不能被弄坏：必须有光。
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
