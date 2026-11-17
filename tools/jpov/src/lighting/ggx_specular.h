// GGX 镜面高光的共享实现：CPU 参考（GL-free）+ GLSL 片段。
//
// 为什么单独一个头：水皮要画「太阳的像」，而那是**同一个 GGX**，不是第二套高光。
// 如果水皮自己再写一份 D/G，改一处就会漂 —— 这里放唯一出处：
//   · CPU 版本给单测与 GPU 像素对拍当参考（test/water:water_skin_*_test）；
//   · kGgxSpecularGlsl 拼进水皮片元（将来 PBR 也改用它，见文末「待统一」）。
//
// 口径（与 object3d_renderer.h 的 distributionGGX 一致，勿改）：
//   a2 = α²，其中 α = roughness²（JPOV/Filament 口径）⇒ a2 = roughness⁴。
//
// ⚠️ 已知的两个「δ 光源 × δ BRDF」问题与解法见 known_issue §11：
//   · 完美镜面 + 平行光 = 两个 δ 相乘 ⇒ 点值无穷、只能积分；
//   · 给光源一个**角半径** θ，半向量只偏 θ/2 ⇒ `a2_eff = a2 + (θ/2)²` 就有带宽；
//   · 掠射时投影会把带宽压回亚像素 ⇒ 再并进「像素足迹」的斜率方差（specular AA）。
//
// 待统一（下一步，会改 gold）：object3d 的三份 PBR 片元目前仍各自拷贝着
//   distributionGGX / geometrySchlickGGX。等给它也接上 angular_radius 时，
//   一并换成这里的函数，届时 `distributionGGX` 的旧实现删除。

#ifndef JPOV_SRC_LIGHTING_GGX_SPECULAR_H_
#define JPOV_SRC_LIGHTING_GGX_SPECULAR_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/interface/camera.h"   // Vec3f

namespace jpov {
namespace lighting {

// 天体角半径（弧度）。太阳 0.2665°、月亮 0.259°；年周期日地距离只变 ±1.7%。
// 这是**光源的几何属性**，与仰角无关（别用仰角插值，见 known_issue §11.6①）。
inline constexpr float kSunAngularRadiusRad = 0.004654f;    // 0.2667°
inline constexpr float kMoonAngularRadiusRad = 0.004521f;   // 0.2591°

// GGX 法线分布 D（a2 = α²）。NoH = max(dot(N,H), 0)。
//
// ⚠️ 护栏取 1e-30，**不能**照抄 object3d 的 `max(denom, 1e-7)`：
//   denom 在 NoH→1 时 ≈ π·a2²，a2 < 1.8e-4（roughness < 0.0206）时 π·a2² 就小于 1e-7，
//   于是峰值被人为钳到 a2/1e-7 —— 越光滑反而越暗，且能量不守恒。
//   水皮的日盘带宽 a2 = (θ/2)² = 5.4e-6 正好落在这个陷阱里（实测峰值被压低 1086 倍）。
//   PBR 那份的实现仍是 1e-7（现有材质 r≥0.02，a2=4e-4 刚好没踩到），统一时一并修。
inline float GgxDistribution(float no_h, float a2) {
    const float no_h2 = no_h * no_h;
    const float t = no_h2 * (a2 - 1.0f) + 1.0f;
    return a2 / std::max(3.14159265358979323846f * t * t, 1.0e-30f);
}

// Smith-GGX 单侧遮蔽项 G1（NoX = 法线与该方向的余弦）。
// Pre-condition: a2 > 0（a2 = 0 时返回 0 —— 与「δ 无带宽」的语义一致）。
inline float GgxSmithG1(float no_x, float a2) {
    if (!(a2 > 0.0f) || !(no_x > 0.0f)) {
        return 0.0f;
    }
    const float denom = no_x + std::sqrt(a2 + (1.0f - a2) * no_x * no_x);
    if (!(denom > 1.0e-6f)) {
        return 0.0f;
    }
    return (2.0f * no_x) / denom;
}

// 盘状光源并入 a2 的带宽：半向量只偏 **θ/2**（角平分线）⇒ 平方后是 (θ/2)²。
// ⚠️ 写成 θ² 会过宽 4 倍（known_issue §11.6②）。
inline float GgxDiskBandwidthA2(float angular_radius) {
    const float h = 0.5f * std::max(angular_radius, 0.0f);
    return h * h;
}

// 像素足迹并入 a2：把半向量在屏幕空间的导数当作附加斜率方差。
// 用法（GLSL）：`0.25 * (dot(dFdx(H),dFdx(H)) + dot(dFdy(H),dFdy(H)))`，
// 系数 0.25 沿用 Vlachos/Filament 的 specular AA 口径（工程近似，非严格推导）。
// 这里给 CPU 版是为了让测试能显式喂一个已知足迹。
inline float GgxFootprintA2(float dhdx_x, float dhdx_y, float dhdx_z,
                            float dhdy_x, float dhdy_y, float dhdy_z) {
    const float a = dhdx_x * dhdx_x + dhdx_y * dhdx_y + dhdx_z * dhdx_z;
    const float b = dhdy_x * dhdy_x + dhdy_y * dhdy_y + dhdy_z * dhdy_z;
    return 0.25f * (a + b);
}

// 平行光（视作角半径 θ 的盘）在**平面/光滑表面**上的镜面出射辐射度。
//
//   normal / toward_cam / toward_sun  单位向量（世界系，y-up）
//   fresnel                          界面反射率（调用方按自己的折射率算，含 TIR）
//   angular_radius                   盘角半径（弧度，> 0）
//   extra_a2                         额外斜率方差（像素足迹），>= 0
//   sun_irradiance                   太阳辐照度（color × intensity）
//
// 返回：P 点沿 toward_cam 的出射辐射度（HDR，未做 tone map）。
// Pre-condition: 三个方向都是单位向量；angular_radius > 0。
inline Vec3f SunDiskSpecularRadiance(const Vec3f& normal, const Vec3f& toward_cam,
                                     const Vec3f& toward_sun, float fresnel,
                                     float angular_radius, float extra_a2,
                                     const Vec3f& sun_irradiance) {
    const float no_v = normal.x() * toward_cam.x() +
                       normal.y() * toward_cam.y() +
                       normal.z() * toward_cam.z();
    const float no_l = normal.x() * toward_sun.x() +
                       normal.y() * toward_sun.y() +
                       normal.z() * toward_sun.z();
    if (!(no_v > 0.0f) || !(no_l > 0.0f) || !(angular_radius > 0.0f)) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }

