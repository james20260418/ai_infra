// AmbientLight::IsotropicColor() 单测（纯 CPU，不渲染、不碰 GL）。
//
// 被测量：把三色环境光塌成「体积消费者（水体介质 / 雾场）用的纯色等效」。
// 物理口径：内散射源是 ∮p(ω',ω)·L(ω')dω'，各向同性相位下等于 L 的**球面平均**；
//   tricolor 的定义是 L 随方向余弦 μ=N.y 分段线性（[0,1] 天际线→天，[-1,0] 天际线→地），
//   μ 在球面上**均匀分布** ⇒ 权重 天 : 天际线 : 地 = 1 : 2 : 1。
//
// 覆盖：
//   - 无 tricolor 时逐字段返回单色 color（单色路径零改动）。
//   - 三条单位权重（只给天 / 只给天际线 / 只给地）分别落在 1/4、1/2、1/4。
//   - **独立数值积分对照**：把三色的分段线性定义直接积一遍球面，必须与实现的权重一致
//     （把权重改成 1/3 之类会立刻失败 —— 这是防“拍脑袋比例”的负向对照）。
//   - 天/地对称：只交换 [0] 与 [2] 不改变结果。
//   - 上色不代表亮度：intensity 不在本函数里乘（由调用方乘），设成任何值结果不变。
#include "tools/jpov/interface/render_command.h"

#include <array>
#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace {

// 三色的分段线性定义（**独立于实现**重写一遍，作为数值积分的被积函数）。
// 定义出处：AmbientLight::tricolor 的注释与 object3d shader 的 mix 顺序。
float TricolorAt(const std::array<Color, 3>& tri, float mu) {
    const float sky = tri[0].r;
    const float horizon = tri[1].r;
    const float ground = tri[2].r;
    if (mu >= 0.0f) {
        return horizon + (sky - horizon) * mu;
    }
    return horizon + (ground - horizon) * (-mu);
}

// 球面平均：dω = 2π·dμ，μ 在 [-1,1] 上均匀 ⇒ L̄ = ½∫₋₁¹L(μ)dμ。
// 被积函数对每半段是线性的 ⇒ 梯形法精确；仍取多点以显式表达“这是积分，不是猜比例”。
float SphericalMeanR(const std::array<Color, 3>& tri) {
    constexpr int kSamplesPerHalf = 1000;
    double sum = 0.0;
    for (int i = 0; i < kSamplesPerHalf; ++i) {
        // 每半段的中点取样（中点法对线性函数精确）。
        const double dx = 1.0 / kSamplesPerHalf;
        const double mu_up = (i + 0.5) * dx;             // (0,1)
        sum += TricolorAt(tri, static_cast<float>(mu_up)) * dx;
        sum += TricolorAt(tri, static_cast<float>(-mu_up)) * dx;   // (-1,0)
    }
    return static_cast<float>(0.5 * sum);
}

Color MakeColor(float r, float g, float b) {
    return Color{r, g, b, 1.0f};
}

