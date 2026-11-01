// SSR 输入准备（降采样）的纯数学单测 —— 不建 GL 上下文。
//
// 钉住三件事：
//   1. 倍率只接受 1/N（N = 1..4）；
//   2. 目标尺寸向上取整、至少 1×1；
//   3. 选点规则 = N×N 组里取**窗口深度最小**的那个 texel（三张图同源的前提）。
// 每条正向断言都配一条负向对照，避免"恒真断言"。

#include "tools/jpov/src/ssr/ssr_downsample.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace jpov {
namespace {

TEST(SsrDownsample, AcceptsOnlyReciprocalScale) {
    EXPECT_TRUE(IsValidScale(1.0f));
    EXPECT_TRUE(IsValidScale(0.5f));
    EXPECT_TRUE(IsValidScale(1.0f / 3.0f));
    EXPECT_TRUE(IsValidScale(0.25f));
    // 负向：非 1/N、0、负数、接近但不等于 1/N。
    EXPECT_FALSE(IsValidScale(0.0f));
    EXPECT_FALSE(IsValidScale(-0.5f));
    EXPECT_FALSE(IsValidScale(0.4f));
    EXPECT_FALSE(IsValidScale(0.75f));
    EXPECT_FALSE(IsValidScale(0.34f));
}

TEST(SsrDownsample, TargetSizeRoundsUp) {
    int w = 0;
    int h = 0;
    DownsampleTargetSize(1280, 720, 2, &w, &h);
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 360);

    // 非整除时向上取整（否则右/下边缘会缺一条）。
    DownsampleTargetSize(1281, 721, 2, &w, &h);
    EXPECT_EQ(w, 641);
    EXPECT_EQ(h, 361);

    // N=3（不是 2 的幂）也要成立。
    DownsampleTargetSize(1280, 720, 3, &w, &h);
    EXPECT_EQ(w, 427);   // ceil(1280/3) = 427
    EXPECT_EQ(h, 240);

    // 极端小图：至少 1×1。
    DownsampleTargetSize(3, 3, 4, &w, &h);
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
    // 负向对照：如果实现对 0 尺寸也返回 0，上面这条会失败（GPU 侧 0 尺寸必炸）。
    DownsampleTargetSize(0, 0, 2, &w, &h);
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
}

// 8×6 的全分辨率深度图，深度值 = 一个**非单调**的函数（保证"最小值不在组内第一格"）。
std::vector<float> MakeDepth(int w, int h) {
    std::vector<float> d(static_cast<size_t>(w) * h, 1.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 0.1..0.9 之间来回摆：组内最小者落在哪一格取决于坐标，不会恒等于 base。
            d[static_cast<size_t>(y) * w + x] = 0.1f + 0.8f * std::fabs(std::sin(1.7f * x + 2.3f * y));
        }
    }
    return d;
}

TEST(SsrDownsample, PicksMinDepthOfEachGroup) {
    constexpr int kW = 8;
    constexpr int kH = 6;
    const std::vector<float> depth = MakeDepth(kW, kH);
    auto at = [&](int x, int y) { return depth[static_cast<size_t>(y) * kW + x]; };

    for (int scale : {1, 2, 3, 4}) {
        int dw = 0;
        int dh = 0;
        DownsampleTargetSize(kW, kH, scale, &dw, &dh);
        for (int dy = 0; dy < dh; ++dy) {
            for (int dx = 0; dx < dw; ++dx) {
                const DownsamplePick p = PickSourceTexel(dx, dy, scale, kW, kH, at);
                // ① 选出来的必须是组内最小（逐个比）。
                float best = 2.0f;
                for (int j = 0; j < scale; ++j) {
                    for (int i = 0; i < scale; ++i) {
                        const int sx = std::min(std::max(dx * scale + i, 0), kW - 1);
                        const int sy = std::min(std::max(dy * scale + j, 0), kH - 1);
                        best = std::min(best, at(sx, sy));
                    }
                }
                EXPECT_FLOAT_EQ(p.depth, best) << "scale=" << scale << " 组(" << dx << "," << dy
                                               << ") 没取到最小深度";
                // ② 坐标必须仍落在这一组里（否则三张图会跨组采样）。
                EXPECT_GE(p.x, std::min(dx * scale, kW - 1));
                EXPECT_LT(p.x, std::min(dx * scale + scale, kW));
                EXPECT_GE(p.y, std::min(dy * scale, kH - 1));
                EXPECT_LT(p.y, std::min(dy * scale + scale, kH));
            }
        }
    }
}

// 负向对照：如果实现退化成"永远取组内第一格"，这条必须红。
TEST(SsrDownsample, PicksAreNotAlwaysTheFirstTexel) {
    constexpr int kW = 8;
    constexpr int kH = 6;
    const std::vector<float> depth = MakeDepth(kW, kH);
    auto at = [&](int x, int y) { return depth[static_cast<size_t>(y) * kW + x]; };

    int non_first = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
            const DownsamplePick p = PickSourceTexel(dx, dy, 2, kW, kH, at);
            if (p.x != dx * 2 || p.y != dy * 2) {
                ++non_first;
            }
        }
    }
    EXPECT_GT(non_first, 6) << "选点看起来恒取组内第一格（规则退化）";
}

// 1:1 必须逐像素等价（scale=1 时"选点"就是它自己）。
TEST(SsrDownsample, ScaleOneIsIdentity) {
    constexpr int kW = 5;
    constexpr int kH = 3;
    const std::vector<float> depth = MakeDepth(kW, kH);
    auto at = [&](int x, int y) { return depth[static_cast<size_t>(y) * kW + x]; };
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const DownsamplePick p = PickSourceTexel(x, y, 1, kW, kH, at);
            EXPECT_EQ(p.x, x);
            EXPECT_EQ(p.y, y);
            EXPECT_FLOAT_EQ(p.depth, at(x, y));
        }
    }
}

}  // namespace
}  // namespace jpov
