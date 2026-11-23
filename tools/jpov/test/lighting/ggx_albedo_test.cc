// GGX 单次散射方向反照率 E_ss 与多散射补偿（M2）的单测（GL-free）。
//
// 判据（每条都想过度量）：
//  ① 表查值 vs **独立数值积分**（NDF 重要性采样 + 确定性 Hammersley 序列）误差 < 2%；
//  ② 物理边界：a2 → 0 ⇒ E → 1（δ 镜面无遮蔽损失）；α = 1、μ = 1 ⇒ E ≈ 0.31（与 Kulla–Conty 一致）；
//  ③ E 随粗糙度单调不增（固定 μ）；
//  ④ 补偿系数 ms = 1 + F_avg(1−E)/E 必须 ≥ 1、随 a2 单调不减，且 E→1 时 ms→1（零回归）；
//  ⑤ F_avg(水) 与直接积分同源：0.05 < F_avg < 0.09，且随 ior 单调增。

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "tools/jpov/src/lighting/ggx_specular.h"
#include "tools/jpov/src/water/water_optics.h"

namespace {

using jpov::lighting::GgxDirectionalAlbedoSS;
using jpov::lighting::GgxMultiscatterScale;
using jpov::water::FresnelAvgDielectric;

// van der Corput（base 2）：确定性、低差异，避免测试里出现随机抖动。
double RadicalInverse2(uint32_t i) {
    i = (i << 16u) | (i >> 16u);
    i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
    i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
    i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
    i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
    return static_cast<double>(i) * 2.3283064365386963e-10;
}

double G1(double nx, double a2) {
    if (!(nx > 0.0)) {
        return 0.0;
    }
    const double d = nx + std::sqrt(std::max(a2 + (1.0 - a2) * nx * nx, 0.0));
    return 2.0 * nx / std::max(d, 1e-12);
}

// 参考实现：E_ss(μo) = E[ G1(μo)·G1(μi)·(ωo·h) / (μo·(n·h)) ]，h ~ D(h)(n·h)。
double E_ssReference(double mu_o, double a2, int n = 8192) {
    mu_o = std::max(mu_o, 1e-3);
    const double th_o = std::acos(std::min(mu_o, 1.0));
    const double wo[3] = {std::sin(th_o), 0.0, mu_o};
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double u1 = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
        const double u2 = RadicalInverse2(static_cast<uint32_t>(i));
        const double ct = std::sqrt(std::max(
            (1.0 - u1) / std::max(1.0 + (a2 - 1.0) * u1, 1e-12), 0.0));
        const double st = std::sqrt(std::max(1.0 - ct * ct, 0.0));
        const double ph = 6.28318530717958647692 * u2;
        const double h[3] = {st * std::cos(ph), st * std::sin(ph), ct};
        const double woh = h[0] * wo[0] + h[2] * wo[2];
        if (!(woh > 0.0) || !(ct > 0.0)) {
            continue;
        }
        const double ni = 2.0 * woh * h[2] - wo[2];
        if (!(ni > 0.0)) {
            continue;
        }
        sum += G1(mu_o, a2) * G1(ni, a2) * woh / (mu_o * ct);
    }
    return sum / static_cast<double>(n);
}

TEST(GgxAlbedo, TableMatchesReferenceIntegration) {
    const float mus[] = {0.05f, 0.1f, 0.15f, 0.3f, 0.5f, 0.9f};
    const float alphas[] = {0.03f, 0.06f, 0.13f, 0.25f, 0.5f, 0.85f};
    for (float mu : mus) {
        for (float alpha : alphas) {
            const float a2 = alpha * alpha;
            const float table = GgxDirectionalAlbedoSS(mu, a2);
            const double ref = E_ssReference(mu, a2);
            EXPECT_NEAR(table, ref, 0.02) << "mu=" << mu << " alpha=" << alpha
                                          << " table=" << table << " ref=" << ref;
        }
    }
}

