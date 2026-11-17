// JPOV 水体介质 CPU 单测（GL-free）—— 锁住闭式解的物理与边界。
//
// 为什么值得单测：合成趟里看到的只是 (τ, S) 两个数；它们的**推导**（半空间求交 /
// 太阳折射路径 / 两个指数合并 / 极限处理）全是纯数学，可在 CPU 上独立验证。
// 这里刻意包含一条**独立数值积分**用例：把物理积分用 200k 步 Simpson 算一遍，
// 与闭式解对比 —— 这样"闭式推错一个系数"这种错不会靠"图看着还行"漏过去。
//
// 常量与语义见 src/water/water_medium_math.h 顶部。

#include "tools/jpov/src/water/water_medium_math.h"

#include <cmath>

#include <gtest/gtest.h>

namespace {

using jpov::Vec3f;
using jpov::water::DecayLength;
using jpov::water::RefractTowardSun;
using jpov::water::WaterAt;
using jpov::water::WaterMediumSample;

// 纯水 20°C 的 σa（650/550/450nm，Pope & Fry 1997 量级）+ 一点散射。
WaterMediumSample PureWater() {
    WaterMediumSample m;
    m.level_y = 0.0f;
    m.ior = 1.333f;
    m.sigma_a = {0.349f, 0.054f, 0.014f};
    m.sigma_s = {0.05f, 0.05f, 0.05f};
    m.phase_g = 0.35f;
    m.ambient = {0.30f, 0.32f, 0.35f};
    m.sun_color = {3.0f, 3.0f, 3.0f};
    m.sun_toward = {0.0f, 1.0f, 0.0f};   // 正上方（太阳在天顶）
    m.has_sun = true;
    return m;
}

Vec3f MakeDir(float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    return {x / len, y / len, z / len};
}

// 独立数值验证用：物理积分（不用任何闭式）。
//   S = ∫ σs·p(cosΘ)·L0·e^{−σt·(t−t0)}·e^{−σt·d(t)/u_w.y} dt
double NumericSunTerm(const WaterMediumSample& m, const Vec3f& ro, const Vec3f& rd,
                      float t0, float t1, float d0, const Vec3f& toward, int channel) {
    const float cos_i = m.sun_toward.y();
    const float fresnel = jpov::water::FresnelDielectric(cos_i, 1.0f, m.ior);
    const float gain = (1.0f - fresnel) / (m.ior * m.ior);
    const float cos_theta = rd.Dot(toward);
    const float phase = jpov::water::HgPhase(std::max(-1.0f, std::min(1.0f, cos_theta)),
                                             m.phase_g);
    const float sa = channel == 0 ? m.sigma_a.x() : (channel == 1 ? m.sigma_a.y()
                                                                 : m.sigma_a.z());
    const float ss = channel == 0 ? m.sigma_s.x() : (channel == 1 ? m.sigma_s.y()
                                                                 : m.sigma_s.z());
    const float sun = channel == 0 ? m.sun_color.x()
                                   : (channel == 1 ? m.sun_color.y() : m.sun_color.z());
    const float st = sa + ss;
    // 水内真实方向（与 WaterAt 的 L0 同一条规则）：相机在水上时视线折射。
    double dy_w = rd.y();
    if (ro.y() > m.level_y) {
        const double sin2_i = std::max(0.0, 1.0 - static_cast<double>(rd.y()) * rd.y());
        const double sin2_w = sin2_i / (static_cast<double>(m.ior) * m.ior);
        dy_w = -std::sqrt(std::max(0.0, 1.0 - sin2_w));
    }
    const double len_w = (static_cast<double>(t1) - t0) *
                         (std::fabs(static_cast<double>(rd.y())) / std::fabs(dy_w));
    const int kSteps = 200000;
    const double h = len_w / kSteps;
    double sum = 0.0;
    for (int i = 0; i <= kSteps; ++i) {
        const double sp = i * h;                       // 沿真实路径的长度
        const double d = d0 - dy_w * sp;               // 该点的深度（dy_w<0 ⇒ 变深）
        const double e_view = std::exp(-static_cast<double>(st) * sp);
        const double e_sun = std::exp(-static_cast<double>(st) * d / toward.y());
        const double w = (i == 0 || i == kSteps) ? 1.0 : (i % 2 == 0 ? 2.0 : 4.0);
        sum += w * e_view * e_sun;
    }
    const double integral = sum * h / 3.0;   // Simpson（沿真实路径）
    return static_cast<double>(ss) * sun * phase * gain * integral;
}

}  // namespace

