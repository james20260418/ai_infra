// 广告牌粒子火 模拟内核单测（GL-free）。
// 断言刻意选取「能区分对错」的量（非恒真）：精确计数、逐位确定性、曲线端点/中点值。

#include "tools/jpov/src/particles/particle_system.h"

#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace particles {
namespace {

// 造一个「大部分特性关掉」的基线配置，便于隔离测试单个特性。
EmitterConfig BaselineConfig() {
    EmitterConfig c;
    c.rate = 0.0f;
    c.burst = 0;
    c.shape = ShapeKind::kPoint;
    c.lifetime = {1e6f, 1e6f};
    c.speed = {0.0f, 0.0f};
    c.size_start = {1.0f, 1.0f};
    c.rotation = {0.0f, 0.0f};
    c.gravity = {0.0f, 0.0f, 0.0f};
    c.drag = 0.0f;
    c.noise_enabled = false;
    c.use_color_over_life = false;
    c.use_size_over_life = false;
    c.flipbook = false;
    return c;
}

TEST(ParticleSystemTest, RngIsDeterministicAndInUnitRange) {
    Rng a(12345u);
    Rng b(12345u);
    for (int i = 0; i < 100; ++i) {
        const float va = a.Next01();
        const float vb = b.Next01();
        EXPECT_FLOAT_EQ(va, vb);           // 同种子逐位一致
        EXPECT_GE(va, 0.0f);
        EXPECT_LT(va, 1.0f);
    }
}

TEST(ParticleSystemTest, RngDifferentSeedsDiverge) {
    Rng a(1u);
    Rng b(2u);
    // 至少有一个采样不同（否则种子无意义）。
    bool any_diff = false;
    for (int i = 0; i < 16; ++i) {
        if (a.Next01() != b.Next01()) {
            any_diff = true;
        }
    }
    EXPECT_TRUE(any_diff);
}

TEST(ParticleSystemTest, CurveEvaluatesEndpointsAndMidpoint) {
    Curve c;
    c.keys = {{0.0f, 2.0f}, {1.0f, 6.0f}};
    EXPECT_FLOAT_EQ(c.Eval(0.0f), 2.0f);
    EXPECT_FLOAT_EQ(c.Eval(1.0f), 6.0f);
    EXPECT_FLOAT_EQ(c.Eval(0.5f), 4.0f);   // 线性中点
    EXPECT_FLOAT_EQ(c.Eval(-1.0f), 2.0f);  // 端点外取端点
    EXPECT_FLOAT_EQ(c.Eval(2.0f), 6.0f);
}

TEST(ParticleSystemTest, GradientEvaluatesEndpointsAndMidpoint) {
    Gradient g;
    g.keys = {{0.0f, Color{0.0f, 0.0f, 0.0f, 0.0f}},
              {1.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}}};
    const Color mid = g.Eval(0.5f);
    EXPECT_FLOAT_EQ(mid.r, 0.5f);
    EXPECT_FLOAT_EQ(mid.a, 0.5f);
    EXPECT_FLOAT_EQ(g.Eval(0.0f).r, 0.0f);
    EXPECT_FLOAT_EQ(g.Eval(1.0f).a, 1.0f);
}

TEST(ParticleSystemTest, EmissionRateProducesExpectedCount) {
    EmitterConfig c = BaselineConfig();
    c.rate = 10.0f;              // 10/s
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    for (int i = 0; i < 10; ++i) {
        e.Update(0.1f, &out);    // 10 × 0.1s = 1s ⇒ 约 10 个
    }
    EXPECT_EQ(e.live_count(), 10);
    EXPECT_EQ(static_cast<int>(out.size()), 55);  // Update 追加：第 k 帧有 k 个存活 ⇒ 1+..+10
}

TEST(ParticleSystemTest, BurstEmitsOnFirstUpdate) {
    EmitterConfig c = BaselineConfig();
    c.burst = 7;
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    e.Update(0.016f, &out);
    EXPECT_EQ(e.live_count(), 7);
}

TEST(ParticleSystemTest, ParticlesRecycleAfterLifetime) {
    EmitterConfig c = BaselineConfig();
    c.rate = 100.0f;             // 100/s
    c.lifetime = {0.2f, 0.2f};   // 恰好 0.2s
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    // 跑 1 秒：稳态存活数 ≈ rate × lifetime = 20。
    for (int i = 0; i < 100; ++i) {
        e.Update(0.01f, &out);
    }
    EXPECT_GE(e.live_count(), 15);
    EXPECT_LE(e.live_count(), 25);
}