TEST(AmbientLightIsotropicColor, NoTricolorReturnsSingleColorUnchanged) {
    AmbientLight a;
    a.color = MakeColor(0.31f, 0.42f, 0.53f);
    a.intensity = 0.7f;

    const Color c = a.IsotropicColor();
    EXPECT_FLOAT_EQ(c.r, 0.31f);
    EXPECT_FLOAT_EQ(c.g, 0.42f);
    EXPECT_FLOAT_EQ(c.b, 0.53f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST(AmbientLightIsotropicColor, UnitWeightsAreQuarterHalfQuarter) {
    AmbientLight a;
    const Color sky = MakeColor(0.8f, 0.6f, 0.4f);
    const Color horizon = MakeColor(0.4f, 0.3f, 0.2f);
    const Color ground = MakeColor(0.2f, 0.1f, 0.0f);

    // 只给天：权重必须是 1/4。
    a.tricolor = std::array<Color, 3>{sky, MakeColor(0.0f, 0.0f, 0.0f),
                                      MakeColor(0.0f, 0.0f, 0.0f)};
    Color c = a.IsotropicColor();
    EXPECT_FLOAT_EQ(c.r, 0.25f * sky.r);
    EXPECT_FLOAT_EQ(c.g, 0.25f * sky.g);
    EXPECT_FLOAT_EQ(c.b, 0.25f * sky.b);

    // 只给天际线：权重必须是 1/2。
    a.tricolor = std::array<Color, 3>{MakeColor(0.0f, 0.0f, 0.0f), horizon,
                                      MakeColor(0.0f, 0.0f, 0.0f)};
    c = a.IsotropicColor();
    EXPECT_FLOAT_EQ(c.r, 0.5f * horizon.r);
    EXPECT_FLOAT_EQ(c.g, 0.5f * horizon.g);
    EXPECT_FLOAT_EQ(c.b, 0.5f * horizon.b);

    // 只给地：权重必须是 1/4。
    a.tricolor = std::array<Color, 3>{MakeColor(0.0f, 0.0f, 0.0f),
                                      MakeColor(0.0f, 0.0f, 0.0f), ground};
    c = a.IsotropicColor();
    EXPECT_FLOAT_EQ(c.r, 0.25f * ground.r);
    EXPECT_FLOAT_EQ(c.g, 0.25f * ground.g);
    EXPECT_FLOAT_EQ(c.b, 0.25f * ground.b);

    // 三色齐全：1/4·天 + 1/2·天际线 + 1/4·地。
    a.tricolor = std::array<Color, 3>{sky, horizon, ground};
    c = a.IsotropicColor();
    EXPECT_NEAR(c.r, 0.25f * sky.r + 0.5f * horizon.r + 0.25f * ground.r, 1e-6);
    EXPECT_NEAR(c.g, 0.25f * sky.g + 0.5f * horizon.g + 0.25f * ground.g, 1e-6);
    EXPECT_NEAR(c.b, 0.25f * sky.b + 0.5f * horizon.b + 0.25f * ground.b, 1e-6);
}

// 负向对照：把权重改成「算术平均 1/3 各一」会在这里失败。
TEST(AmbientLightIsotropicColor, MatchesIndependentSphericalIntegral) {
    const std::array<Color, 3> tri{
        MakeColor(0.90f, 0.80f, 0.70f),
        MakeColor(0.62f, 0.45f, 0.30f),   // 天际线刻意**不等于**天/地中点，
        MakeColor(0.10f, 0.20f, 0.30f),   // 否则 1:2:1 与算术平均会碰巧相等，这条断言就测不到东西。
    };
    AmbientLight a;
    a.tricolor = tri;
    const Color got = a.IsotropicColor();

    const float expected = SphericalMeanR(tri);
    EXPECT_NEAR(got.r, expected, 1e-6);
    // 与算术平均**必须不同**（否则说明测的还是 1/3 各一，测不到东西）。
    const float arithmetic = (tri[0].r + tri[1].r + tri[2].r) / 3.0f;
    EXPECT_GT(std::abs(expected - arithmetic), 1e-4);
}

TEST(AmbientLightIsotropicColor, SkyAndGroundAreSymmetric) {
    AmbientLight a;
    a.tricolor = std::array<Color, 3>{MakeColor(0.9f, 0.7f, 0.5f),
                                      MakeColor(0.4f, 0.4f, 0.4f),
                                      MakeColor(0.1f, 0.2f, 0.3f)};
    const Color c1 = a.IsotropicColor();

    a.tricolor = std::array<Color, 3>{MakeColor(0.1f, 0.2f, 0.3f),
                                      MakeColor(0.4f, 0.4f, 0.4f),
                                      MakeColor(0.9f, 0.7f, 0.5f)};
    const Color c2 = a.IsotropicColor();
    EXPECT_FLOAT_EQ(c1.r, c2.r);
    EXPECT_FLOAT_EQ(c1.g, c2.g);
    EXPECT_FLOAT_EQ(c1.b, c2.b);
}

TEST(AmbientLightIsotropicColor, TricolorIgnoresSingleColorField) {
    AmbientLight a;
    a.color = MakeColor(1.0f, 1.0f, 1.0f);
    a.tricolor = std::array<Color, 3>{MakeColor(0.2f, 0.2f, 0.2f),
                                      MakeColor(0.2f, 0.2f, 0.2f),
                                      MakeColor(0.2f, 0.2f, 0.2f)};
    const Color c = a.IsotropicColor();
    EXPECT_NEAR(c.r, 0.2f, 1e-6);   // 设了 tricolor 就完全不看 color
}

TEST(AmbientLightIsotropicColor, DoesNotApplyIntensity) {
    AmbientLight a;
    a.tricolor = std::array<Color, 3>{MakeColor(0.6f, 0.5f, 0.4f),
                                      MakeColor(0.3f, 0.3f, 0.3f),
                                      MakeColor(0.1f, 0.1f, 0.1f)};
    a.intensity = 0.25f;
    const Color small = a.IsotropicColor();
    a.intensity = 7.0f;
    const Color big = a.IsotropicColor();
    EXPECT_FLOAT_EQ(small.r, big.r);
    EXPECT_FLOAT_EQ(small.g, big.g);
    EXPECT_FLOAT_EQ(small.b, big.b);
}

}  // namespace
}  // namespace jpov
