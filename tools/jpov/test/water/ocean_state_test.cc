// WaterConfig → 谱 的展开测试（纯 CPU，不碰 GL）。
//
// 锁的是"用户只给风/涌，谱自动推出来"这条链路的**物理正确性**与**可复现性**：
//   ① 充分成长 / 有限风区的 Hs、Tp 落在 SMB 经验值上；
//   ② 合成后的方差精确等于 Hs²/16（否则"设 1.5 m 却得到 2 m"）；
//   ③ 同一 seed 逐位相同、不同 seed 只换相位；
//   ④ 无风 ⇒ 镜面（没有主波、细节振幅归零）；
//   ⑤ 涌与风海按能量叠加；
//   ⑥ 与渲染侧同源的两个常数（色散 / Cox–Munk）不许分叉。

#include <cmath>
#include <cstring>

#include <gtest/gtest.h>

#include "tools/jpov/interface/water_config.h"
#include "tools/jpov/src/water/water_wave_math.h"

namespace {

using jpov::WaterWave;
using jpov::WaterWaves;
using jpov::water::BuildWater;
using jpov::water::WaterConfig;
using jpov::water::WaterQuality;
using jpov::water::WaterReadout;

constexpr float kPi = 3.14159265358979323846f;

WaterConfig MakeBase(float wind_ms, float dir_deg, float fetch_km) {
    WaterConfig c;
    c.SetWind(wind_ms, dir_deg * kPi / 180.0f);
    c.fetch_km = fetch_km;
    c.duration_h = std::numeric_limits<float>::infinity();
    c.swell.height_m = 0.0f;
    c.depth_m = 300.0f;
    return c;
}

float Variance(const WaterWaves& w) {
    float v = 0.0f;
    for (int i = 0; i < w.num_waves; ++i) {
        v += 0.5f * w.waves[i].amplitude_m * w.waves[i].amplitude_m;
    }
    return v;
}

// ① 充分成长：U=8 ⇒ Hs=1.85 m / Tp=6.15 s（SMB，U 取 10 m）
TEST(OceanState, FullyDevelopedMatchesSmb) {
    WaterConfig c = MakeBase(8.0f, 34.0f, std::numeric_limits<float>::infinity());
    WaterWaves w;
    WaterReadout r;
    BuildWater(c, WaterQuality{}, &w, &r);
    EXPECT_NEAR(r.hs_wind_m, 0.283f * 64.0f / 9.81f, 0.02f);
    EXPECT_NEAR(r.period_s, 7.54f * 8.0f / 9.81f, 0.05f);
    EXPECT_NEAR(r.lambda_peak_m, 1.56f * r.period_s * r.period_s, 1.0f);
}

// ② 有限风区把 Hs 压小（20 km @ 8 m/s ≈ 0.64 m）
TEST(OceanState, FetchLimitedShrinksHs) {
    WaterWaves w1, w2;
    WaterReadout r1, r2;
    BuildWater(MakeBase(8.0f, 34.0f, 20.0f), WaterQuality{}, &w1, &r1);
    BuildWater(MakeBase(8.0f, 34.0f, std::numeric_limits<float>::infinity()),
               WaterQuality{}, &w2, &r2);
    EXPECT_LT(r1.hs_wind_m, r2.hs_wind_m);
    EXPECT_NEAR(r1.hs_wind_m, 0.64f, 0.08f);
    EXPECT_LT(r1.lambda_peak_m, r2.lambda_peak_m);
}

// ③ 方差精确 = Hs²/16（用户设的波高必须就是看到的波高）
TEST(OceanState, VarianceMatchesHs) {
    for (float u : {4.0f, 8.0f, 12.0f}) {
        WaterConfig c = MakeBase(u, 34.0f, 250.0f);
        WaterWaves w;
        WaterReadout r;
        BuildWater(c, WaterQuality{}, &w, &r);
        const float expect = r.hs_wind_m * r.hs_wind_m / 16.0f;
        EXPECT_NEAR(Variance(w), expect, 1.0e-4f * std::max(expect, 1.0f))
            << "u=" << u;
    }
}

// ④ 确定性：同 seed 逐位相同；换 seed 只改相位（Hs 不变）
TEST(OceanState, DeterministicAndSeedOnlyChangesPhase) {
    WaterConfig c = MakeBase(8.0f, 34.0f, 250.0f);
    WaterWaves a, b, d;
    WaterReadout ra, rb, rd;
    BuildWater(c, WaterQuality{}, &a, &ra);
    BuildWater(c, WaterQuality{}, &b, &rb);
    ASSERT_EQ(a.num_waves, b.num_waves);
    for (int i = 0; i < a.num_waves; ++i) {
        EXPECT_EQ(a.waves[i].wavelength_m, b.waves[i].wavelength_m);
        EXPECT_EQ(a.waves[i].amplitude_m, b.waves[i].amplitude_m);
        EXPECT_EQ(a.waves[i].phase_rad, b.waves[i].phase_rad);
    }
    c.seed = 7u;
    BuildWater(c, WaterQuality{}, &d, &rd);
    EXPECT_NEAR(ra.hs_wind_m, rd.hs_wind_m, 1.0e-6f) << "换 seed 不该改波高";
    bool any_phase_diff = false;
    for (int i = 0; i < d.num_waves; ++i) {
        if (d.waves[i].phase_rad != a.waves[i].phase_rad) {
            any_phase_diff = true;
        }
    }
    EXPECT_TRUE(any_phase_diff) << "换 seed 之后相位完全没变：seed 没接上";
}

// ⑤ 无风 ⇒ 镜面：没有主波，细节振幅归零
TEST(OceanState, NoWindIsMirror) {
    WaterConfig c = MakeBase(0.0f, 0.0f, 250.0f);
    c.swell.height_m = 0.0f;
    WaterWaves w;
    WaterReadout r;
    BuildWater(c, WaterQuality{}, &w, &r);
    EXPECT_EQ(w.num_waves, 0);
    EXPECT_NEAR(r.rms_slope, 0.0f, 1.0e-6f);
    for (int i = 0; i < w.num_detail_waves; ++i) {
        EXPECT_NEAR(w.detail_waves[i].amplitude_m, 0.0f, 1.0e-9f);
    }
    EXPECT_EQ(w.short_wave.wind_speed_ms, 0.0f);
}

// ⑥ 涌与风海按能量叠加：Hs_total = √(Hs_wind² + Hs_swell²)
TEST(OceanState, SwellAddsInQuadrature) {
    WaterConfig c = MakeBase(8.0f, 34.0f, 250.0f);
    c.SetSwell(1.3f, 9.5f, 104.0f * kPi / 180.0f);
    WaterWaves w;
    WaterReadout r;
    BuildWater(c, WaterQuality{}, &w, &r);
    EXPECT_NEAR(r.hs_swell_m, 1.3f, 1.0e-6f);
    EXPECT_NEAR(r.hs_total_m, std::sqrt(r.hs_wind_m * r.hs_wind_m + 1.69f), 1.0e-3f);
    // 涌的峰周期比风海长 ⇒ 读数是混合后的（这里涌能量更小，取风海）
    EXPECT_GT(w.num_waves, 0);
    // 主波里必须同时存在"很长的"（涌 9.5 s ⇒ λ ≈ 141 m）
    float max_lam = 0.0f;
    for (int i = 0; i < w.num_waves; ++i) {
        max_lam = std::max(max_lam, w.waves[i].wavelength_m);
    }
    EXPECT_GT(max_lam, 100.0f) << "给了涌却没有长波：涌没进谱";
}

// ⑦ 质量档：细节条数受 WaterQuality 控制，且不改变主波
TEST(OceanState, QualityControlsDetailCount) {
    WaterConfig c = MakeBase(8.0f, 34.0f, 250.0f);
    WaterWaves w32, w64;
    BuildWater(c, WaterQuality{32, 8}, &w32, nullptr);
    BuildWater(c, WaterQuality{64, 8}, &w64, nullptr);
    EXPECT_EQ(w32.num_detail_waves, 32);
    EXPECT_EQ(w64.num_detail_waves, 64);
    EXPECT_EQ(w32.num_waves, w64.num_waves);
    for (int i = 0; i < w32.num_waves; ++i) {
        EXPECT_EQ(w32.waves[i].wavelength_m, w64.waves[i].wavelength_m);
    }
}

// ⑧ 与渲染侧同源的两个常数不许分叉（色散 / Cox–Munk）
TEST(OceanState, SharedConstantsMatch) {
    for (float k : {0.05f, 0.2f, 1.0f, 5.0f}) {
        for (float d : {2.0f, 10.0f, 50.0f, 300.0f}) {
            EXPECT_NEAR(jpov::water::OceanOmega(k, d),
                        jpov::water::DispersionOmega(k, d), 1.0e-5f)
                << "k=" << k << " d=" << d;
        }
    }
    for (float u : {0.0f, 4.0f, 8.0f, 15.0f, 25.0f}) {
        EXPECT_NEAR(jpov::water::OceanCoxMunkMss(u),
                    jpov::water::CoxMunkMeanSquareSlope(u), 1.0e-6f) << "u=" << u;
    }
}

// ⑨ 浅水色散：kd 小 ⇒ ω 比深水小（波变慢）
TEST(OceanState, ShallowWaterSlowsWaves) {
    const float k = 2.0f * kPi / 100.0f;   // λ=100 m
    const float deep = jpov::water::OceanOmega(k, 300.0f);
    const float shallow = jpov::water::OceanOmega(k, 5.0f);
    EXPECT_LT(shallow, deep * 0.75f);
    // 浅水极限 ω → k√(gd)
    const float expect_shallow = k * std::sqrt(9.81f * 5.0f);
    EXPECT_NEAR(shallow, expect_shallow, 0.02f * expect_shallow);
}

}  // namespace
