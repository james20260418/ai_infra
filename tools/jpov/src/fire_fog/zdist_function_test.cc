// ZDist CPU 参考实现 单元测试
//
// 覆盖：
//   1. 单段均匀雾 → 闭式解（T=exp(−σL)、S=c(1−T)）
//   2. 段加法：线性（一段 = 两半之和）、乱序可加（免排序）、域外裁掉
//   3. 降采样 Reduce：端点保留 ⇒ 总上升量守恒；单调性保持；控制点数 ≤8
//   4. 末端积分：CPU 闭式 vs 高分辨率数值积分；降采样后 vs 精确（误差有界）
//   5. 纹理布局：Pack → Unpack 往返一致

#include "tools/jpov/src/fire_fog/zdist_function.h"

#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "tools/jpov/src/fire_fog/zdist_texture_layout.h"

namespace jpov {
namespace {

// 确定性伪随机（splitmix64），跨平台一致。
unsigned long long g_state = 0x1234567890abcdefULL;
double NextUnit() {
    g_state += 0x9e3779b97f4a7c15ULL;
    unsigned long long z = g_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return static_cast<double>(z >> 11) / 9007199254740992.0;  // [0,1)
}

Vec3f V3(float r, float g, float b) { return Vec3f(r, g, b); }

// ── 1. 单段均匀雾 → 闭式 ──
TEST(ZDistTest, UniformSegmentMatchesClosedForm) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 10.0);
    acc.AddSegment(ZDistSegment{0.0, 10.0, 0.3, V3(1.0f, 0.5f, 0.2f)});

    float T = 0.0f;
    Vec3f S(0, 0, 0);
    Vec3f L(0, 0, 0);
    acc.Integrate(V3(2.0f, 2.0f, 2.0f), &T, &S, &L);

    const float T_expected = std::exp(-0.3f * 10.0f);  // τ = σL = 3
    EXPECT_NEAR(T, T_expected, 1e-6f);
    // S = c · (1 − T)
    EXPECT_NEAR(S[0], 1.0f * (1.0f - T_expected), 1e-6f);
    EXPECT_NEAR(S[1], 0.5f * (1.0f - T_expected), 1e-6f);
    EXPECT_NEAR(S[2], 0.2f * (1.0f - T_expected), 1e-6f);
    // L_out = L_scene·T + S
    EXPECT_NEAR(L[0], 2.0f * T_expected + S[0], 1e-6f);
}

// 末端积分闭式 vs 高分辨率数值积分（对同一精确函数做对照）。
TEST(ZDistTest, IntegrateMatchesNumericQuadrature) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 12.0);
    acc.AddSegment(ZDistSegment{1.0, 4.0, 0.25, V3(0.8f, 0.1f, 0.0f)});
    acc.AddSegment(ZDistSegment{3.0, 9.0, 0.15, V3(0.0f, 0.6f, 0.3f)});

    float T = 0.0f;
    Vec3f S(0, 0, 0);
    Vec3f L(0, 0, 0);
    acc.Integrate(V3(0, 0, 0), &T, &S, &L);

    const int kSteps = 200000;
    const double dz = (acc.z_far() - acc.z_near()) / kSteps;
    Vec3f S_num(0, 0, 0);
    for (int i = 0; i < kSteps; ++i) {
        const double z0 = acc.z_near() + i * dz;
        const double z1 = z0 + dz;
        const double tmid = std::exp(-acc.TauAt(0.5 * (z0 + z1)));
        const Vec3f dE = acc.EdAt(z1) - acc.EdAt(z0);
        S_num += dE * static_cast<float>(tmid);
    }
    EXPECT_NEAR(S[0], S_num[0], 2e-3f);
    EXPECT_NEAR(S[1], S_num[1], 2e-3f);
    EXPECT_NEAR(S[2], S_num[2], 2e-3f);
}

// ── 2. 段加法性质 ──
TEST(ZDistTest, AddIsLinearityOfSplit) {
    // 一段 [0,10] == 两段 [0,5] + [5,10]（同 σ、同 emission）。
    ZDistAccumulator a;
    a.Reset(0.0, 10.0);
    a.AddSegment(ZDistSegment{0.0, 10.0, 0.4, V3(1.0f, 1.0f, 1.0f)});

    ZDistAccumulator b;
    b.Reset(0.0, 10.0);
    b.AddSegment(ZDistSegment{0.0, 5.0, 0.4, V3(1.0f, 1.0f, 1.0f)});
    b.AddSegment(ZDistSegment{5.0, 10.0, 0.4, V3(1.0f, 1.0f, 1.0f)});

    // 两条精确函数应逐点一致（含中间 x=5）。
    for (int i = 0; i < a.size(); ++i) {
        EXPECT_DOUBLE_EQ(a.TauAt(a.z(i)), b.TauAt(a.z(i)));
    }
    EXPECT_NEAR(a.TauAt(5.0), b.TauAt(5.0), 1e-12);
    EXPECT_NEAR(a.EdAt(5.0)[0], b.EdAt(5.0)[0], 1e-5);
}