// σ = 0 ⇒ 恒等（τ = S = 0）：这条保证"关介质"是零回归，而不是近似为零。
TEST(WaterMediumMath, ZeroSigmaIsIdentity) {
    WaterMediumSample m = PureWater();
    m.sigma_a = {0.0f, 0.0f, 0.0f};
    m.sigma_s = {0.0f, 0.0f, 0.0f};
    Vec3f tau;
    Vec3f S;
    WaterAt(m, /*ro*/ {0.0f, 5.0f, 0.0f}, /*rd*/ {0.0f, -1.0f, 0.0f}, /*z*/ 20.0f,
            &tau, &S);
    EXPECT_EQ(tau.x(), 0.0f);
    EXPECT_EQ(tau.y(), 0.0f);
    EXPECT_EQ(tau.z(), 0.0f);
    EXPECT_EQ(S.x(), 0.0f);
    EXPECT_EQ(S.y(), 0.0f);
    EXPECT_EQ(S.z(), 0.0f);
}

// τ 就是「水里那段长度 × σt」：相机在 +5m、直视下方、看到 10m 处 ⇒ 水段 = 5m。
TEST(WaterMediumMath, TauIsSigmaTimesPathInsideWater) {
    WaterMediumSample m = PureWater();
    Vec3f tau;
    Vec3f S;
    WaterAt(m, /*ro*/ {0.0f, 5.0f, 0.0f}, /*rd*/ {0.0f, -1.0f, 0.0f}, /*z*/ 10.0f,
            &tau, &S);
    EXPECT_NEAR(tau.x(), (m.sigma_a.x() + m.sigma_s.x()) * 5.0f, 1e-5f);
    EXPECT_NEAR(tau.y(), (m.sigma_a.y() + m.sigma_s.y()) * 5.0f, 1e-5f);
    EXPECT_NEAR(tau.z(), (m.sigma_a.z() + m.sigma_s.z()) * 5.0f, 1e-5f);
}