    // 半向量 H = normalize(L + V)
    const float hx = toward_sun.x() + toward_cam.x();
    const float hy = toward_sun.y() + toward_cam.y();
    const float hz = toward_sun.z() + toward_cam.z();
    const float hlen = std::sqrt(hx * hx + hy * hy + hz * hz);
    if (!(hlen > 1.0e-8f)) {
        return Vec3f{0.0f, 0.0f, 0.0f};
    }
    const float no_h = std::max(0.0f, (normal.x() * hx + normal.y() * hy +
                                       normal.z() * hz) / hlen);

    const float a2 = std::min(GgxDiskBandwidthA2(angular_radius) +
                                  std::max(extra_a2, 0.0f),
                              0.25f);
    const float d = GgxDistribution(no_h, a2);
    const float g = GgxSmithG1(no_v, a2) * GgxSmithG1(no_l, a2);
    // f_r = F·D·G/(4·NoV·NoL)；出射 = f_r·E·NoL ⇒ 消掉一个 NoL
    const float weight = (fresnel * d * g) / std::max(4.0f * no_v * no_l, 1.0e-6f) * no_l;
    return Vec3f{sun_irradiance.x() * weight,
                 sun_irradiance.y() * weight,
                 sun_irradiance.z() * weight};
}

// 盘状光源在屏幕上的角直径（弧度）—— 供测试与文档引用。
inline float GgxDiskAngularDiameter(float angular_radius) {
    return 2.0f * angular_radius;
}

}  // namespace lighting
}  // namespace jpov

// ───────────────────────────── GLSL 片段 ─────────────────────────────
//
// 与上面四个 CPU 函数**同源**（改一处必须改另一处），由
// test/water:water_skin_gpu_test 的逐像素对拍兜底。
// 使用方要在自己片元里先拼这段，再拼自己的 main()。
namespace jpov {
namespace lighting {

inline constexpr const char* kGgxSpecularGlsl = R"glsl(
// a2 = α²（α = roughness²）。与 ggx_specular.h 的 CPU 版逐项相同。
// ⚠️ 护栏 1e-30：denom 在 NoH→1 时 ≈ π·a2²，小 a2（光滑面/日盘带宽）会被 1e-7 钳住峰值。
float GgxDistribution(float NoH, float a2) {
    float NoH2 = NoH * NoH;
    float d = NoH2 * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-30);
}

float GgxSmithG1(float NoX, float a2) {
    if (!(a2 > 0.0) || !(NoX > 0.0)) {
        return 0.0;
    }
    float denom = NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX);
    return 2.0 * NoX / max(denom, 1e-6);
}

// 盘状光源的带宽：半向量只偏 θ/2（角平分线）⇒ (θ/2)²。
// ⚠️ 写 θ² 会过宽 4 倍（known_issue §11.6②）。
float GgxDiskBandwidthA2(float angularRadius) {
    float h = 0.5 * max(angularRadius, 0.0);
    return h * h;
}
)glsl";

}  // namespace lighting
}  // namespace jpov

#endif  // JPOV_SRC_LIGHTING_GGX_SPECULAR_H_