TEST(ZDistTest, AddIsOrderIndependent) {
    std::vector<ZDistSegment> segs;
    for (int i = 0; i < 12; ++i) {
        const double z0 = NextUnit() * 20.0;
        const double z1 = z0 + 0.5 + NextUnit() * 5.0;
        segs.push_back(ZDistSegment{z0, z1, 0.05 + NextUnit() * 0.4,
                                    V3(static_cast<float>(NextUnit()),
                                       static_cast<float>(NextUnit()),
                                       static_cast<float>(NextUnit()))});
    }
    ZDistAccumulator fwd;
    fwd.Reset(0.0, 25.0);
    for (const ZDistSegment& s : segs) {
        fwd.AddSegment(s);
    }
    ZDistAccumulator rev;
    rev.Reset(0.0, 25.0);
    for (int i = static_cast<int>(segs.size()) - 1; i >= 0; --i) {
        rev.AddSegment(segs[i]);
    }
    // 断点集合一致 ⇒ 逐点值一致（浮点求和次序不同，给容差）。
    ASSERT_EQ(fwd.size(), rev.size());
    for (int i = 0; i < fwd.size(); ++i) {
        EXPECT_DOUBLE_EQ(fwd.z(i), rev.z(i));
        EXPECT_NEAR(fwd.tau(i), rev.tau(i), 1e-9);
        EXPECT_NEAR(fwd.ed(i)[0], rev.ed(i)[0], 1e-5);
        EXPECT_NEAR(fwd.ed(i)[1], rev.ed(i)[1], 1e-5);
    }
}

TEST(ZDistTest, SegmentOutsideDomainIsNoop) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 10.0);
    acc.AddSegment(ZDistSegment{-5.0, -1.0, 1.0, V3(1, 1, 1)});  // 全在域外（左）
    acc.AddSegment(ZDistSegment{11.0, 20.0, 1.0, V3(1, 1, 1)});  // 全在域外（右）
    EXPECT_NEAR(acc.TauAt(10.0), 0.0, 1e-12);  // 总量仍为 0
    EXPECT_NEAR(acc.EdAt(10.0)[0], 0.0, 1e-6);
}

TEST(ZDistTest, SegmentClampedToDomain) {
    ZDistAccumulator acc;
    acc.Reset(2.0, 8.0);
    acc.AddSegment(ZDistSegment{0.0, 10.0, 0.5, V3(1, 1, 1)});  // 裁到 [2,8]
    EXPECT_NEAR(acc.TauAt(8.0), 0.5 * 6.0, 1e-12);             // σ·L = 0.5·6
}

// ── 3. Reduce：守恒 / 单调 / 点数 ──
TEST(ZDistTest, ReduceConservesTotalRiseAndKeepsEndpoints) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 24.0);
    for (int i = 0; i < 14; ++i) {
        const double z0 = NextUnit() * 20.0;
        acc.AddSegment(ZDistSegment{z0, z0 + 1.0 + NextUnit() * 4.0,
                                    0.1 + NextUnit() * 0.3,
                                    V3(0.5f, 0.7f, 0.9f)});
    }
    ASSERT_GT(acc.size(), kZDistControlPoints);  // 保证走了降采样分支

    const ZDistFunction f = ZDistFunction::Reduce(acc);
    ASSERT_EQ(f.size(), kZDistControlPoints);     // 恰好 8 个控制点
    EXPECT_DOUBLE_EQ(f.z(0), acc.z_near());       // 保留近端
    EXPECT_DOUBLE_EQ(f.z(f.size() - 1), acc.z_far());  // 保留远端
    // 总上升量守恒：τ_total / Ed_total 端点值逐位不变。
    EXPECT_DOUBLE_EQ(f.tau(f.size() - 1), acc.tau(acc.size() - 1));
    EXPECT_FLOAT_EQ(f.ed(f.size() - 1)[0], acc.ed(acc.size() - 1)[0]);
    EXPECT_FLOAT_EQ(f.ed(f.size() - 1)[1], acc.ed(acc.size() - 1)[1]);
    EXPECT_FLOAT_EQ(f.ed(f.size() - 1)[2], acc.ed(acc.size() - 1)[2]);
}