TEST(ParticleSystemTest, DeterministicGivenSameSeed) {
    EmitterConfig c = BaselineConfig();
    c.rate = 50.0f;
    c.shape = ShapeKind::kCone;
    c.radius = 0.3f;
    c.cone_half_angle_deg = 25.0f;
    c.speed = {0.5f, 1.5f};
    c.noise_enabled = true;
    c.noise_amp = 1.0f;
    c.lifetime = {0.8f, 1.2f};

    std::vector<ParticleInstance> a;
    std::vector<ParticleInstance> b;
    ParticleEmitter ea(c);
    ParticleEmitter eb(c);
    ea.Reset(777u);
    eb.Reset(777u);
    for (int i = 0; i < 30; ++i) {
        ea.Update(1.0f / 60.0f, &a);
        eb.Update(1.0f / 60.0f, &b);
    }
    ASSERT_EQ(a.size(), b.size());
    ASSERT_GT(a.size(), 0u);
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i].position.x(), b[i].position.x());
        EXPECT_FLOAT_EQ(a[i].position.y(), b[i].position.y());
        EXPECT_FLOAT_EQ(a[i].frame, b[i].frame);
    }
}

TEST(ParticleSystemTest, FlipbookFpsFrameAdvancesOverLife) {
    EmitterConfig c = BaselineConfig();
    c.burst = 1;
    c.flipbook = true;
    c.tiles_x = 4;
    c.tiles_y = 4;                       // 16 帧
    c.sheet_time_mode = SheetTimeMode::kFps;
    c.sheet_fps = 16.0f;                 // 1 圈/秒
    c.start_frame_random = false;        // 相位固定 0

    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    e.Update(0.5f, &out);                // age=0.5 ⇒ phase 0.5 ⇒ frame 8
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].frame, 8.0f);
}

TEST(ParticleSystemTest, SizeOverLifeChangesWithAge) {
    EmitterConfig c = BaselineConfig();
    c.burst = 2;
    c.use_size_over_life = true;
    c.size_curve.keys = {{0.0f, 0.5f}, {1.0f, 2.0f}};
    c.size_start = {1.0f, 1.0f};
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    e.Update(0.0001f, &out);             // t≈0 ⇒ size≈0.5
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NEAR(out[0].width, 0.5f, 0.01f);
}

TEST(ParticleSystemTest, SpinAdvancesRotationAndDecays) {
    EmitterConfig c = BaselineConfig();
    c.burst = 1;
    c.lifetime = {2.0f, 2.0f};
    c.rotation = {0.0f, 0.0f};
    c.rotation_speed = {2.0f, 2.0f};
    c.rotation_speed_decay = 0.5f;
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;

    // age=0.5 ⇒ t=0.25；spin = 2·(1−0.5·0.25) = 1.75；rotation = 0.875。
    e.Update(0.5f, &out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].rotation, 0.875f, 1e-4f);

    // age=1.0 ⇒ t=0.5；spin = 1.5；rotation = 0.875 + 0.75 = 1.625。
    out.clear();
    e.Update(0.5f, &out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].rotation, 1.625f, 1e-4f);
}

TEST(ParticleSystemTest, ValidationRejectsBadConfig) {
    EmitterConfig c = BaselineConfig();
    c.rate = -1.0f;
    EXPECT_DEATH({ ParticleEmitter e(c); (void)e; }, "rate");
}

TEST(ParticleSystemTest, DeathsReportedAndSpawnAtInjects) {
    // 死亡上报（子发射器用）
    EmitterConfig c = BaselineConfig();
    c.rate = 100.0f;
    c.lifetime = {0.2f, 0.2f};
    ParticleEmitter e(c);
    e.Reset(1u);
    std::vector<ParticleInstance> out;
    std::vector<Vec3f> deaths;
    for (int i = 0; i < 40; ++i) {
        e.Update(0.01f, &out, &deaths);
    }
    EXPECT_GT(deaths.size(), 0u);   // 0.4s 内必有粒子寿终

    // SpawnAt：在指定位置注入一个粒子（不依赖 rate）。
    EmitterConfig s = BaselineConfig();   // rate=0
    ParticleEmitter injector(s);
    injector.Reset(2u);
    EXPECT_EQ(injector.live_count(), 0);
    injector.SpawnAt(Vec3f(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(injector.live_count(), 1);
    std::vector<ParticleInstance> so;
    injector.Update(0.001f, &so);
    ASSERT_EQ(so.size(), 1u);
    EXPECT_NEAR(so[0].position.x(), 1.0f, 0.01f);
    EXPECT_NEAR(so[0].position.y(), 2.0f, 0.01f);
    EXPECT_NEAR(so[0].position.z(), 3.0f, 0.01f);
}

}  // namespace
}  // namespace particles
}  // namespace jpov
