// JPOV 远景仰角雾雾色推导单测（纯 CPU）—— SkyCommand::ElevationFogColor()
//
// 验证「仰角雾雾色 = 地平线附近天光色」这条推理链：白天/夜晚/浊度/季节 各自的影响，
// 以及它与 AmbientTricolor() 地平端的一致性。

#include <gtest/gtest.h>

#include "tools/jpov/interface/render_command.h"

namespace jpov {
namespace {

// 造一个天光：太阳仰角 elev_deg、浊度 turb、日光季节色温 tint、方位固定。
SkyCommand MakeSky(float elev_deg, float turb, Color season = {1, 1, 1, 1}) {
    SkyCommand s;
    const float e = elev_deg * 3.14159265358979323846f / 180.0f;
    s.sun_dir = Vec3f(0.0f, std::sin(e), std::cos(e));  // 方位固定（+z 侧）
    s.turbidity = turb;
    s.daylight_season = season;
    return s;
}

TEST(ElevationFogColorTest, MatchesHorizonEndOfTricolor) {
    // 雾色就是三色环境光的「地平(天际线)」端 —— 保证推理链同源。
    SkyCommand s = MakeSky(45.0f, 2.0f);
    const Color fc = s.ElevationFogColor();
    const std::array<Color, 3> trio = s.AmbientTricolor();
    EXPECT_FLOAT_EQ(fc.r, trio[1].r);
    EXPECT_FLOAT_EQ(fc.g, trio[1].g);
    EXPECT_FLOAT_EQ(fc.b, trio[1].b);
}

TEST(ElevationFogColorTest, DaylightIsPositiveAndFinite) {
    SkyCommand s = MakeSky(60.0f, 2.0f);
    const Color fc = s.ElevationFogColor();
    EXPECT_GT(fc.r, 0.0f);
    EXPECT_GT(fc.g, 0.0f);
    EXPECT_GT(fc.b, 0.0f);
    EXPECT_TRUE(std::isfinite(fc.r) && std::isfinite(fc.g) && std::isfinite(fc.b));
}

TEST(ElevationFogColorTest, HazeDesaturatesTowardWhite) {
    // 高浊度 → 雾度插值把色往灰白走 ⇒ 饱和度下降。
    const Color clear_c = MakeSky(45.0f, 2.0f).ElevationFogColor();
    const Color hazy_c = MakeSky(45.0f, 8.0f).ElevationFogColor();
    auto sat = [](const Color& c) {
        const float mx = std::max(c.r, std::max(c.g, c.b));
        const float mn = std::min(c.r, std::min(c.g, c.b));
        return mx > 0 ? (mx - mn) / mx : 0.0f;
    };
    EXPECT_LT(sat(hazy_c), sat(clear_c));
}

TEST(ElevationFogColorTest, SeasonTintsHorizonRedder) {
    // 暖(红)季节 → 地平线雾色更偏红（R/B 比升高）。
    const Color neutral = MakeSky(45.0f, 2.0f).ElevationFogColor();
    const Color warm = MakeSky(45.0f, 2.0f, Color{1.0f, 0.9f, 0.75f, 1.0f})
                           .ElevationFogColor();
    const float rn = neutral.r / neutral.b;
    const float rw = warm.r / warm.b;
    EXPECT_GT(rw, rn);
}

TEST(ElevationFogColorTest, NightIsDarkButFinite) {
    SkyCommand s = MakeSky(-30.0f, 2.0f);   // 太阳落到地平线下（夜）
    const Color fc = s.ElevationFogColor();
    EXPECT_TRUE(std::isfinite(fc.r) && std::isfinite(fc.g) && std::isfinite(fc.b));
    EXPECT_GE(fc.r, 0.0f);
    EXPECT_GE(fc.g, 0.0f);
    EXPECT_GE(fc.b, 0.0f);
    // 夜间雾色应明显暗于正午（避免夜里远景被"照亮"）。
    const Color noon = MakeSky(80.0f, 2.0f).ElevationFogColor();
    const float night_lum = (fc.r + fc.g + fc.b) / 3.0f;
    const float noon_lum = (noon.r + noon.g + noon.b) / 3.0f;
    EXPECT_LT(night_lum, noon_lum);
}

}  // namespace
}  // namespace jpov
