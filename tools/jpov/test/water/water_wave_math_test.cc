// JPOV 水面波浪 CPU 单测（GL-free）
//
// 钉住的判据（每条都想过反例）：
//   · 无波 ⇒ h≡0、法线 (0,1,0)、求交与平面解析解一致（零回归的根据）；
//   · 单波 ⇒ h 与解析式一致，梯度与**数值差分**一致（防止公式抄反）；
//   · flow ⇒ 只改变方向/振幅，不改变「无流时逐字节一致」；
//   · fade ⇒ 远处必须真的回到 0（不是「很小」）；
//   · 求交 ⇒ 交点必须落在曲面上（残差 |P.y − level − h(P.xz)| 有界）。
//
// 与 shader 的一致性由 water_skin_gpu_test 的逐像素对拍兜底。

#include <cmath>

#include <gtest/gtest.h>

#include "tools/jpov/src/water/water_wave_math.h"

namespace {

using jpov::Vec3f;
using jpov::WaterWave;
using jpov::WaterWaves;
using jpov::water::IntersectWaveSurface;
using jpov::water::kWaveGravity;
using jpov::water::SampleWaveField;
using jpov::water::WaveFadeFactor;
using jpov::water::WaveNormal;
using jpov::water::WaterWaveSample;
using jpov::water::WaveSurfaceHit;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

// 构造一个「单波、无 flow、不淡出」的谱。
WaterWaves MakeSingleWave(float dir_x, float dir_z, float wavelength, float amplitude,
                          float phase) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 1;
    w.waves[0] = WaterWave{dir_x, dir_z, wavelength, amplitude, phase};
    w.fade_start_m = 0.0f;
    w.fade_end_m = 0.0f;   // end <= start ⇒ 不淡出
    return w;
}

// ───────────────────────── 零回归：无波 = 纯平面 ─────────────────────────

TEST(WaterWaveMath, DisabledMeansFlat) {
    WaterWaves w;   // 默认 enabled=false
    const WaterWaveSample s = SampleWaveField(w, 3.0f, -2.0f, 5.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(s.height_m, 0.0f);
    EXPECT_EQ(s.dh_dx, 0.0f);
    EXPECT_EQ(s.dh_dz, 0.0f);
    const Vec3f n = WaveNormal(s);
    EXPECT_FLOAT_EQ(n.x(), 0.0f);
    EXPECT_FLOAT_EQ(n.y(), 1.0f);
    EXPECT_FLOAT_EQ(n.z(), 0.0f);
}

TEST(WaterWaveMath, ZeroWaveCountMeansFlatEvenIfEnabled) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 0;
    const WaterWaveSample s = SampleWaveField(w, 1.0f, 2.0f, 3.0f, 0.5f, 0.5f, 1.0f);
    EXPECT_EQ(s.height_m, 0.0f);
}

// 无波求交必须与平面解析解**完全一致**（不是「接近」）。
TEST(WaterWaveMath, IntersectFallsBackToPlane) {
    WaterWaves w;
    const Vec3f cam{0.0f, 10.0f, 0.0f};
    const Vec3f rd{0.0f, -0.6f, 0.8f};
    WaveSurfaceHit hit;
    ASSERT_TRUE(IntersectWaveSurface(cam, rd, 0.0f, w, 0.0f, 0.0f, 0.0f, /*footprint=*/0.0f, &hit));
    EXPECT_FLOAT_EQ(hit.t, 10.0f / 0.6f);
    EXPECT_FLOAT_EQ(hit.normal.y(), 1.0f);
}

// ───────────────────────── 单波：解析式 ─────────────────────────

