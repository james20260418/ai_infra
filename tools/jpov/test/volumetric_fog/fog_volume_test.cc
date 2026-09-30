// JPOV 局部体积雾 — 闭式积分核单测（纯 CPU，对照 Simpson 数值积分）
//
// 验证 fog_volume.h 的解析 τ 与数值积分一致（这是整个雾正确性的地基）。

#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "tools/jpov/src/volumetric_fog/fog_volume.h"

namespace jpov {
namespace volumetric_fog {
namespace {

using jpov::FogCylinder;
using jpov::FogProfile;
using jpov::FogSphere;

// 构造一个单位方向（把任意向量归一化）。
void Unit(const double v[3], double out[3]) {
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    out[0] = v[0] / len;
    out[1] = v[1] / len;
    out[2] = v[2] / len;
}

// ---- 球：全弦解析值 —— kDome 应 = σ₀·(4/3)L³/r²（设计文档 §6.2 闭式核对）----
TEST(FogVolumeTest, SphereDomeFullChordMatchesFormula) {
    const double r = 2.0;
    const double sigma = 1.7;
    FogSphere f;
    f.center = {0.0f, 0.0f, 0.0f};
    f.radius = static_cast<float>(r);
    f.intensity = static_cast<float>(sigma);
    f.profile = FogProfile::kDome;

    // 视线沿 +x，从 x=-10 射向 +x（穿过球心，h=0 ⇒ L=r）。
    const double o[3] = {-10.0, 0.0, 0.0};
    const double d[3] = {1.0, 0.0, 0.0};
    const double tau = SphereTau(f, o, d, 0.0, 1e30);
    const double expect = sigma * (4.0 / 3.0) * r * r * r / (r * r);  // = σ₀·(4/3)·r
    EXPECT_NEAR(tau, expect, 1e-6);   // float32 输入 → 放宽
}

TEST(FogVolumeTest, SphereSharpFullChordMatchesFormula) {
    const double r = 2.0;
    const double sigma = 1.7;
    FogSphere f;
    f.radius = static_cast<float>(r);
    f.intensity = static_cast<float>(sigma);
    f.profile = FogProfile::kSharp;
    const double o[3] = {-10.0, 0.0, 0.0};
    const double d[3] = {1.0, 0.0, 0.0};
    const double tau = SphereTau(f, o, d, 0.0, 1e30);
    const double expect = sigma * (16.0 / 15.0) * (r * r * r * r * r) / (r * r * r * r);
    EXPECT_NEAR(tau, expect, 1e-6);   // float32 输入 → 放宽
}

// ---- 球：随机视线 + 深度裁剪，对照 Simpson ----
TEST(FogVolumeTest, SphereMatchesNumericRandom) {
    std::mt19937 g(12345);
    std::uniform_real_distribution<double> un(-1.0, 1.0);
    std::uniform_real_distribution<double> un01(0.0, 1.0);
    for (int iter = 0; iter < 400; ++iter) {
        FogSphere f;
        f.center = {static_cast<float>(3.0 * un(g)), static_cast<float>(3.0 * un(g)),
                    static_cast<float>(3.0 * un(g))};
        f.radius = static_cast<float>(0.3 + 3.0 * un01(g));
        f.intensity = static_cast<float>(0.1 + 4.0 * un01(g));
        f.profile = (iter % 2 == 0) ? FogProfile::kDome : FogProfile::kSharp;

        const double o[3] = {static_cast<double>(6.0 * un(g)),
                             static_cast<double>(6.0 * un(g)),
                             static_cast<double>(6.0 * un(g))};
        const double raw[3] = {un(g), un(g), un(g)};
        double d[3];
        Unit(raw, d);
        const double t_max = 0.3 + 20.0 * un01(g);

        const double analytic = SphereTau(f, o, d, 0.0, t_max);
        const double numeric = SphereTauReference(f, o, d, 0.0, t_max, 4000);
        EXPECT_NEAR(analytic, numeric, 1e-4)
            << "iter=" << iter << " analytic=" << analytic << " numeric=" << numeric;
    }
}

TEST(FogVolumeTest, SphereMissIsZero) {
    FogSphere f;
    f.center = {0.0f, 0.0f, 0.0f};
    f.radius = 1.0f;
    const double o[3] = {-10.0, 5.0, 0.0};   // 从上方掠过，h=5 > r
    const double d[3] = {1.0, 0.0, 0.0};
    EXPECT_DOUBLE_EQ(SphereTau(f, o, d, 0.0, 1e30), 0.0);
}

// ---- 圆柱：对照 Simpson（对圆柱写一个通用数值参考）----
double CylinderTauReference(const FogCylinder& f, const double o[3],
                            const double d[3], double n_a, double n_b, int n) {
    FogCylinder g = f;
    const double len = std::sqrt(g.axis.x() * g.axis.x() + g.axis.y() * g.axis.y() +
                                 g.axis.z() * g.axis.z());
    const double ax = g.axis.x() / len, ay = g.axis.y() / len, az = g.axis.z() / len;
    const double R2 = static_cast<double>(g.radius) * g.radius;
    const double h = g.height;
    if (n % 2 == 1) ++n;
    const double step = (n_b - n_a) / n;
    double sum = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double t = n_a + step * i;
        const double px = o[0] + t * d[0] - g.base.x();
        const double py = o[1] + t * d[1] - g.base.y();
        const double pz = o[2] + t * d[2] - g.base.z();
        const double y = px * ax + py * ay + pz * az;
        double rx = px - y * ax, ry = py - y * ay, rz = pz - y * az;
        const double rho2 = rx * rx + ry * ry + rz * rz;
        double fr = 1.0 - rho2 / R2;
        if (fr < 0.0) fr = 0.0;
        double fy = 1.0;
        if (g.axial_profile == FogProfile::kDomeAxial) {
            const double uy2 = (y * y) / (h * h);
            fy = 1.0 - uy2;
            if (fy < 0.0) fy = 0.0;
        }
        if (y < 0.0 || y > h) fr = 0.0;   // 轴向支撑
        double dens = static_cast<double>(g.intensity) * fr * fy;
        if (g.radial_profile == FogProfile::kSharp) {
            dens = static_cast<double>(g.intensity) * fr * fr * fy;
        }
        const double w = (i == 0 || i == n) ? 1.0 : ((i % 2) ? 4.0 : 2.0);
        sum += w * dens;
    }
    return sum * step / 3.0;
}

TEST(FogVolumeTest, CylinderUniformOnAxisMatchesFormula) {
    // 沿轴看：ρ=0，f_r=1（kDome）；kUniform ⇒ 密度恒 σ₀，弦长 = 高度（h=4）。
    FogCylinder f;
    f.base = {0.0f, 0.0f, 0.0f};
    f.axis = {0.0f, 1.0f, 0.0f};
    f.radius = 1.0f;
    f.height = 4.0f;
    f.intensity = 2.0f;
    f.radial_profile = FogProfile::kDome;
    f.axial_profile = FogProfile::kUniform;
    const double o[3] = {0.0, -10.0, 0.0};
    const double d[3] = {0.0, 1.0, 0.0};
    const double tau = CylinderTau(f, o, d, 0.0, 1e30);
    EXPECT_NEAR(tau, 2.0 * 4.0, 1e-6);   // σ₀ · 高度（ρ=0 处 f_r=1）
}

TEST(FogVolumeTest, CylinderMatchesNumericRandom) {
    std::mt19937 g(999);
    std::uniform_real_distribution<double> un(-1.0, 1.0);
    std::uniform_real_distribution<double> un01(0.0, 1.0);
    for (int iter = 0; iter < 400; ++iter) {
        FogCylinder f;
        f.base = {static_cast<float>(2.0 * un(g)), static_cast<float>(2.0 * un(g)),
                  static_cast<float>(2.0 * un(g))};
        const double axr[3] = {un(g), 0.3 + un01(g), un(g)};   // 轴偏 y，避免退化
        double axn[3];
        Unit(axr, axn);
        f.axis = {static_cast<float>(axn[0]), static_cast<float>(axn[1]),
                  static_cast<float>(axn[2])};
        f.radius = static_cast<float>(0.3 + 2.0 * un01(g));
        f.height = static_cast<float>(0.5 + 4.0 * un01(g));
        f.intensity = static_cast<float>(0.1 + 3.0 * un01(g));
        f.radial_profile = (iter % 2 == 0) ? FogProfile::kDome : FogProfile::kSharp;
        f.axial_profile =
            (iter % 3 == 0) ? FogProfile::kUniform : FogProfile::kDomeAxial;

        const double o[3] = {static_cast<double>(6.0 * un(g)),
                             static_cast<double>(6.0 * un(g)),
                             static_cast<double>(6.0 * un(g))};
        const double raw[3] = {un(g), un(g), un(g)};
        double d[3];
        Unit(raw, d);
        const double t_max = 0.3 + 20.0 * un01(g);

        const double analytic = CylinderTau(f, o, d, 0.0, t_max);
        const double numeric = CylinderTauReference(f, o, d, 0.0, t_max, 8000);
        EXPECT_NEAR(analytic, numeric, 2e-3)
            << "iter=" << iter << " analytic=" << analytic << " numeric=" << numeric;
    }
}

TEST(FogVolumeTest, DegenerateInputsAreZero) {
    {  // 半径 0
        FogSphere f;
        f.radius = 0.0f;
        const double o[3] = {0, 0, -5}, d[3] = {0, 0, 1};
        EXPECT_DOUBLE_EQ(SphereTau(f, o, d, 0.0, 1e30), 0.0);
    }
    {  // 强度 0
        FogSphere f;
        f.intensity = 0.0f;
        const double o[3] = {0, 0, -5}, d[3] = {0, 0, 1};
        EXPECT_DOUBLE_EQ(SphereTau(f, o, d, 0.0, 1e30), 0.0);
    }
    {  // 圆柱轴为零向量
        FogCylinder f;
        f.axis = {0.0f, 0.0f, 0.0f};
        const double o[3] = {0, 0, -5}, d[3] = {0, 0, 1};
        EXPECT_DOUBLE_EQ(CylinderTau(f, o, d, 0.0, 1e30), 0.0);
    }
}

}  // namespace
}  // namespace volumetric_fog
}  // namespace jpov
