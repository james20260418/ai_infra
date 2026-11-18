// JPOV 水皮 CPU 单测（GL-free）—— 钉住界面物理的四条判据
//
// 这些断言都是「换一种写法就会红」的硬判据（每条都想过反例）：
//   · 临界角：ior=1.333 ⇒ cosθc ≈ 0.6614（θc ≈ 48.6°），TIR 在它「内侧」发生；
//   · 菲涅尔：正入射 2%、掠射 → 1，且水上/水下用同一个 n 自洽；
//   · 交点：视线与平面平行/背离相机时**没有**交点（不是 t=0 或 inf）；
//   · 深度拒绝：水面比不透明场景更远 ⇒ 不画；背景（远平面）⇒ 画。
//
// 与着色器的一致性由 test/water:water_skin_gpu_test 的逐像素对拍兜底。

#include <cmath>

#include <gtest/gtest.h>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/lighting/ggx_specular.h"
#include "tools/jpov/src/water/water_skin_math.h"

namespace {

using jpov::Vec3f;
using jpov::water::ClampReflectionAboveHorizon;
using jpov::water::CriticalCos;
using jpov::water::EvaluateWaterPixel;
using jpov::water::FresnelDielectric;
using jpov::water::kDepthBiasM;
using jpov::water::kWaterIorDefault;
using jpov::water::LinearizeDepthNdc;
using jpov::water::PlaneHitDistance;
using jpov::water::ReflectDirection;
using jpov::water::WaterPixel;

constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;

// ───────────────────────── 临界角 ─────────────────────────

TEST(WaterSkinMath, CriticalCosMatchesSnell) {
    // cosθc = sqrt(1 − 1/n²)；n=1.333 ⇒ 0.66123（θc ≈ 48.62°，文献常说 48.6°）。
    EXPECT_NEAR(CriticalCos(kWaterIorDefault), 0.66123f, 1.0e-4f);
    // 反射率在临界角两侧应该是「阶跃」：窗内（更靠近法线）略小于 1，窗外恒 1。
    EXPECT_LT(FresnelDielectric(0.67f, kWaterIorDefault, 1.0f), 1.0f);
    EXPECT_EQ(FresnelDielectric(0.66f, kWaterIorDefault, 1.0f), 1.0f);
    // ior <= 1：没有全内反射（临界角退化为「只有正入射能透射」）。
    EXPECT_EQ(CriticalCos(1.0f), 1.0f);
    EXPECT_LT(FresnelDielectric(0.5f, 1.0f, 1.0f), 1.0e-6f);
}

// ───────────────────────── 菲涅尔 ─────────────────────────

TEST(WaterSkinMath, FresnelNormalIncidenceIsTwoPercent) {
    // ((n−1)/(n+1))² = (0.333/2.333)² = 0.02037
    EXPECT_NEAR(FresnelDielectric(1.0f, 1.0f, kWaterIorDefault), 0.02037f, 2.0e-4f);
    // 同一界面反向（水→空气）在正入射时反射率相同（能量口径对称）。
    EXPECT_NEAR(FresnelDielectric(1.0f, kWaterIorDefault, 1.0f), 0.02037f, 2.0e-4f);
}

TEST(WaterSkinMath, FresnelGrazingGoesToOne) {
    EXPECT_GT(FresnelDielectric(0.02f, 1.0f, kWaterIorDefault), 0.85f);
    EXPECT_GT(FresnelDielectric(0.002f, 1.0f, kWaterIorDefault), 0.98f);
    // 单调：入射角越大（cos 越小）反射越强。
    EXPECT_LT(FresnelDielectric(0.9f, 1.0f, kWaterIorDefault),
              FresnelDielectric(0.3f, 1.0f, kWaterIorDefault));
    // 水下掠射 = 全内反射，恒 1。
    EXPECT_EQ(FresnelDielectric(0.1f, kWaterIorDefault, 1.0f), 1.0f);
}

// ───────────────────────── 平面交点 ─────────────────────────

TEST(WaterSkinMath, PlaneHitRequiresForwardRay) {
    float t = -1.0f;
    // 相机在 y=10，视线向下斜（dir.y=-0.5）⇒ t = 10/0.5 = 20。
    ASSERT_TRUE(PlaneHitDistance(/*cam_y*/ 10.0f, /*level*/ 0.0f, /*dir_y*/ -0.5f, &t));
    EXPECT_NEAR(t, 20.0f, 1.0e-4f);

    // 相机在水下、向上看 ⇒ 同样有交点（同一公式，不靠相机上下分支）。
    ASSERT_TRUE(PlaneHitDistance(-4.0f, 0.0f, 0.5f, &t));
    EXPECT_NEAR(t, 8.0f, 1.0e-4f);

    // 背离平面 ⇒ 交点在相机后方，必须拒绝（不能返回负 t）。
    EXPECT_FALSE(PlaneHitDistance(10.0f, 0.0f, 0.5f, &t));
    // 与平面平行 ⇒ 没有交点（t → ∞，不能当成「很远但有效」）。
    EXPECT_FALSE(PlaneHitDistance(10.0f, 0.0f, 0.0f, &t));
    // 空输出指针：防御性返回 false，不崩。
    EXPECT_FALSE(PlaneHitDistance(10.0f, 0.0f, -0.5f, nullptr));
}

TEST(WaterSkinMath, ReflectionFlipsYAndStaysAboveHorizon) {
    const Vec3f r = ReflectDirection(Vec3f{0.0f, -1.0f, 0.0f});
    EXPECT_NEAR(r.y(), 1.0f, 1.0e-6f);

    // 掠射（y≈0）也必须被抬到地平线以上 —— 探针下半球是"地色"，不抬会出黑边。
    const Vec3f c = ClampReflectionAboveHorizon(Vec3f{1.0f, 0.0f, 0.0f});
    EXPECT_GE(c.y(), jpov::water::kHorizonClampSin - 1.0e-6f);
    EXPECT_NEAR(std::sqrt(c.x() * c.x() + c.y() * c.y() + c.z() * c.z()), 1.0f, 1.0e-6f);
}

TEST(WaterSkinMath, LinearizeDepthMatchesEndpoints) {
    // ndc_z = 0 ⇒ 近平面；ndc_z = 1 ⇒ 远平面（MRT#1 的 1.0 = 背景）。
    EXPECT_NEAR(LinearizeDepthNdc(0.0f, kNear, kFar), kNear, 1.0e-4f);
    EXPECT_NEAR(LinearizeDepthNdc(1.0f, kNear, kFar), kFar, 1.0f);
    // ndc_z = 0.5（NDC 0）⇒ 2nf/(f+n)。
    EXPECT_NEAR(LinearizeDepthNdc(0.5f, kNear, kFar),
                2.0f * kNear * kFar / (kFar + kNear), 1.0e-3f);
}

// ───────────────────────── 逐像素判定 ─────────────────────────

TEST(WaterSkinMath, PixelRejectedWhenSceneIsCloser) {
    // 相机在 y=10 俯视 45°，水面交点在 14.1 m 处；场景在 5 m ⇒ 水面被挡住。
    const Vec3f fwd{0.0f, -0.70710678f, 0.70710678f};
    const Vec3f rd = fwd;
    const Vec3f cam{0.0f, 10.0f, 0.0f};
    // 场景深度 5 m 的 NDC：ndc = (f+n−2nf/z)/(f−n)；这里直接算好再转 [0,1]。
    const float ndc5 = ((kFar + kNear) - 2.0f * kNear * kFar / 5.0f) / (kFar - kNear);
    const float ndc5_01 = 0.5f * ndc5 + 0.5f;

    WaterPixel px;
    EXPECT_FALSE(EvaluateWaterPixel(cam, fwd, rd, 0.0f, kWaterIorDefault, ndc5_01,
                                    kNear, kFar, &px));
    // 背景（远平面）⇒ 必须画（否则水平线附近会留一条地色带）。
    EXPECT_TRUE(EvaluateWaterPixel(cam, fwd, rd, 0.0f, kWaterIorDefault, 1.0f,
                                   kNear, kFar, &px));
    EXPECT_TRUE(px.hit);
    // 视轴深度：14.14 m（45° 斜视），不是欧氏距离 14.14…两者在这里相等；换个斜角验证见下。
    EXPECT_NEAR(px.view_z, std::sqrt(200.0f), 1.0e-2f);
    // 45° 入射 ⇒ 反射率约 2.5%（比正入射 2% 略高，绝不可能是 0）。
    EXPECT_GT(px.reflectance, 0.02f);
    EXPECT_LT(px.reflectance, 0.06f);
}

TEST(WaterSkinMath, ViewZIsAxisDepthNotEuclidean) {
    // 俯视 60°（dir.y = −0.866），相机 y=10 ⇒ t = 11.55 m。
    // 视轴深度 = t·dot(rd, fwd)。取 fwd 与 rd 不同：相机朝向水平、像素射线偏下 30°，
    // 这时视轴深度 = t·cos(30°) < t —— 必须按视轴深度比较（MRT#1 的口径）。
    const Vec3f fwd{0.0f, 0.0f, 1.0f};
    const Vec3f rd{0.0f, -0.5f, 0.8660254f};
    WaterPixel px;
    ASSERT_TRUE(EvaluateWaterPixel(Vec3f{0.0f, 10.0f, 0.0f}, fwd, rd, 0.0f,
                                   kWaterIorDefault, 1.0f, kNear, kFar, &px));
    const float t = 20.0f;   // 10 / 0.5
    EXPECT_NEAR(px.view_z, t * 0.8660254f, 1.0e-3f);
    EXPECT_LT(px.view_z, t);   // 明确区别于「欧氏距离」
    // 反射率按 |rd.y| = 0.5（60°）算：应在 5%~10% 之间。
    EXPECT_GT(px.reflectance, 0.05f);
    EXPECT_LT(px.reflectance, 0.10f);
}

TEST(WaterSkinMath, UnderwaterSwitchesToTirInsideWindow) {
    const Vec3f cam{0.0f, -10.0f, 0.0f};
    const Vec3f fwd{0.0f, 1.0f, 0.0f};
    WaterPixel px;
    // 正上方（cos_i = 1）⇒ 窗内，反射率 ≈ 2%。
    ASSERT_TRUE(EvaluateWaterPixel(cam, fwd, fwd, 0.0f, kWaterIorDefault, 1.0f,
                                   kNear, kFar, &px));
    EXPECT_NEAR(px.reflectance, 0.02037f, 2.0e-4f);
    // 与竖直成 60°（cos_i = 0.5 < 0.661）⇒ 窗外 ⇒ TIR，反射率 1。
    const Vec3f rd{0.8660254f, 0.5f, 0.0f};
    ASSERT_TRUE(EvaluateWaterPixel(cam, fwd, rd, 0.0f, kWaterIorDefault, 1.0f,
                                   kNear, kFar, &px));
    EXPECT_EQ(px.reflectance, 1.0f);
}

TEST(WaterSkinMath, DepthBiasToleranceIsTight) {
    // 场景正好在水面深度上（差 1 cm < kDepthBiasM）⇒ 仍然画（贴水面的几何不该把水整片剔除）。
    const Vec3f fwd{0.0f, -1.0f, 0.0f};
    const Vec3f cam{0.0f, 10.0f, 0.0f};
    const float z = 10.0f - 0.01f;   // 比水面近 1 cm
    const float ndc = ((kFar + kNear) - 2.0f * kNear * kFar / z) / (kFar - kNear);
    WaterPixel px;
    EXPECT_TRUE(EvaluateWaterPixel(cam, fwd, fwd, 0.0f, kWaterIorDefault,
                                   0.5f * ndc + 0.5f, kNear, kFar, &px));
    // 近 5 cm（> kDepthBiasM）⇒ 判定被挡住。
    ASSERT_TRUE(kDepthBiasM < 0.05f);
    const float z2 = 10.0f - 0.05f;
    const float ndc2 = ((kFar + kNear) - 2.0f * kNear * kFar / z2) / (kFar - kNear);
    EXPECT_FALSE(EvaluateWaterPixel(cam, fwd, fwd, 0.0f, kWaterIorDefault,
                                    0.5f * ndc2 + 0.5f, kNear, kFar, &px));
}

// ─────────────── 太阳的像（解析 GGX，盘状光源）───────────────
//
// 这组断言对应 docs/jpov_specular_bandwidth.md：镜面高光的带宽 = max(材质 roughness, 光源角半径, 像素足迹)。
// 水皮没有 roughness ⇒ 带宽完全来自 θ 与足迹，所以这三条必须钉死。

// 带宽 = (θ/2)²。写成 θ² 会过宽 4 倍，这条会立刻红。
TEST(GgxSunDisk, BandwidthIsQuarterThetaSquared) {
    const float theta = 0.004654f;
    const float a2 = jpov::lighting::GgxDiskBandwidthA2(theta);
    EXPECT_NEAR(a2, 0.25f * theta * theta, 1e-12f);
    // 反向对照：与“直接用 θ²”差 4 倍（别把它们写成一样）。
    EXPECT_NEAR(a2 * 4.0f, theta * theta, 1e-12f);
}

// GGX 的 D 归一化：∫ D(h)(n·h) dω_h = 1 —— 这就是「加带宽不会爆亮、只是摊开」的数学依据。
// 用斜率换元 t = √a2·u 消除尖峰（小 a2 时直接积分会欠采样）。
// 若把 D 里的 a2 换成 a、或漏掉 cos 项，这条会红。
TEST(GgxSunDisk, DistributionIntegratesToOne) {
    const float kA2[] = {1.0e-6f, 5.41e-6f, 1.0e-3f, 1.0e-1f};
    for (float a2 : kA2) {
        const float k = std::sqrt(a2);
        // u ∈ [0, 200] 的 Simpson（尾项衰减为 1/u³，截断误差 < 1e-4）。
        const int kN = 200000;
        const float kUmax = 200.0f;
        const float du = kUmax / kN;
        double sum = 0.0;
        for (int i = 0; i <= kN; ++i) {
            const double u = i * static_cast<double>(du);
            const double t = k * u;
            const double no_h = 1.0 / std::sqrt(1.0 + t * t);
            const double d = jpov::lighting::GgxDistribution(
                static_cast<float>(no_h), a2);
            // dθ/du = k / (1 + t²)；sinθ = t/√(1+t²)
            const double dtheta_du = k / (1.0 + t * t);
            const double sin_theta = t / std::sqrt(1.0 + t * t);
            double w = (i == 0 || i == kN) ? 1.0 : ((i % 2 == 0) ? 2.0 : 4.0);
            sum += w * 2.0 * 3.14159265358979323846 * d * no_h * sin_theta * dtheta_du;
        }
        sum *= du / 3.0;
        // 容差 2%：float 在 NoH→1 处有 ~0.3% 的**抵消误差**（`1 − NoH²(1−a2)` 在小 a2 时
        // 丢掉有效位），a2 越小越明显。这里测的是归一化，不是峰值的绝对精度。
        EXPECT_NEAR(sum, 1.0, 2e-2) << "a2=" << a2;
    }
}

// 正对太阳的镜面方向（法线=视线=太阳方向）：峰值必须是 E·F·D(1)/(4)。
// 这条同时钉住「半边向量把角半径折半」与「÷(4 NoV NoL)×NoL」的口径。
TEST(GgxSunDisk, PeakMatchesClosedForm) {
    const Vec3f n{0.0f, 1.0f, 0.0f};
    const Vec3f v{0.0f, 1.0f, 0.0f};
    const Vec3f l{0.0f, 1.0f, 0.0f};
    const float theta = 0.004654f;
    const float fresnel = 0.02f;
    const Vec3f e{2.0f, 2.0f, 2.0f};
    const Vec3f got = jpov::lighting::SunDiskSpecularRadiance(
        n, v, l, fresnel, theta, /*extra_a2=*/0.0f, e);

    const float a2 = jpov::lighting::GgxDiskBandwidthA2(theta);
    const float expected = 2.0f * fresnel * (1.0f / (3.14159265358979323846f * a2)) / 4.0f;
    // 容差 1%：同上，float 抵消误差（实测 0.33%）。峰值本身是 HDR 值、tone map 后全白，
    // 这点误差没有画面意义 —— 但**数量级**必须对，所以这里不能放宽到 10%。
    EXPECT_NEAR(got.x(), expected, expected * 1e-2f);
    EXPECT_NEAR(got.y(), expected, expected * 1e-2f);
}

// 太阳在地平线下 / 相机在水下看上方 ⇒ 没有日盘像（不是“很暗”，是恒 0）。
TEST(GgxSunDisk, NoImageWhenSunOrViewIsBelowHorizon) {
    const Vec3f n{0.0f, 1.0f, 0.0f};
    const Vec3f v{0.0f, 1.0f, 0.0f};
    const Vec3f e{2.0f, 2.0f, 2.0f};
    const Vec3f sun_below{0.6f, -0.8f, 0.0f};
    const Vec3f cam_below{0.0f, -1.0f, 0.0f};
    const float theta = 0.004654f;
    Vec3f got = jpov::lighting::SunDiskSpecularRadiance(
        n, v, sun_below, 0.02f, theta, 0.0f, e);
    EXPECT_EQ(got.x(), 0.0f);
    got = jpov::lighting::SunDiskSpecularRadiance(
        n, cam_below, v, 0.02f, theta, 0.0f, e);
    EXPECT_EQ(got.x(), 0.0f);
    // 角半径为 0（配置错误）也不得产出 inf/NaN。
    got = jpov::lighting::SunDiskSpecularRadiance(
        n, v, v, 0.02f, 0.0f, 0.0f, e);
    EXPECT_EQ(got.x(), 0.0f);
}

// 像素足迹（掠射压缩的正解）只能让高光**变矮变宽**，绝不变亮。
TEST(GgxSunDisk, FootprintLowersPeak) {
    const Vec3f n{0.0f, 1.0f, 0.0f};
    const Vec3f v{0.0f, 1.0f, 0.0f};
    const Vec3f l{0.0f, 1.0f, 0.0f};
    const Vec3f e{2.0f, 2.0f, 2.0f};
    const float theta = 0.004654f;
    const Vec3f no_fp = jpov::lighting::SunDiskSpecularRadiance(
        n, v, l, 0.02f, theta, /*extra_a2=*/0.0f, e);
    const Vec3f with_fp = jpov::lighting::SunDiskSpecularRadiance(
        n, v, l, 0.02f, theta, /*extra_a2=*/1.0e-4f, e);
    EXPECT_LT(with_fp.x(), no_fp.x());
    // 峰值 ≈ 1/a2 ⇒ a2 扩了 (θ/2)²+1e-4 倍，峰值就该缩到 1/那倍。
    const float a2_ref = jpov::lighting::GgxDiskBandwidthA2(theta);
    const float ratio_expected = a2_ref / (a2_ref + 1.0e-4f);
    EXPECT_NEAR(with_fp.x() / no_fp.x(), ratio_expected, ratio_expected * 2e-2f);
}

}  // namespace
