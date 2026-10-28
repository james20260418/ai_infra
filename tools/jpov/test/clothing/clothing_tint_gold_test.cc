// 顶点 tint gold 测试 —— 锁住三条路径（object3d / 静态实例 / 蒙皮）的 tint 渲染，
// 并给「逐实例颜色」加行为门禁。
//
// 三道门禁：
//   ① gold 比对（ROI 均值，阈值 20；llvmpipe PBR 非逐像素稳定，与其它 gold 同口径）。
//   ② tint on/off 必须明显不同 ⇒ 证明 tint 真的进了 shader（不是“调色板设了但没接”）。
//   ③ 实例路径：左半屏应有红主导像素、右半屏应有蓝主导像素 ⇒ **逐实例颜色**确实不同
//      （左实例红/绿，右实例蓝/黄）。
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"

#include "tools/jpov/test/clothing/clothing_tint_gold_common.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/test_utils.h"

namespace {
using jpov_cloth_tint_gold::Mode;

std::string GoldPath(Mode m) {
    const char* e = std::getenv("TEST_SRCDIR");
    const std::string rel = "tools/jpov/test" + jpov_cloth_tint_gold::GoldRelPath(m);
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/" + rel;
    }
    return jpov::GetTestDataDir() + jpov_cloth_tint_gold::GoldRelPath(m);
}

struct Img {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> px;
};

Img Load(const std::string& p) {
    Img im;
    int c = 0;
    unsigned char* d = stbi_load(p.c_str(), &im.w, &im.h, &c, 4);
    if (d == nullptr) {
        LOG(ERROR) << "读图失败: " << p;
        return im;
    }
    im.px.assign(d, d + static_cast<size_t>(im.w) * im.h * 4);
    stbi_image_free(d);
    return im;
}

size_t CountChanged(const Img& a, const Img& b, int threshold) {
    CHECK_EQ(a.px.size(), b.px.size());
    size_t changed = 0;
    for (size_t i = 0; i + 3 < a.px.size(); i += 4) {
        const int dr = std::abs(int(a.px[i]) - int(b.px[i]));
        const int dg = std::abs(int(a.px[i + 1]) - int(b.px[i + 1]));
        const int db = std::abs(int(a.px[i + 2]) - int(b.px[i + 2]));
        if (dr > threshold || dg > threshold || db > threshold) {
            ++changed;
        }
    }
    return changed;
}

// 半屏内「某通道主导」的像素数（前景+背景都算，但背景不满足主导条件）。
size_t CountDominant(const Img& im, int x0, int x1, int channel) {
    size_t n = 0;
    for (int y = 0; y < im.h; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * im.w + x) * 4;
            const int r = im.px[i];
            const int g = im.px[i + 1];
            const int b = im.px[i + 2];
            if (channel == 0 && r > 2 * g + 15 && r > 2 * b + 15) {
                ++n;
            } else if (channel == 1 && g > 2 * r + 15 && g > 2 * b + 15) {
                ++n;
            } else if (channel == 2 && b > 2 * r + 15 && b > 2 * g + 15) {
                ++n;
            }
        }
    }
    return n;
}

}  // namespace

int main() {
    const std::string outdir = jpov::GetOutputDir() + "clothing_tint_gold_test/";
    std::system(("mkdir -p " + outdir).c_str());

    for (Mode m : {Mode::kObject3d, Mode::kInstanced, Mode::kSkinned}) {
        const std::string on_png = outdir + jpov_cloth_tint_gold::ModeName(m) + "_on.png";
        const std::string off_png = outdir + jpov_cloth_tint_gold::ModeName(m) + "_off.png";
        CHECK(jpov_cloth_tint_gold::RenderTintGold(m, /*tint_on=*/true, on_png))
            << "渲图失败: " << jpov_cloth_tint_gold::ModeName(m);
        CHECK(jpov_cloth_tint_gold::RenderTintGold(m, /*tint_on=*/false, off_png))
            << "渲图失败（off）: " << jpov_cloth_tint_gold::ModeName(m);

        const Img gold = Load(GoldPath(m));
        const Img on = Load(on_png);
        const Img off = Load(off_png);
        CHECK(gold.w > 0 && gold.h > 0)
            << "gold 缺失/读不出，请先跑 generator: " << GoldPath(m);
        CHECK(on.w == gold.w && on.h == gold.h)
            << "渲图与 gold 尺寸不一致: " << jpov_cloth_tint_gold::ModeName(m);

        // ① gold 比对（ROI 均值）。
        jpov::MeanCompareReport rep;
        const double d = jpov::CompareLightMeanRoi(
            gold.px.data(), on.px.data(), gold.w, gold.h, /*nrow*/ 8, /*ncol*/ 8, &rep);
        LOG(INFO) << jpov_cloth_tint_gold::ModeName(m) << " gold ROI 最大通道差 = " << d;
        CHECK_LE(d, 20.0) << jpov_cloth_tint_gold::ModeName(m)
                          << " 与 gold 差异过大（" << d << "）";

        // ② tint on/off 行为门禁。
        const size_t changed = CountChanged(on, off, /*threshold*/ 20);
        LOG(INFO) << jpov_cloth_tint_gold::ModeName(m)
                  << " tint on/off 变化像素 = " << changed;
        CHECK_GT(changed, 800u)
            << "tint on/off 几乎一样（" << jpov_cloth_tint_gold::ModeName(m)
            << "）—— tint 可能没接上";

        // ③ 逐实例颜色门禁（两条实例路径）。
        if (m != Mode::kObject3d) {
            const size_t red_left = CountDominant(on, 0, on.w / 2, /*channel*/ 0);
            const size_t blue_right = CountDominant(on, on.w / 2, on.w, /*channel*/ 2);
            LOG(INFO) << jpov_cloth_tint_gold::ModeName(m) << " 左红主导 = " << red_left
                      << "，右蓝主导 = " << blue_right;
            CHECK_GT(red_left, 500u) << "左实例没有红主导像素（逐实例颜色可能没生效）";
            CHECK_GT(blue_right, 500u) << "右实例没有蓝主导像素（逐实例颜色可能没生效）";
        }
    }

    LOG(INFO) << "TEST PASSED: 顶点 tint 三条路径 gold + 逐实例颜色";
    return 0;
}
