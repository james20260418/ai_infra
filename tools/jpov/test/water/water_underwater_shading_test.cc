// 水下照明衰减（照明侧）的 CPU 参考单测。
//
// 锁的是 water_underwater_shading.h 里 `UnderwaterGains()` 的口径：
//   · 直射：E_w = E_air·(1−R)·exp(−σt·d/cosθw)   （方向不折、路径按 Snell 后的水内角）
//   · 环境：L_w = L_air·0.94·exp(−K_d·d)，K_d = σa + (1−g)·σs
// 期望值**手算**（不调用被测公式），先在水面之上/开关关闭时确认恒等，再逐通道对数值。
//
// ⚠️ GLSL 侧（kUnderwaterAttenGlsl）是同一式子的另一份实现：本测试锁不住 shader，
//    shader 由出图（skylight viewer 场景 5 的 A/B）验收。改一处必须同时改另一处。

#include "tools/jpov/src/water/water_underwater_shading.h"

#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace water {
namespace {

WaterMediumSample MakeSample() {
    WaterMediumSample m;
    m.level_y = 0.0f;
    m.ior = 1.333f;
    m.sigma_a = {0.50f, 0.054f, 0.014f};
    m.sigma_s = {0.04f, 0.04f, 0.04f};
    m.phase_g = 0.35f;
    m.has_sun = true;
    const float c = std::sqrt(0.5f);   // 45°
    m.sun_toward = {0.0f, c, -c};
    return m;
}

void ExpectVec3(const Vec3f& got, float r, float g, float b, float tol) {
    EXPECT_NEAR(got.x(), r, tol);
    EXPECT_NEAR(got.y(), g, tol);
    EXPECT_NEAR(got.z(), b, tol);
}

TEST(WaterUnderwaterShading, DisabledIsIdentity) {
    const WaterMediumSample m = MakeSample();
    const UnderwaterAttenuation a = BuildUnderwaterAttenuation(m, false, false);
    EXPECT_FALSE(a.enabled());
    Vec3f amb;
    Vec3f sun;
    UnderwaterGains(a, {0.0f, -5.0f, 0.0f}, &amb, &sun);
    ExpectVec3(amb, 1.0f, 1.0f, 1.0f, 0.0f);
    ExpectVec3(sun, 1.0f, 1.0f, 1.0f, 0.0f);
}

TEST(WaterUnderwaterShading, AboveWaterIsIdentity) {
    const WaterMediumSample m = MakeSample();
    const UnderwaterAttenuation a = BuildUnderwaterAttenuation(m, true, true);
    Vec3f amb;
    Vec3f sun;
    // 水面之上一米：无论开关怎样都必须是恒等（水上零回归的唯一保证）。
    UnderwaterGains(a, {3.0f, 1.0f, -2.0f}, &amb, &sun);
    ExpectVec3(amb, 1.0f, 1.0f, 1.0f, 0.0f);
    ExpectVec3(sun, 1.0f, 1.0f, 1.0f, 0.0f);
    // 正好在水面上（d = 0）也算水上。
    UnderwaterGains(a, {0.0f, 0.0f, 0.0f}, &amb, &sun);
    ExpectVec3(sun, 1.0f, 1.0f, 1.0f, 0.0f);
}

TEST(WaterUnderwaterShading, SunAndAmbientMatchHandComputed) {
    const WaterMediumSample m = MakeSample();
    const UnderwaterAttenuation a = BuildUnderwaterAttenuation(m, true, true);
    Vec3f amb;
    Vec3f sun;
    UnderwaterGains(a, {0.0f, -5.0f, 0.0f}, &amb, &sun);

    // 手算（5 m、太阳 45°）：
    //   σt = (0.54, 0.094, 0.054)，K_d = (0.526, 0.080, 0.040)
    //   cosθw = 0.8477075 ⇒ 路径 5.89826 m；R(45°) = 0.027914 ⇒ (1−R) = 0.972086
    //   直射 = 0.972102·exp(−σt·5.89826)  ⇒ (0.04022, 0.55837, 0.70695)
    //   环境 = 0.94·exp(−K_d·5)          ⇒ (0.06775, 0.63010, 0.76961)
    ExpectVec3(sun, 0.04022f, 0.55837f, 0.70695f, 1.0e-4f);
    ExpectVec3(amb, 0.06775f, 0.63010f, 0.76961f, 1.0e-4f);
}

TEST(WaterUnderwaterShading, TogglesAreIndependent) {
    const WaterMediumSample m = MakeSample();
    Vec3f amb;
    Vec3f sun;

    const UnderwaterAttenuation amb_only = BuildUnderwaterAttenuation(m, true, false);
    UnderwaterGains(amb_only, {0.0f, -5.0f, 0.0f}, &amb, &sun);
    ExpectVec3(sun, 1.0f, 1.0f, 1.0f, 0.0f);
    EXPECT_LT(amb.x(), 1.0f);

    const UnderwaterAttenuation sun_only = BuildUnderwaterAttenuation(m, false, true);
    UnderwaterGains(sun_only, {0.0f, -5.0f, 0.0f}, &amb, &sun);
    ExpectVec3(amb, 1.0f, 1.0f, 1.0f, 0.0f);
    EXPECT_LT(sun.x(), 1.0f);
}

TEST(WaterUnderwaterShading, DeeperIsDarker) {
    const WaterMediumSample m = MakeSample();
    const UnderwaterAttenuation a = BuildUnderwaterAttenuation(m, true, true);
    Vec3f amb1;
    Vec3f sun1;
    Vec3f amb2;
    Vec3f sun2;
    UnderwaterGains(a, {0.0f, -2.0f, 0.0f}, &amb1, &sun1);
    UnderwaterGains(a, {0.0f, -8.0f, 0.0f}, &amb2, &sun2);
    EXPECT_LT(amb2.x(), amb1.x());
    EXPECT_LT(sun2.x(), sun1.x());
    // 红通道衰减最快（水色）：8 m 时红/蓝比应远小于 1。
    EXPECT_LT(sun2.x() / sun2.z(), 0.1f);
}

TEST(WaterUnderwaterShading, LowerSunIsDarker) {
    // 太阳越低 → 界面透射越低 + 水内路径越长 ⇒ 直射更暗（「斜着入射衰减更多」）。
    const WaterMediumSample m = MakeSample();
    const UnderwaterAttenuation a45 =
        BuildUnderwaterAttenuation(m, false, true);
    WaterMediumSample low = MakeSample();
    const float c = std::sin(20.0f * 3.14159265f / 180.0f);   // 仰角 20°（y = sin）
    low.sun_toward = {0.0f, c, -std::sqrt(1.0f - c * c)};
    const UnderwaterAttenuation a20 = BuildUnderwaterAttenuation(low, false, true);

    Vec3f amb;
    Vec3f sun45;
    Vec3f sun20;
    UnderwaterGains(a45, {0.0f, -3.0f, 0.0f}, &amb, &sun45);
    UnderwaterGains(a20, {0.0f, -3.0f, 0.0f}, &amb, &sun20);
    EXPECT_LT(sun20.x(), sun45.x());
}

TEST(WaterUnderwaterShading, SunBelowHorizonHasNoDirectLight) {
    WaterMediumSample m = MakeSample();
    m.sun_toward = {0.0f, -0.2f, -0.98f};   // 太阳在地平线以下
    const UnderwaterAttenuation a = BuildUnderwaterAttenuation(m, false, true);
    Vec3f amb;
    Vec3f sun;
    UnderwaterGains(a, {0.0f, -5.0f, 0.0f}, &amb, &sun);
    ExpectVec3(sun, 0.0f, 0.0f, 0.0f, 0.0f);
}

}  // namespace
}  // namespace water
}  // namespace jpov