TEST(ZDistTest, ReducedSamplesMonotone) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 18.0);
    for (int i = 0; i < 10; ++i) {
        const double z0 = (i % 3) * 2.0 + NextUnit() * 8.0;
        acc.AddSegment(ZDistSegment{z0, z0 + 2.0 + NextUnit() * 3.0,
                                    0.2 + NextUnit() * 0.2, V3(1, 1, 1)});
    }
    const ZDistFunction f = ZDistFunction::Reduce(acc);
    for (int i = 1; i < f.size(); ++i) {
        EXPECT_GT(f.z(i), f.z(i - 1));            // z 严格递增
        EXPECT_GE(f.tau(i), f.tau(i - 1));        // τ 单调不减
        EXPECT_GE(f.ed(i)[0], f.ed(i - 1)[0] - 1e-5f);
        EXPECT_GE(f.ed(i)[1], f.ed(i - 1)[1] - 1e-5f);
        EXPECT_GE(f.ed(i)[2], f.ed(i - 1)[2] - 1e-5f);
    }
}

TEST(ZDistTest, ReduceSmallStaysExact) {
    // ≤8 个断点时降采样不丢信息，应与精确完全一致。
    ZDistAccumulator acc;
    acc.Reset(0.0, 10.0);
    acc.AddSegment(ZDistSegment{1.0, 3.0, 0.5, V3(1, 0, 0)});
    acc.AddSegment(ZDistSegment{4.0, 8.0, 0.25, V3(0, 1, 0)});
    ASSERT_LE(acc.size(), kZDistControlPoints);
    const ZDistFunction f = ZDistFunction::Reduce(acc);
    for (int i = 0; i < acc.size(); ++i) {
        EXPECT_DOUBLE_EQ(f.z(i), acc.z(i));
        EXPECT_DOUBLE_EQ(f.tau(i), acc.tau(i));
    }
}

TEST(ZDistTest, ReduceIntegralWithinToleranceOfExact) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 20.0);
    // 平滑一些的剖面 → 8 点降采样误差应较小。
    for (int i = 0; i < 12; ++i) {
        const double z0 = i * 1.5;
        acc.AddSegment(ZDistSegment{z0, z0 + 2.5, 0.1, V3(0.6f, 0.2f, 0.1f)});
    }
    float T0 = 0.0f;
    Vec3f S0(0, 0, 0);
    Vec3f L0(0, 0, 0);
    acc.Integrate(V3(0, 0, 0), &T0, &S0, &L0);

    const ZDistFunction f = ZDistFunction::Reduce(acc);
    float T1 = 0.0f;
    Vec3f S1(0, 0, 0);
    Vec3f L1(0, 0, 0);
    f.Integrate(V3(0, 0, 0), &T1, &S1, &L1);

    EXPECT_NEAR(T1, T0, 1e-6f);  // T 精确守恒
    const float tol = 0.03f * std::max(S0[0], 1e-3f);
    EXPECT_NEAR(S1[0], S0[0], tol);
    EXPECT_NEAR(S1[1], S0[1], tol);
    EXPECT_NEAR(S1[2], S0[2], tol);
}

// ── 5. 纹理布局往返 ──
TEST(ZDistTest, TexturePackUnpackRoundTrip) {
    ZDistAccumulator acc;
    acc.Reset(0.0, 16.0);
    for (int i = 0; i < 9; ++i) {
        const double z0 = NextUnit() * 14.0;
        acc.AddSegment(ZDistSegment{z0, z0 + 1.0 + NextUnit() * 3.0,
                                    0.2, V3(0.3f, 0.6f, 0.9f)});
    }
    const ZDistFunction f = ZDistFunction::Reduce(acc);
    float buf[ZDistTextureLayout::kFloatsPerPixel];
    PackZDist(f, buf);
    const ZDistFunction g = UnpackZDist(buf);
    ASSERT_EQ(g.size(), f.size());
    for (int i = 0; i < f.size(); ++i) {
        EXPECT_FLOAT_EQ(static_cast<float>(g.z(i)), static_cast<float>(f.z(i)));
        EXPECT_FLOAT_EQ(static_cast<float>(g.tau(i)),
                        static_cast<float>(f.tau(i)));
        EXPECT_FLOAT_EQ(g.ed(i)[0], f.ed(i)[0]);
        EXPECT_FLOAT_EQ(g.ed(i)[1], f.ed(i)[1]);
        EXPECT_FLOAT_EQ(g.ed(i)[2], f.ed(i)[2]);
    }
}

TEST(ZDistTest, TextureLayoutBudgetConstants) {
    EXPECT_EQ(ZDistTextureLayout::kPoints, 8);
    EXPECT_EQ(ZDistTextureLayout::kFloatsPerPixel, 40);
    EXPECT_EQ(ZDistTextureLayout::kTexelsPerPixel, 10);
}

}  // namespace
}  // namespace jpov
