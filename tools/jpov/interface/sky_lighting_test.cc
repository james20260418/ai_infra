// SkyCommand 平行光推导单测（纯 CPU，不渲染、不碰 GL）。
//
// 覆盖：
//   - 月亮直射光 MoonDirectionalIntensity：天顶基准、白天门控、地平线下归零、
//     随月亮仰角单调不减、随浊度衰减。
//   - 月亮直射光 MoonDirectionalColor：常量色温 4100K（与月盘同色）、moon_season 染色
//     （不归一化，血月又红又暗）、不吃 daylight_season。
//   - daylight_season：经 DaylightSeasonTintScale 归一化、只染太阳能通道
//     （白天 ambient 受染 / 夜间 ambient 不受染）。
//   - SunDirectionalIntensity 抽取 DniFactorFromElevDeg 后的零回归锚点。
#include "tools/jpov/interface/render_command.h"

#include <cmath>

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace jpov {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.0f;

// 构造“夜”天光：太阳在地平线下（daylight=0），月亮在给定仰角/方位。
// 太阳方向固定 (0,-1,0) ⇒ sun_y = -1 ⇒ daylight = 0。
SkyCommand NightSky(float moon_elev_deg, float moon_azim_deg = 0.0f,
                    float turbidity = 2.0f) {
    SkyCommand sky;
    sky.turbidity = turbidity;
    sky.sun_dir = Vec3f(0.0f, -1.0f, 0.0f);
    const float e = moon_elev_deg * kDeg;
    const float a = moon_azim_deg * kDeg;
    sky.moon_dir = Vec3f(std::cos(e) * std::sin(a), std::sin(e),
                         std::cos(e) * std::cos(a));
    return sky;
}

}  // namespace

// 夜 + 满月天顶：DniFactor(90°)=1、TurbSunLoss(2.0)=1、daylight=0 ⇒ 强度 = 基准值
//（默认 0.22 = 日光基准 2.2 的 1/10）。
TEST(SkyMoonLightTest, ZenithFullMoonAtClearNightEqualsBase) {
    const SkyCommand sky = NightSky(90.0f);
    EXPECT_NEAR(sky.MoonDirectionalIntensity(0.22f), 0.22f, 1e-5f);
    EXPECT_NEAR(sky.MoonDirectionalIntensity(), 0.22f, 1e-5f);  // 默认基准
}

// 白天（太阳正午）即便满月天顶，月光压到 0（(1−daylight) 门控）。
TEST(SkyMoonLightTest, SuppressedInDaylight) {
    SkyCommand sky = NightSky(90.0f);
    sky.sun_dir = Vec3f(0.0f, 1.0f, 0.0f);  // 正午 ⇒ daylight=1
    EXPECT_NEAR(sky.MoonDirectionalIntensity(), 0.0f, 1e-6f);
}

// 月亮在地平线上（仰角 ≤ 1°）时 DniFactor 夹断到 0 ⇒ 无月光。
TEST(SkyMoonLightTest, ZeroWhenMoonBelowHorizon) {
    EXPECT_NEAR(NightSky(0.0f).MoonDirectionalIntensity(), 0.0f, 1e-6f);
    EXPECT_NEAR(NightSky(-30.0f).MoonDirectionalIntensity(), 0.0f, 1e-6f);
}

// 夜间：随月亮仰角单调不减（仰角越低穿大气越厚 ⇒ 越暗）。
TEST(SkyMoonLightTest, MonotonicWithMoonElevation) {
    float prev = -1.0f;
    for (float el = -10.0f; el <= 90.0f; el += 5.0f) {
        const float v = NightSky(el).MoonDirectionalIntensity();
        EXPECT_GE(v, prev - 1e-6f) << "moon_elev=" << el;
        prev = v;
    }
}

// 浊度越大月光越弱（同仰角）；turb=2 → 乘子 1.0，turb=8 → 乘子 0.2。
TEST(SkyMoonLightTest, LowersWithTurbidity) {
    const float clear = NightSky(45.0f, 0.0f, 2.0f).MoonDirectionalIntensity(1.0f);
    const float hazy = NightSky(45.0f, 0.0f, 8.0f).MoonDirectionalIntensity(1.0f);
    EXPECT_GT(clear, hazy);
    EXPECT_NEAR(clear, 1.0f, 1e-5f);  // DniFactor(45°)=1，TurbSunLoss(2)=1
    EXPECT_NEAR(hazy, 0.2f, 1e-5f);   // TurbSunLoss(8)=0.2
}

// 颜色 = 常量 4100K（同一套色温曲线；shader 月盘侧写的是同一个常量）⇒ 偏暖 R>B。
TEST(SkyMoonLightTest, ColorIsMoonDiskColorTemp) {
    const SkyCommand sky = NightSky(45.0f);
    const Color c = sky.MoonDirectionalColor();
    const Color ref = SkyCommand::ColorTempToLinear(4100.0f);
    EXPECT_NEAR(c.r, ref.r, 1e-6f);
    EXPECT_NEAR(c.g, ref.g, 1e-6f);
    EXPECT_NEAR(c.b, ref.b, 1e-6f);
    EXPECT_GT(c.r, c.b);
}