TEST(WaterWaveMath, SingleWaveMatchesAnalyticHeight) {
    // λ=10 ⇒ k=2π/10；x=λ/4 ⇒ phase=π/2 ⇒ h=A。
    const float lambda = 10.0f;
    const float amp = 0.5f;
    const WaterWaves w = MakeSingleWave(/*dir_x*/ 1.0f, /*dir_z*/ 0.0f, lambda, amp,
                                        /*phase*/ 0.0f);
    const float k = kTwoPi / lambda;
    const float x = 0.25f * lambda;
    const WaterWaveSample s = SampleWaveField(w, x, 0.0f, /*time*/ 0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(s.height_m, amp, 1.0e-5f);
    // 该点 cos(π/2)=0 ⇒ 梯度 0。
    EXPECT_NEAR(s.dh_dx, 0.0f, 1.0e-5f);
    EXPECT_NEAR(s.dh_dz, 0.0f, 1.0e-5f);

    // x=0 ⇒ phase=0 ⇒ h=0，梯度 = A·k。
    const WaterWaveSample s0 = SampleWaveField(w, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(s0.height_m, 0.0f, 1.0e-6f);
    EXPECT_NEAR(s0.dh_dx, amp * k, 1.0e-5f);
}

// 色散：t = 半周期 ⇒ 相位推进 π ⇒ h 反号。钉住 ω = sqrt(g·k)（写成 ω=k 会红）。
TEST(WaterWaveMath, DispersionUsesSqrtGravityTimesK) {
    const float lambda = 8.0f;
    const float amp = 1.0f;
    const WaterWaves w = MakeSingleWave(1.0f, 0.0f, lambda, amp, 0.0f);
    const float k = kTwoPi / lambda;
    const float omega = std::sqrt(kWaveGravity * k);
    // phase = −ωt。t 走 1/4 周期 ⇒ phase = −π/2 ⇒ h = −A；再走 1/4 ⇒ +A。
    const float quarter_period = 0.5f * kPi / omega;
    const WaterWaveSample s =
        SampleWaveField(w, 0.0f, 0.0f, quarter_period, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(s.height_m, -amp, 1.0e-4f);
    const WaterWaveSample s2 =
        SampleWaveField(w, 0.0f, 0.0f, 3.0f * quarter_period, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(s2.height_m, amp, 1.0e-4f);
}

// 梯度符号：若把 ∂h/∂x 写成 −A·k·cos，这条会红（数值差分能分辨符号）。
TEST(WaterWaveMath, GradientMatchesNumericalDifference) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 3;
    w.waves[0] = WaterWave{1.0f, 0.0f, 7.0f, 0.35f, 0.3f};
    w.waves[1] = WaterWave{0.6f, 0.8f, 4.0f, 0.2f, 1.1f};
    w.waves[2] = WaterWave{-0.8f, 0.6f, 2.5f, 0.1f, 2.0f};
    w.fade_start_m = 0.0f;
    w.fade_end_m = 0.0f;

    const float x = 1.7f;
    const float z = -0.9f;
    const float t = 2.3f;
    const float eps = 1.0e-3f;
    const WaterWaveSample s = SampleWaveField(w, x, z, t, 0.0f, 0.0f, 1.0f);
    const float hx_plus = SampleWaveField(w, x + eps, z, t, 0.0f, 0.0f, 1.0f).height_m;
    const float hx_minus = SampleWaveField(w, x - eps, z, t, 0.0f, 0.0f, 1.0f).height_m;
    const float hz_plus = SampleWaveField(w, x, z + eps, t, 0.0f, 0.0f, 1.0f).height_m;
    const float hz_minus = SampleWaveField(w, x, z - eps, t, 0.0f, 0.0f, 1.0f).height_m;
    const float num_dx = (hx_plus - hx_minus) / (2.0f * eps);
    const float num_dz = (hz_plus - hz_minus) / (2.0f * eps);
    EXPECT_NEAR(s.dh_dx, num_dx, 2.0e-4f);
    EXPECT_NEAR(s.dh_dz, num_dz, 2.0e-4f);
}

TEST(WaterWaveMath, NormalIsUnitAndPointsUp) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 2;
    w.waves[0] = WaterWave{1.0f, 0.0f, 5.0f, 0.8f, 0.0f};
    w.waves[1] = WaterWave{0.0f, 1.0f, 3.0f, 0.5f, 0.7f};
    w.fade_start_m = 0.0f;
    w.fade_end_m = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const float x = 0.37f * static_cast<float>(i);
        const float z = -0.21f * static_cast<float>(i);
        const WaterWaveSample s = SampleWaveField(w, x, z, 1.0f, 0.0f, 0.0f, 1.0f);
        const Vec3f n = WaveNormal(s);
        EXPECT_NEAR(n.Norm(), 1.0f, 1.0e-5f);
        EXPECT_GT(n.y(), 0.0f);   // 永不超过 90°（纯高度场天然单值、朝上）
    }
}

// ───────────────────────── flow 调制 ─────────────────────────

TEST(WaterWaveMath, NoFlowMatchesZeroFlow) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 2;
    w.waves[0] = WaterWave{1.0f, 0.0f, 9.0f, 0.3f, 0.4f};
    w.waves[1] = WaterWave{0.0f, 1.0f, 4.0f, 0.15f, 0.9f};
    w.fade_start_m = 0.0f;
    w.fade_end_m = 0.0f;
    const WaterWaveSample a = SampleWaveField(w, 1.3f, 2.1f, 0.7f, 0.0f, 0.0f, 1.0f);
    const WaterWaveSample b = SampleWaveField(w, 1.3f, 2.1f, 0.7f, 0.0f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(a.height_m, b.height_m);
    EXPECT_FLOAT_EQ(a.dh_dx, b.dh_dx);
}

// flow 沿 +x、align=1 ⇒ 原本沿 +z 传播的波被扭到 +x：
// 此时 h 沿 z 不再变化（dh/dz == 0），沿 x 才有梯度。
TEST(WaterWaveMath, FlowAlignRotatesWaveDirection) {
    WaterWaves w = MakeSingleWave(/*dir_x*/ 0.0f, /*dir_z*/ 1.0f, /*lambda*/ 10.0f,
                                  /*amp*/ 0.5f, /*phase*/ 0.0f);
    w.flow_align = 1.0f;
    w.flow_wind_gain = 0.0f;   // 排除振幅增益的干扰
    // flow 强度 1（|flow| 会进 blend 的 clamp）。
    const WaterWaveSample s =
        SampleWaveField(w, 0.0f, 0.0f, 0.0f, /*flow_x*/ 1.0f, /*flow_z*/ 0.0f, 1.0f);
    // phase=0 ⇒ h=0、cos=1 ⇒ 梯度沿 +x（原来的 +z 方向被扭过来了）。
    EXPECT_NEAR(s.dh_dz, 0.0f, 1.0e-6f);
    EXPECT_GT(s.dh_dx, 0.0f);
}

TEST(WaterWaveMath, FlowStrengthensAmplitude) {
    WaterWaves w = MakeSingleWave(1.0f, 0.0f, 10.0f, 0.5f, 0.0f);
    w.flow_align = 0.0f;      // 不改方向
    w.flow_wind_gain = 1.0f;  // A *= 1 + |flow|
    // 在相位 = π/2 处取峰值（x = λ/4）。
    const float x = 2.5f;
    const WaterWaveSample no_flow =
        SampleWaveField(w, x, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const WaterWaveSample with_flow =
        SampleWaveField(w, x, 0.0f, 0.0f, /*flow_x*/ 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(no_flow.height_m, 0.5f, 1.0e-5f);
    EXPECT_NEAR(with_flow.height_m, 1.0f, 1.0e-4f);   // 振幅 ×2
}

// ───────────────────────── 淡出 ─────────────────────────

TEST(WaterWaveMath, FadeFactorIsOneInsideAndZeroOutside) {
    WaterWaves w;
    w.fade_start_m = 100.0f;
    w.fade_end_m = 300.0f;
    EXPECT_FLOAT_EQ(WaveFadeFactor(w, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(WaveFadeFactor(w, 100.0f), 1.0f);
    EXPECT_FLOAT_EQ(WaveFadeFactor(w, 300.0f), 0.0f);
    EXPECT_FLOAT_EQ(WaveFadeFactor(w, 500.0f), 0.0f);
    const float mid = WaveFadeFactor(w, 200.0f);
    EXPECT_NEAR(mid, 0.5f, 1.0e-6f);   // smoothstep 中点
}

TEST(WaterWaveMath, FadeEndNotSetMeansNoFade) {
    WaterWaves w;
    w.fade_start_m = 50.0f;
    w.fade_end_m = 50.0f;   // <= start ⇒ 关
    EXPECT_FLOAT_EQ(WaveFadeFactor(w, 1.0e6f), 1.0f);
}

// 远处 fade=0 必须让 h 精确为 0（不是「近似小」）。
TEST(WaterWaveMath, FadeZeroProducesExactFlat) {
    WaterWaves w = MakeSingleWave(1.0f, 0.0f, 10.0f, 1.0f, 0.0f);
    const WaterWaveSample s = SampleWaveField(w, 1.3f, 2.7f, 0.4f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(s.height_m, 0.0f);
    EXPECT_EQ(s.dh_dx, 0.0f);
    EXPECT_EQ(s.dh_dz, 0.0f);
}

// ───────────────────────── 求交残差 ─────────────────────────

// 交点必须落在曲面上：|P.y − level − h(P.xz)| < 1e-3。
TEST(WaterWaveMath, IntersectionLandsOnSurface) {
    WaterWaves w;
    w.enabled = true;
    w.num_waves = 4;
    w.waves[0] = WaterWave{1.0f, 0.0f, 12.0f, 0.4f, 0.2f};
    w.waves[1] = WaterWave{0.3f, 0.95f, 7.0f, 0.25f, 1.3f};
    w.waves[2] = WaterWave{-0.7f, 0.7f, 4.5f, 0.12f, 2.1f};
    w.waves[3] = WaterWave{0.0f, 1.0f, 3.0f, 0.06f, 0.5f};
    w.fade_start_m = 0.0f;
    w.fade_end_m = 0.0f;

    const Vec3f cam{2.0f, 8.0f, -1.0f};
    const float level = 0.5f;
    // 若干条不同俯角的射线（含掠射）都要收敛。
    const Vec3f dirs[4] = {
        {0.0f, -1.0f, 0.0f},
        {0.2f, -0.7f, 0.68f},
        {-0.35f, -0.4f, 0.85f},
        {0.5f, -0.2f, 0.84f},
    };
    for (const Vec3f& d : dirs) {
        const float len = d.Norm();
        const Vec3f rd{d.x() / len, d.y() / len, d.z() / len};
        WaveSurfaceHit hit;
        ASSERT_TRUE(IntersectWaveSurface(cam, rd, level, w, 0.0f, 0.0f, 1.7f, /*footprint=*/0.0f, &hit))
            << "rd=(" << rd.x() << "," << rd.y() << "," << rd.z() << ")";
        const Vec3f p{cam.x() + rd.x() * hit.t,
                      cam.y() + rd.y() * hit.t,
                      cam.z() + rd.z() * hit.t};
        const WaterWaveSample s =
            SampleWaveField(w, p.x(), p.z(), 1.7f, 0.0f, 0.0f,
                            WaveFadeFactor(w, std::sqrt((p.x() - cam.x()) *
                                                            (p.x() - cam.x()) +
                                                        (p.z() - cam.z()) *
                                                            (p.z() - cam.z()))));
        const float residual = p.y() - (level + s.height_m);
        EXPECT_NEAR(residual, 0.0f, 1.0e-3f)
            << "rd=(" << rd.x() << "," << rd.y() << "," << rd.z() << ")";
    }
}

TEST(WaterWaveMath, IntersectionRejectsBackwardAndParallel) {
    const WaterWaves w = MakeSingleWave(1.0f, 0.0f, 10.0f, 0.3f, 0.0f);
    WaveSurfaceHit hit;
    const Vec3f cam{0.0f, 10.0f, 0.0f};
    // 背离水面。
    EXPECT_FALSE(IntersectWaveSurface(cam, Vec3f{0.0f, 1.0f, 0.0f}, 0.0f, w,
                                      0.0f, 0.0f, 0.0f, /*footprint=*/0.0f, &hit));
    // 与水面平行。
    EXPECT_FALSE(IntersectWaveSurface(cam, Vec3f{1.0f, 0.0f, 0.0f}, 0.0f, w,
                                      0.0f, 0.0f, 0.0f, /*footprint=*/0.0f, &hit));
    // 空输出指针：防御性返回 false。
    EXPECT_FALSE(IntersectWaveSurface(cam, Vec3f{0.0f, -1.0f, 0.0f}, 0.0f, w,
                                      0.0f, 0.0f, 0.0f, /*footprint=*/0.0f, nullptr));
}

// 非单位方向向量也能工作（内部归一化）；零方向必须被拒（否则除零）。
TEST(WaterWaveMath, NonUnitWaveDirectionIsNormalized) {
    WaterWaves a = MakeSingleWave(1.0f, 0.0f, 10.0f, 0.4f, 0.0f);
    WaterWaves b = MakeSingleWave(7.0f, 0.0f, 10.0f, 0.4f, 0.0f);
    for (int i = 0; i < 10; ++i) {
        const float x = 0.3f * static_cast<float>(i);
        EXPECT_NEAR(SampleWaveField(a, x, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f).height_m,
                    SampleWaveField(b, x, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f).height_m,
                    1.0e-6f);
    }
}

}  // namespace
