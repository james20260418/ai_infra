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
#include <string>

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

// ⑤b 微风（0.2 m/s）必须是「几乎不动」，不是「6 Hz 抖动」（2026-11-24 Danis 报障）
//
// 现象：风开 0.2 m/s，水面还在快速运动。原因不是渲染，是 BuildWater 把 SMB 外插到
// 0.2 m/s：Hs=1.2 mm（看不见）但 λp=3.7 cm ⇒ ω≈41 rad/s（6.5 Hz）、kA 仍 ~0.07
// （SMB 自相似）；细节短波又按 √(mss(U)/mss(8)) 衰减，0.2 m/s 时还有 1.9% 坡度。
// 修法：Hs 乘 WindEnergyGate（∝u²，u≥2 恒 1），细节陡度改成 ∝ Hs。
TEST(OceanState, LightAirIsCalm) {
    WaterConfig c = MakeBase(0.2f, 0.0f, 250.0f);
    c.swell.height_m = 0.0f;
    WaterWaves w;
    WaterReadout r;
    BuildWater(c, WaterQuality{}, &w, &r);

    // 总能量必须先塌下去（SMB 在 0.2 m/s 无效）：Hs < 0.1 mm，坡度 < 0.1°。
    EXPECT_LT(r.hs_total_m, 1.0e-4f) << "0.2 m/s 的 Hs 还在毫米级 ⇒ 能量门没生效";
    EXPECT_LT(r.rms_slope, 2.0e-3f) << "0.2 m/s 的 rms 坡度还在 0.1° 以上";

    // 主波振幅与法线斜率都要低到看不出来（kA < 1e-3 rad）。
    for (int i = 0; i < w.num_waves; ++i) {
        const float k = 6.283185307179586f / w.waves[i].wavelength_m;
        EXPECT_LT(k * w.waves[i].amplitude_m, 1.0e-3f)
            << "主波 #" << i << " 的坡度还看得见（λ=" << w.waves[i].wavelength_m << "）";
    }
    // 细节短波：陡度必须跟着能量归零（不能停在 1.9% / 7% 的中间值）。
    for (int i = 0; i < w.num_detail_waves; ++i) {
        const float k = 6.283185307179586f / w.detail_waves[i].wavelength_m;
        EXPECT_LT(k * w.detail_waves[i].amplitude_m, 1.0e-4f)
            << "细节波 #" << i << " 的坡度没有随能量归零";
    }
}

// ⑤c 默认海况（8 m/s）必须**逐位不变**：能量门在 u ≥ 2 恒 1，细节陡度恰为 0.07。
TEST(OceanState, DefaultWindUnchangedByCalmFix) {
    WaterConfig c = MakeBase(8.0f, 34.0f, 250.0f);
    c.swell.height_m = 0.0f;
    WaterWaves w;
    WaterReadout r;
    BuildWater(c, WaterQuality{}, &w, &r);
    for (int i = 0; i < w.num_detail_waves; ++i) {
        const float k = 6.283185307179586f / w.detail_waves[i].wavelength_m;
        EXPECT_NEAR(k * w.detail_waves[i].amplitude_m, 0.07f, 1.0e-5f)
            << "8 m/s 的细节陡度必须仍是 0.07（旧行为）";
    }
    // 8 m/s + 250 km 风区的 Hs 仍是 SMB 值（与 FullyDevelopedMatchesSmb 同口径）。
    EXPECT_NEAR(r.hs_wind_m, 0.283f * 64.0f / 9.81f *
                                 std::tanh(0.0125f * std::pow(9.81f * 250.0f * 1000.0f /
                                                                  (8.0f * 8.0f), 0.42f)),
                1.0e-4f);
}