// moon_season 染月盘/月光，且**不归一化**（乘子直接作用，故血月也变暗）。
TEST(SkyMoonLightTest, MoonSeasonTintsMoonLightWithoutNormalizing) {
    SkyCommand sky = NightSky(45.0f);
    const Color neutral = sky.MoonDirectionalColor();
    sky.moon_season = Color{1.0f, 0.35f, 0.05f, 1.0f};  // 血月
    const Color blood = sky.MoonDirectionalColor();
    // R 通道**不动** —— 直接证明 moon_season 不做亮度归一化
    //（若像 daylight_season 那样归一化，R 会被放大，此断言失败）。
    EXPECT_NEAR(blood.r, neutral.r, 1e-6f);
    EXPECT_LT(blood.g, neutral.g);  // 压绿
    EXPECT_LT(blood.b, neutral.b);  // 压蓝
}

// daylight_season **不吃月亮**：月亮由 moon_season 单独管（两把旋钮互不干扰）。
TEST(SkyMoonLightTest, DaylightSeasonDoesNotAffectMoon) {
    SkyCommand sky = NightSky(45.0f);
    const Color before = sky.MoonDirectionalColor();
    sky.daylight_season = Color{0.75f, 0.85f, 1.0f, 1.0f};
    const Color after = sky.MoonDirectionalColor();
    EXPECT_NEAR(after.r, before.r, 1e-6f);
    EXPECT_NEAR(after.g, before.g, 1e-6f);
    EXPECT_NEAR(after.b, before.b, 1e-6f);
}

// daylight_season 对太阳直射光生效（冷偏 ⇒ 压红提蓝），且**只偏色不改亮度**：
// 中性灰乘子（各通道相等）的有效乘子仍为 (1,1,1) ⇒ 颜色逐分量不变。
TEST(SkyMoonLightTest, DaylightSeasonTintsSunLightPreservingBrightness) {
    SkyCommand sky = NightSky(45.0f);
    sky.sun_dir = Vec3f(0.0f, 1.0f, 0.0f);
    const Color neutral = sky.SunDirectionalColor();
    // 冷偏 ⇒ 压红提蓝。
    sky.daylight_season = Color{0.75f, 0.85f, 1.0f, 1.0f};
    const Color tinted = sky.SunDirectionalColor();
    EXPECT_LT(tinted.r, neutral.r);
    EXPECT_GT(tinted.b, neutral.b);
    // 灰乘子 ⇒ 无偏色（归一化把有效乘子拉回 (1,1,1)）。
    sky.daylight_season = Color{0.5f, 0.5f, 0.5f, 1.0f};
    const Color grey = sky.SunDirectionalColor();
    EXPECT_NEAR(grey.r, neutral.r, 1e-6f);
    EXPECT_NEAR(grey.g, neutral.g, 1e-6f);
    EXPECT_NEAR(grey.b, neutral.b, 1e-6f);
}

// daylight_season 只染太阳能通道：白天 ambient 色受染，夜间 ambient 色（夜色端）完全不变。
TEST(SkyMoonLightTest, DaylightSeasonTintsOnlySolarChannels) {
    const Color cold{0.75f, 0.85f, 1.0f, 1.0f};
    // 白天（正午 → daylight=1）：ambient 色受染。
    SkyCommand day = NightSky(45.0f);
    day.sun_dir = Vec3f(0.0f, 1.0f, 0.0f);
    const Color day0 = day.AmbientColor();
    day.daylight_season = cold;
    const Color day1 = day.AmbientColor();
    EXPECT_LT(day1.r, day0.r);
    EXPECT_GT(day1.b, day0.b);
    // 夜间（sun 在地平线下 → daylight=0）：ambient 色逐分量不变。
    SkyCommand night = NightSky(45.0f);
    const Color n0 = night.AmbientColor();
    night.daylight_season = cold;
    const Color n1 = night.AmbientColor();
    EXPECT_NEAR(n1.r, n0.r, 1e-6f);
    EXPECT_NEAR(n1.g, n0.g, 1e-6f);
    EXPECT_NEAR(n1.b, n0.b, 1e-6f);
}

// SunDirectionalIntensity 抽取 DniFactorFromElevDeg 后的零回归：钉住两个锚点。
TEST(SkyMoonLightTest, SunIntensityUnchangedAfterCurveExtraction) {
    SkyCommand sky = NightSky(0.0f);
    // 天顶：DniFactor(90°)=1、turb=2 ⇒ 等于基准。
    sky.sun_dir = Vec3f(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(sky.SunDirectionalIntensity(2.2f), 2.2f, 1e-5f);
    // 30°：DniFactor=0.91（PWL 锚点）。
    sky.sun_dir = Vec3f(std::cos(30.0f * kDeg), std::sin(30.0f * kDeg), 0.0f);
    EXPECT_NEAR(sky.SunDirectionalIntensity(2.2f), 2.2f * 0.91f, 1e-5f);
}

}  // namespace jpov