// 水段为空（物体在水面之上）⇒ 恒等。
TEST(WaterMediumMath, SegmentAboveWaterIsIdentity) {
    WaterMediumSample m = PureWater();
    Vec3f tau;
    Vec3f S;
    // 相机在 +5m，看到 3m 处的岛顶（水面交点其实在 5m 处）
    WaterAt(m, {0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, /*z*/ 3.0f, &tau, &S);
    EXPECT_EQ(tau.x(), 0.0f);
    EXPECT_EQ(S.x(), 0.0f);
    EXPECT_EQ(S.z(), 0.0f);
}

// 相机在水下向上看：水段到**水面**为止（再往上就是空气，不能再积分）。
TEST(WaterMediumMath, UnderwaterRayStopsAtSurface) {
    WaterMediumSample m = PureWater();
    Vec3f tau;
    Vec3f S;
    // 相机在 −3m，向上，看到 100m 处（= 天空）⇒ 水段只有 3m
    WaterAt(m, {0.0f, -3.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, /*z*/ 100.0f, &tau, &S);
    EXPECT_NEAR(tau.y(), (m.sigma_a.y() + m.sigma_s.y()) * 3.0f, 1e-5f);
}

// 闭式解 vs 200k 步 Simpson：太阳不在天顶、视线斜着 —— 这样 B ≠ 1、d0 ≠ 0，
// 两个指数合并的系数只要错一点就会被抓出来。
TEST(WaterMediumMath, ClosedFormMatchesNumericIntegration) {
    WaterMediumSample m = PureWater();
    // 只留直射项（ambient = 0）：否则环境项在数值上会盖过太阳项（~10:1），
    // 这个用例就变成在验环境积分了。
    m.ambient = {0.0f, 0.0f, 0.0f};
    m.sun_color = {50.0f, 50.0f, 50.0f};
    m.sun_toward = MakeDir(0.35f, 0.80f, 0.20f);   // 斜射（仰角 ≈53°）
    const Vec3f ro = {0.0f, 4.0f, 0.0f};
    const Vec3f rd = MakeDir(0.30f, -0.90f, 0.15f);   // 斜向下
    const float z = 12.0f;

    // 与 WaterAt 相同的段（重算一遍，确保数值积分用的是同一段与同一 d0）
    const float t0 = (m.level_y - ro.y()) / rd.y();
    const float t1 = z;
    const float d0 = m.level_y - (ro.y() + rd.y() * t0);
    Vec3f toward;
    ASSERT_TRUE(RefractTowardSun(m.sun_toward, m.ior, &toward));
    ASSERT_GT(t0, 0.0f);
    ASSERT_LT(t0, t1);

    Vec3f tau;
    Vec3f S;
    WaterAt(m, ro, rd, z, &tau, &S);

    // WaterAt 的 S 含环境项；环境项的闭式同样用 Simpson 验（B = 1 的情形）。
    const float amb_t = jpov::water::kAmbientInterfaceTransmittance;
    for (int ch = 0; ch < 3; ++ch) {
        const double sun_numeric = NumericSunTerm(m, ro, rd, t0, t1, d0, toward, ch);
        const float st = (ch == 0 ? m.sigma_a.x() + m.sigma_s.x()
                                  : (ch == 1 ? m.sigma_a.y() + m.sigma_s.y()
                                             : m.sigma_a.z() + m.sigma_s.z()));
        const float ss = ch == 0 ? m.sigma_s.x() : (ch == 1 ? m.sigma_s.y() : m.sigma_s.z());
        const float amb = ch == 0 ? m.ambient.x() : (ch == 1 ? m.ambient.y() : m.ambient.z());
        double dy_w2 = rd.y();
        if (ro.y() > m.level_y) {
            const double sin2_i = std::max(0.0, 1.0 - static_cast<double>(rd.y()) * rd.y());
            const double sin2_w = sin2_i / (static_cast<double>(m.ior) * m.ior);
            dy_w2 = -std::sqrt(std::max(0.0, 1.0 - sin2_w));
        }
        const double len_w2 = (static_cast<double>(t1) - t0) *
                              (std::fabs(static_cast<double>(rd.y())) / std::fabs(dy_w2));
        const double amb_numeric =
            static_cast<double>(ss) * amb * amb_t *
            (1.0 - std::exp(-static_cast<double>(st) * len_w2)) / st;
        const double expected = sun_numeric + amb_numeric;
        const double actual = ch == 0 ? S.x() : (ch == 1 ? S.y() : S.z());
        // 防止“直射项为 0 ⇒ 比的是环境项”这种空测：直射项必须真的非零且占主导。
        EXPECT_GT(sun_numeric, 1.0e-6) << "channel " << ch;
        EXPECT_GT(sun_numeric, 0.9 * expected) << "channel " << ch;
        // 相对误差 1e-3（闭式与 Simpson 都是双精度，差异只来自步长）
        EXPECT_NEAR(actual, expected, std::max(1e-4, std::fabs(expected) * 1.0e-3))
            << "channel " << ch;
    }
}

// 「红光先被吃掉」：同样路径长度，红通道透射率远低于蓝通道，且 S 也红 < 蓝
// （因为沿程的太阳光也被水吸收了）。这是"水自己变蓝"的物理来源，不是后期染色。
TEST(WaterMediumMath, RedAttenuatesFasterThanBlue) {
    WaterMediumSample m = PureWater();
    const Vec3f ro = {0.0f, 2.0f, 0.0f};
    const Vec3f rd = {0.0f, -1.0f, 0.0f};
    Vec3f tau_near;
    Vec3f S_near;
    Vec3f tau_far;
    Vec3f S_far;
    WaterAt(m, ro, rd, /*z*/ 4.0f, &tau_near, &S_near);     // 水段 2m
    WaterAt(m, ro, rd, /*z*/ 12.0f, &tau_far, &S_far);      // 水段 10m

    EXPECT_GT(tau_far.x(), tau_far.z()) << "红通道光学厚度必须大于蓝通道";
    // 透射率：10m 后红光几乎全没（e^−4 ≈ 0.018），蓝光还留了大部分（e^−0.64 ≈ 0.53）
    const float t_red = std::exp(-tau_far.x());
    const float t_blue = std::exp(-tau_far.z());
    EXPECT_LT(t_red, 0.05f);
    EXPECT_GT(t_blue, 0.45f);
    // 入射太阳光被吸收 ⇒ 红通道注入反而更少（S 红 < S 蓝）
    EXPECT_LT(S_far.x(), S_far.z());
    // S 是**累积**注入：路径越长（直到饱和）越多，蓝通道两者都还没饱和 ⇒ 更深更亮
    EXPECT_GT(S_far.z(), S_near.z());
}

// 无太阳 ⇒ 只剩环境项（τ 不变）。
TEST(WaterMediumMath, NoSunIsAmbientOnly) {
    WaterMediumSample with_sun = PureWater();
    WaterMediumSample no_sun = PureWater();
    no_sun.has_sun = false;
    Vec3f tau_a;
    Vec3f S_a;
    Vec3f tau_b;
    Vec3f S_b;
    WaterAt(with_sun, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau_a, &S_a);
    WaterAt(no_sun, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau_b, &S_b);
    EXPECT_NEAR(tau_a.x(), tau_b.x(), 1e-6f);
    EXPECT_GT(S_a.x(), S_b.x());   // 有太阳时多出直射项
    EXPECT_GT(S_b.x(), 0.0f);      // 环境项不为零
}

// 没有任何光（ambient = 0 且无太阳）⇒ 注入项必须**恒为零**，只剩吸收（tau > 0）。
// 对应「没配光就该暗」：介质不得自己造出亮度（以前 ambient 会从 sky 推导，
// 没配 ambient 也有天光注入）。
TEST(WaterMediumMath, NoLightMeansNoInjection) {
    WaterMediumSample m = PureWater();
    m.ambient = {0.0f, 0.0f, 0.0f};
    m.has_sun = false;
    m.sun_color = {0.0f, 0.0f, 0.0f};
    Vec3f tau;
    Vec3f S;
    WaterAt(m, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau, &S);
    EXPECT_GT(tau.x(), 0.0f);   // 吸收还在（背景会被衰减）
    EXPECT_FLOAT_EQ(S.x(), 0.0f);
    EXPECT_FLOAT_EQ(S.y(), 0.0f);
    EXPECT_FLOAT_EQ(S.z(), 0.0f);
}

// 太阳在地平线及以下 ⇒ 水下没有直射项（不是"照得很斜"，是"没有"）。
TEST(WaterMediumMath, SunBelowHorizonHasNoDirectTerm) {
    WaterMediumSample m = PureWater();
    m.sun_toward = {0.6f, -0.8f, 0.0f};   // 在下半球
    RefractTowardSun(m.sun_toward, m.ior, nullptr);   // 不 crash
    Vec3f toward;
    EXPECT_FALSE(RefractTowardSun(m.sun_toward, m.ior, &toward));

    Vec3f tau;
    Vec3f S;
    WaterAt(m, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau, &S);
    EXPECT_GT(tau.z(), 0.0f);

    // 与“明确无太阳”逐通道完全相等（太阳在地平线下 ≠ 照得很斜）。
    WaterMediumSample no_sun = m;
    no_sun.has_sun = false;
    Vec3f tau_b;
    Vec3f S_b;
    WaterAt(no_sun, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau_b, &S_b);
    EXPECT_FLOAT_EQ(S.x(), S_b.x());
    EXPECT_FLOAT_EQ(S.y(), S_b.y());
    EXPECT_FLOAT_EQ(S.z(), S_b.z());
    EXPECT_GT(S.z(), 0.0f);   // 环境项仍在
}

// 折射：结果在单位球上，且水平分量按 sinθw/sinθi = 1/n 缩放（Snell）。
TEST(WaterMediumMath, RefractionIsUnitAndFollowsSnell) {
    const float ior = 1.333f;
    const Vec3f air = MakeDir(0.5f, 0.8f, 0.33f);
    Vec3f wet;
    ASSERT_TRUE(RefractTowardSun(air, ior, &wet));
    const float len = std::sqrt(wet.x() * wet.x() + wet.y() * wet.y() + wet.z() * wet.z());
    EXPECT_NEAR(len, 1.0f, 1e-5f);
    // sinθi / sinθw = n ⇒ 水平分量比 = sinθw/sinθi = 1/n
    const float sin_i = std::sqrt(std::max(0.0f, 1.0f - air.y() * air.y()));
    const float sin_w = std::sqrt(std::max(0.0f, wet.x() * wet.x() + wet.z() * wet.z()));
    EXPECT_NEAR(sin_i / sin_w, ior, 1e-4f);
    EXPECT_GT(wet.y(), air.y()) << "折射后更靠近竖直（cosθw > cosθi）";
}

// DecayLength：x → 0 用级数（不产生 0/0），x 大时收敛到 1/(σ·rate)。
TEST(WaterMediumMath, DecayLengthLimits) {
    const float len = 3.0f;
    EXPECT_NEAR(DecayLength(/*sigma*/ 0.0f, /*rate*/ 1.0f, len), len, 1e-6f);
    EXPECT_NEAR(DecayLength(/*sigma*/ 1.0f, /*rate*/ 0.0f, len), len, 1e-6f);
    // 薄极限：σ·len ≪ 1 ⇒ ≈ len
    EXPECT_NEAR(DecayLength(1e-6f, 1.0f, len), len, 1e-5f);
    // 厚极限：≈ 1/σ
    EXPECT_NEAR(DecayLength(10.0f, 1.0f, len), 1.0f / 10.0f, 1e-6f);
    // 一般情形与解析式一致
    const float sigma = 0.7f;
    const float rate = 1.8f;
    const float expected = (1.0f - std::exp(-sigma * rate * len)) / (sigma * rate);
    EXPECT_NEAR(DecayLength(sigma, rate, len), expected, 1e-6f);
}

// L0：水上视线在水里的**路径长度**要按折射后的角度算（τ 比值 = cosθi/cosθw < 1）。
// 这条守的是"掠射时不要多算 50% 衰减"——曾经的实现偏差（见 known_issue §11.4）。
TEST(WaterMediumMath, RefractedViewPathIsShorter) {
    WaterMediumSample m = PureWater();
    m.has_sun = false;   // 只看 τ，不看 S
    const Vec3f ro = {0.0f, 2.0f, 0.0f};
    // 斜 60° 入射（rd.y = −cos60 = −0.5）
    const Vec3f rd = {0.8660254f, -0.5f, 0.0f};
    Vec3f tau;
    Vec3f S;
    WaterAt(m, ro, rd, /*z*/ 12.0f, &tau, &S);
    const float t0 = (m.level_y - ro.y()) / rd.y();       // 水面交点（直走参数）
    const float straight = 12.0f - t0;
    // cosθw = sqrt(1 − (sinθi/n)²)，sinθi = 0.8660254
    const float sin2_w = (1.0f - rd.y() * rd.y()) / (m.ior * m.ior);
    const float cos_w = std::sqrt(1.0f - sin2_w);
    const float expect_ratio = std::fabs(rd.y()) / cos_w;   // < 1
    EXPECT_LT(expect_ratio, 1.0f);
    const float st = m.sigma_a.x() + m.sigma_s.x();
    EXPECT_NEAR(tau.x(), st * straight * expect_ratio, 1e-4f);
    // 竖直视线（θi=0）不受影响：折射不改路径
    Vec3f tau_v;
    Vec3f S_v;
    WaterAt(m, {0.0f, 2.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 12.0f, &tau_v, &S_v);
    EXPECT_NEAR(tau_v.x(), st * 10.0f, 1e-4f);
}

// 空指针不 crash（防御式：公开函数）。
TEST(WaterMediumMath, NullOutputsDoNotCrash) {
    WaterMediumSample m = PureWater();
    WaterAt(m, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, nullptr, nullptr);
    Vec3f tau;
    WaterAt(m, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, &tau, nullptr);
    Vec3f S;
    WaterAt(m, {0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 8.0f, nullptr, &S);
    RefractTowardSun({0.0f, 1.0f, 0.0f}, 1.333f, nullptr);
}