// ⑦ 配置矩阵不变量（2026-11-24：只用 8 m/s 一个点验过的教训）
//
// 风/风区/风时/深度/涌/seed 组合出上百种工况，逐个手写用例不现实；这里对**每个**组合
// 断言同一组不变量。任何一条破了都说明"某个配置下"的推导有问题（0.2 m/s 的 bug
// 就是这一类：单点测不出来）。
TEST(OceanState, ConfigMatrixInvariants) {
    const float kWinds[] = {0.0f, 0.2f, 1.0f, 2.0f, 4.0f, 8.0f, 14.0f, 20.0f};
    const float kFetchesKm[] = {0.05f, 0.3f, 1.0f, 250.0f};   // 池塘 → 充分成长
    const float kDepths[] = {0.3f, 3.5f, 300.0f};
    const bool kSwell[] = {false, true};
    const uint32_t kSeeds[] = {0u, 7u, 12345u};

    for (float wind : kWinds) {
        for (float fetch_km : kFetchesKm) {
            for (float depth : kDepths) {
                for (bool swell : kSwell) {
                    for (uint32_t seed : kSeeds) {
                        WaterConfig c = MakeBase(wind, 34.0f, fetch_km);
                        c.depth_m = depth;
                        c.seed = seed;
                        if (swell) {
                            c.SetSwell(1.3f, 9.5f, 104.0f * kPi / 180.0f);
                        } else {
                            c.swell.height_m = 0.0f;
                        }
                        WaterWaves w;
                        WaterReadout r;
                        BuildWater(c, WaterQuality{}, &w, &r);
                        const std::string tag =
                            "wind=" + std::to_string(wind) + " fetch=" + std::to_string(fetch_km) +
                            " depth=" + std::to_string(depth) + " swell=" + (swell ? "1" : "0");

                        // ① 数量与基本域
                        ASSERT_GE(w.num_waves, 0) << tag;
                        ASSERT_LE(w.num_waves, WaterWaves::kMaxWaves) << tag;
                        ASSERT_LE(w.num_detail_waves, WaterWaves::kMaxDetailWaves) << tag;
                        ASSERT_TRUE(std::isfinite(r.hs_total_m)) << tag;
                        ASSERT_GE(r.hs_total_m, 0.0f) << tag;
                        ASSERT_TRUE(std::isfinite(r.rms_slope)) << tag;

                        // ② 主波：λ>0、A≥0 且有限
                        for (int i = 0; i < w.num_waves; ++i) {
                            EXPECT_GT(w.waves[i].wavelength_m, 0.0f) << tag;
                            EXPECT_GE(w.waves[i].amplitude_m, 0.0f) << tag;
                            EXPECT_TRUE(std::isfinite(w.waves[i].wavelength_m)) << tag;
                            EXPECT_TRUE(std::isfinite(w.waves[i].amplitude_m)) << tag;
                        }
                        // ③ 能量账：Σ½A² == Hs²/16（风海部分）
                        float sum_a2 = 0.0f;
                        for (int i = 0; i < w.num_waves; ++i) {
                            // 涌也在这张表里：把涌的 Hs 扣掉后再核（两者按能量叠加）。
                            if (swell && w.waves[i].wavelength_m > 20.0f) {
                                continue;   // 涌的 λ≈120~160 m，风海的 λp ≤ ~66 m
                            }
                            sum_a2 += 0.5f * w.waves[i].amplitude_m * w.waves[i].amplitude_m;
                        }
                        if (!swell) {
                            EXPECT_NEAR(4.0f * std::sqrt(sum_a2), r.hs_wind_m,
                                        1.0e-3f * std::max(1.0f, r.hs_wind_m))
                                << tag << "：方差与 Hs 不闭合";
                        }
                        // ④ 陡度必须留在物理区间（破碎极限 kA≈0.44 是单波极限，
                        //    多波叠加的 rms 坡度不该超过 ~0.6）
                        EXPECT_LT(r.rms_slope, 0.6f) << tag << "：坡度超出破碎上限";

                        // ⑤ 细节波：坡度必须 ≤ 主波的 0.07 量级（能量大时封顶）
                        for (int i = 0; i < w.num_detail_waves; ++i) {
                            const float k = 2.0f * kPi / w.detail_waves[i].wavelength_m;
                            EXPECT_LT(k * w.detail_waves[i].amplitude_m, 0.08f) << tag;
                        }
                    }
                }
            }
        }
    }
}

// ⑧ 单调性：固定风区时 Hs 随风速单调不减；固定风速时 Hs 随风区单调不减（有饱和）。
TEST(OceanState, HsMonotonicInWindAndFetch) {
    float prev_hs = -1.0f;
    for (float wind : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 18.0f}) {
        WaterConfig c = MakeBase(wind, 34.0f, 250.0f);
        c.swell.height_m = 0.0f;
        WaterWaves w;
        WaterReadout r;
        BuildWater(c, WaterQuality{}, &w, &r);
        EXPECT_GE(r.hs_wind_m, prev_hs - 1.0e-6f) << "Hs 不随风速单调（wind=" << wind << "）";
        prev_hs = r.hs_wind_m;
    }
    prev_hs = -1.0f;
    for (float fetch_km : {0.05f, 0.1f, 0.3f, 1.0f, 10.0f, 250.0f}) {
        WaterConfig c = MakeBase(8.0f, 34.0f, fetch_km);
        c.swell.height_m = 0.0f;
        WaterWaves w;
        WaterReadout r;
        BuildWater(c, WaterQuality{}, &w, &r);
        EXPECT_GE(r.hs_wind_m, prev_hs - 1.0e-6f) << "Hs 不随风区单调（fetch=" << fetch_km << "）";
        prev_hs = r.hs_wind_m;
    }
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
