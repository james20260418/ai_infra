// 覆盖率保持 alpha mip 链单测 —— GL-free、纯 CPU。
//
// 验证 tools/jpov/src/coverage_mipmap.h 的 BuildCoverageMipChain：
//   ① level0 逐字节保留（近景零回归）；
//   ② 每级 alpha 二值化（仅 0/255）；
//   ③ 每级覆盖率（alpha>=cutoff 的纹素占比）恒等于 mip0 —— 且**确实优于**朴素盒式平均
//      （后者会把稀疏结构的覆盖率塌到 0，正是要被修掉的 bug —— 这一条保证测试非恒真）；
//   ④ 层级尺寸遵循 GL 归约规则 max(1, floor(尺寸/2))，到 1x1 为止；
//   ⑤ 全透明 / 全不透明边界；
//   ⑥ 同 alpha 临界档用 Bayer 抖散（不聚堆）。

#include "tools/jpov/src/coverage_mipmap.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr float kCutoff = 0.5f;

// 用 alpha_at(x,y) 生成 w*h RGBA（RGB 填固定色，便于校验 level0 逐字节保留）。
template <typename F>
std::vector<unsigned char> MakeRgba(int w, int h, F alpha_at) {
    std::vector<unsigned char> v(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 4;
            v[o + 0] = 200;
            v[o + 1] = 180;
            v[o + 2] = 160;
            v[o + 3] = static_cast<unsigned char>(alpha_at(x, y));
        }
    }
    return v;
}

// 某级「可见」纹素数（alpha/255 >= cutoff）。
int CountVisible(const std::vector<unsigned char>& rgba, float cutoff) {
    int c = 0;
    for (size_t i = 0; i < rgba.size() / 4; ++i) {
        if (static_cast<float>(rgba[i * 4 + 3]) / 255.0f >= cutoff) {
            ++c;
        }
    }
    return c;
}

bool AllBinaryAlpha(const std::vector<unsigned char>& rgba) {
    for (size_t i = 0; i < rgba.size() / 4; ++i) {
        const unsigned char a = rgba[i * 4 + 3];
        if (a != 0 && a != 255) {
            return false;
        }
    }
    return true;
}

// 朴素（GL 语义）alpha 盒式链：仅用于对照，证明覆盖率确实会塌。
std::vector<std::vector<unsigned char>> NaiveAlphaLevels(
    const std::vector<unsigned char>& rgba0, int w, int h) {
    std::vector<std::vector<unsigned char>> levels;
    std::vector<unsigned char> a(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = rgba0[i * 4 + 3];
    }
    levels.push_back(a);
    int cw = w;
    int ch = h;
    while (cw > 1 || ch > 1) {
        const int dw = (cw > 1) ? cw / 2 : 1;
        const int dh = (ch > 1) ? ch / 2 : 1;
        std::vector<unsigned char> d(static_cast<size_t>(dw) * dh, 0);
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                int sum = 0;
                int n = 0;
                for (int oy = 0; oy < 2; ++oy) {
                    const int sy = 2 * y + oy;
                    if (sy >= ch) {
                        continue;
                    }
                    for (int ox = 0; ox < 2; ++ox) {
                        const int sx = 2 * x + ox;
                        if (sx >= cw) {
                            continue;
                        }
                        sum += levels.back()[static_cast<size_t>(sy) * cw + sx];
                        ++n;
                    }
                }
                d[static_cast<size_t>(y) * dw + x] =
                    static_cast<unsigned char>((sum + n / 2) / n);
            }
        }
        levels.push_back(std::move(d));
        cw = dw;
        ch = dh;
    }
    return levels;
}

float NaiveCoverage(const std::vector<unsigned char>& alpha_level, float cutoff) {
    int c = 0;
    for (unsigned char a : alpha_level) {
        if (static_cast<float>(a) / 255.0f >= cutoff) {
            ++c;
        }
    }
    return static_cast<float>(c) / static_cast<float>(alpha_level.size());
}

}  // namespace

// ① level0 逐字节保留。
TEST(CoverageMipmap, Level0PreservedByteForByte) {
    const int w = 32;
    const int h = 16;
    std::vector<unsigned char> in = MakeRgba(w, h, [](int x, int y) {
        return ((x * 7 + y * 3) % 5 == 0) ? 255 : 0;
    });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);
    ASSERT_FALSE(chain.empty());
    EXPECT_EQ(chain[0].width, w);
    EXPECT_EQ(chain[0].height, h);
    EXPECT_EQ(chain[0].rgba, in);   // 全通道逐字节一致（近景零回归）
}

// ② 每级 alpha 二值（level>=1）。
TEST(CoverageMipmap, AlphaBinaryizedEveryLevel) {
    const int w = 64;
    const int h = 64;
    std::vector<unsigned char> in =
        MakeRgba(w, h, [](int x, int y) { return (x % 2 == 0 && y % 2 == 0) ? 255 : 0; });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);
    for (size_t level = 1; level < chain.size(); ++level) {
        EXPECT_TRUE(AllBinaryAlpha(chain[level].rgba)) << "level " << level;
    }
}