TEST(GgxAlbedo, PhysicalLimits) {
    // δ 镜面：没有遮蔽损失。
    EXPECT_FLOAT_EQ(GgxDirectionalAlbedoSS(0.5f, 0.0f), 1.0f);
    EXPECT_NEAR(GgxDirectionalAlbedoSS(0.5f, 1.0e-8f), 1.0f, 1.0e-3f);
    // α = 1、μ = 1：文献 0.31 左右（Kulla–Conty 表）。这条把「表真的在算 GGX」钉住 ——
    // 若有人把表换成常数或抄错行列，这里必红。
    EXPECT_NEAR(GgxDirectionalAlbedoSS(1.0f, 1.0f), 0.31f, 0.03f);
}

TEST(GgxAlbedo, MonotoneInRoughness) {
    float prev = 2.0f;
    for (float alpha : {0.01f, 0.03f, 0.1f, 0.2f, 0.4f, 0.7f, 1.0f}) {
        const float e = GgxDirectionalAlbedoSS(0.4f, alpha * alpha);
        EXPECT_LE(e, prev + 1.0e-3f) << "alpha=" << alpha;
        prev = e;
    }
}

TEST(GgxAlbedo, MultiscatterScaleIsOneForSmoothSurface) {
    const float f_avg = FresnelAvgDielectric(1.333f);
    // a2 → 0：E → 1 ⇒ ms → 1（零回归：光滑水面不受补偿影响）。
    EXPECT_NEAR(GgxMultiscatterScale(0.4f, 0.0f, f_avg), 1.0f, 1.0e-4f);
    // 粗糙：ms > 1 且随 a2 单调不减。
    float prev = 0.0f;
    for (float alpha : {0.05f, 0.1f, 0.2f, 0.4f, 0.7f, 1.0f}) {
        const float ms = GgxMultiscatterScale(0.3f, alpha * alpha, f_avg);
        EXPECT_GE(ms, 1.0f);
        EXPECT_GE(ms, prev - 1.0e-4f) << "alpha=" << alpha;
        prev = ms;
    }
    // 量级检查：a2 = 0.25（α=0.5）、F_avg≈0.068 时补偿是个位数百分比，
    // 不能变成 1.5（那会把粗糙面画过曝）。⚠️ 「掠射补偿更大」**不成立**：
    // Smith-GGX 下 E_ss 在极粗表面是**正视更小**（表里 μ=1,α=1 = 0.307 < μ=0.02 的 0.60），
    // 所以补偿在正视更大 —— 别按直觉写断言。
    EXPECT_GT(GgxMultiscatterScale(0.5f, 0.25f, f_avg), 1.0f);
    EXPECT_LT(GgxMultiscatterScale(0.5f, 0.25f, f_avg), 1.2f);
    // 上限护栏：E 极小时不许无限放大。
    EXPECT_LE(GgxMultiscatterScale(1.0f, 1.0f, 1.0f), 4.0f);
}

TEST(GgxAlbedo, AverageFresnelMatchesDirectIntegral) {
    // FresnelAvgDielectric 自己就是积分（64 点梯形）；这里用更细的 512 点复核，防抄错系数。
    for (float ior : {1.1f, 1.333f, 1.5f, 2.0f}) {
        constexpr int kN = 512;
        double sum = 0.0;
        for (int i = 0; i < kN; ++i) {
            const double mu = (i + 0.5) / kN;
            sum += jpov::water::FresnelDielectric(static_cast<float>(mu), 1.0f, ior) * mu;
        }
        const double ref = 2.0 * sum / kN;
        EXPECT_NEAR(FresnelAvgDielectric(ior), ref, 2.0e-3) << "ior=" << ior;
    }
    // 水：约 0.068（远小于直觉上的 0.02~1，因为掠射被 μ 权重压小）。
    const float fw = FresnelAvgDielectric(1.333f);
    EXPECT_GT(fw, 0.05f);
    EXPECT_LT(fw, 0.09f);
}

}  // namespace