// ③ 关键非恒真：稀疏结构下朴素平均把覆盖率塌到 0，覆盖率保持链不塌。
TEST(CoverageMipmap, SparseStructureNaiveCollapsesButCoveragePreserved) {
    const int w = 64;
    const int h = 64;
    // 每 2x2 恰好 1 个不透明纹素 → 密度 0.25；朴素平均每块 = 64/255 < 0.5 → 全被裁掉。
    std::vector<unsigned char> in =
        MakeRgba(w, h, [](int x, int y) { return (x % 2 == 0 && y % 2 == 0) ? 255 : 0; });

    const int visible0 = CountVisible(in, kCutoff);
    const float coverage0 = static_cast<float>(visible0) / static_cast<float>(w * h);
    ASSERT_NEAR(coverage0, 0.25f, 1e-6f);

    // 朴素链：level1 覆盖率应塌到 ~0（证明 bug 真实存在，测试非恒真）。
    std::vector<std::vector<unsigned char>> naive = NaiveAlphaLevels(in, w, h);
    ASSERT_GE(naive.size(), 2u);
    EXPECT_LT(NaiveCoverage(naive[1], kCutoff), 0.05f)
        << "朴素平均应把稀疏结构覆盖率塌掉（参照系）";

    // 覆盖率保持链：每级可见数 == round(coverage0 * N)。
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);
    for (size_t level = 1; level < chain.size(); ++level) {
        const int n = chain[level].width * chain[level].height;
        const int expect = static_cast<int>(coverage0 * n + 0.5f);
        EXPECT_EQ(CountVisible(chain[level].rgba, kCutoff), expect)
            << "level " << level << " 覆盖率应保持";
    }
}

// ④ 层级尺寸遵循 GL 归约规则 max(1, floor(尺寸/2))，到 1x1 为止（NPOT）。
TEST(CoverageMipmap, DimsFollowGlReductionRule) {
    const int w = 60;
    const int h = 40;
    std::vector<unsigned char> in =
        MakeRgba(w, h, [](int x, int y) { return ((x + y) % 3 == 0) ? 255 : 0; });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);

    const std::vector<std::pair<int, int>> expect = {
        {60, 40}, {30, 20}, {15, 10}, {7, 5}, {3, 2}, {1, 1},
    };
    ASSERT_EQ(chain.size(), expect.size());
    for (size_t i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(chain[i].width, expect[i].first) << "level " << i;
        EXPECT_EQ(chain[i].height, expect[i].second) << "level " << i;
        EXPECT_EQ(chain[i].rgba.size(),
                  static_cast<size_t>(expect[i].first) * expect[i].second * 4);
    }
}

// ⑤a 全透明 → 每级全透明。
TEST(CoverageMipmap, AllTransparentStaysTransparent) {
    const int w = 32;
    const int h = 32;
    std::vector<unsigned char> in = MakeRgba(w, h, [](int, int) { return 0; });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);
    for (size_t level = 0; level < chain.size(); ++level) {
        EXPECT_EQ(CountVisible(chain[level].rgba, kCutoff), 0) << "level " << level;
    }
}

// ⑤b 全不透明 → 每级全不透明（可见数 == 纹素数）。
TEST(CoverageMipmap, AllOpaqueStaysOpaque) {
    const int w = 32;
    const int h = 16;
    std::vector<unsigned char> in = MakeRgba(w, h, [](int, int) { return 255; });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);
    for (size_t level = 0; level < chain.size(); ++level) {
        const int n = chain[level].width * chain[level].height;
        EXPECT_EQ(CountVisible(chain[level].rgba, kCutoff), n) << "level " << level;
    }
}

// ⑥ 同 alpha 临界档用空间哈希抖散：选中纹素应铺满各列，而非聚成一块。
TEST(CoverageMipmap, ThresholdTieBreakSpreadsSpatially) {
    const int w = 64;
    const int h = 64;
    std::vector<unsigned char> in =
        MakeRgba(w, h, [](int x, int y) { return (x % 2 == 0 && y % 2 == 0) ? 255 : 0; });
    std::vector<jpov::MipLevel> chain =
        jpov::BuildCoverageMipChain(in.data(), w, h, kCutoff);

    // level1（32x32）：朴素降采样后所有纹素 alpha 相同（=64），是一整档平局。
    ASSERT_GE(chain.size(), 2u);
    const jpov::MipLevel& l1 = chain[1];
    std::vector<bool> col_hit(static_cast<size_t>(l1.width), false);
    for (int y = 0; y < l1.height; ++y) {
        for (int x = 0; x < l1.width; ++x) {
            if (l1.rgba[(static_cast<size_t>(y) * l1.width + x) * 4 + 3] == 255) {
                col_hit[x] = true;
            }
        }
    }
    int hit = 0;
    for (bool b : col_hit) {
        hit += b ? 1 : 0;
    }
    EXPECT_GT(hit, l1.width * 3 / 4) << "平局档应空间铺开（哈希），不应聚堆";
}
