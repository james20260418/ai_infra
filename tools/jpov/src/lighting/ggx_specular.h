// GGX 镜面高光的共享实现：CPU 参考（GL-free）+ GLSL 片段。
//
// 为什么单独一个头：水皮要画「太阳的像」，而那是**同一个 GGX**，不是第二套高光。
// 如果水皮自己再写一份 D/G，改一处就会漂 —— 这里放唯一出处：
//   · CPU 版本给单测与 GPU 像素对拍当参考（test/water:water_skin_*_test）；
//   · kGgxSpecularGlsl 拼进水皮片元。
//
// 口径（与 object3d_renderer.h 的 distributionGGX 一致，勿改）：
//   a2 = α²，其中 α = roughness²（JPOV/Filament 口径）⇒ a2 = roughness⁴。
//
// ⚠️ 已知的两个「δ 光源 × δ BRDF」问题与解法见 docs/jpov_specular_bandwidth.md：
//   · 完美镜面 + 平行光 = 两个 δ 相乘 ⇒ 点值无穷、只能积分；
//   · 给光源一个**角半径** θ，半向量只偏 θ/2 ⇒ `a2_eff = a2 + (θ/2)²` 就有带宽；
//   · 掠射时投影会把带宽压回亚像素 ⇒ 再并进「像素足迹」的斜率方差（specular AA）。
//
// 已落地（2026-11-18）：object3d / instanced / skeleton 三份 PBR 片元都已接上
//   `uSunAngularRadius`（太阳路径用 `a2_eff = roughness⁴ + (θ/2)²`，护栏 1e-30），
//   验收见 `--capture-mirror-specular` 与 test/object3d:jpov_specular_bandwidth_gold_*。
//   三处的 `distributionGGX` 仍各自拷贝（GLSL 字符串、共享片段只拼给水皮），
//   真正做到“一份实现”要把它们也改成拼 kGgxSpecularGlsl —— 仍未做，改一处记得改三处。

#ifndef JPOV_SRC_LIGHTING_GGX_SPECULAR_H_
#define JPOV_SRC_LIGHTING_GGX_SPECULAR_H_

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "tools/jpov/interface/camera.h"   // Vec3f

namespace jpov {
namespace lighting {

// 天体角半径（弧度）。太阳 0.2665°、月亮 0.259°；年周期日地距离只变 ±1.7%。
// 这是**光源的几何属性**，与仰角无关（别用仰角插值，见 docs/jpov_specular_bandwidth.md §1.6①）。
inline constexpr float kSunAngularRadiusRad = 0.004654f;    // 0.2667°
inline constexpr float kMoonAngularRadiusRad = 0.004521f;   // 0.2591°

// GGX 法线分布 D（a2 = α²）。NoH = max(dot(N,H), 0)。
//
// ⚠️ 护栏取 1e-30，**不能**用 1e-7：denom 在 NoH→1 时 ≈ π·a2²，a2 < 1.8e-4
//   （roughness < 0.115）时 π·a2² 就小于 1e-7，于是峰值被人为钳到 a2/1e-7 ——
//   越光滑反而越暗，且能量不守恒。水皮的日盘带宽 a2 = (θ/2)² = 5.4e-6 正好落在这个
//   陷阱里（实测峰值被压低 1086 倍）；三份 PBR 片元已于 2026-11-18 同步改成 1e-30。
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
// ⚠️ 写成 θ² 会过宽 4 倍（docs/jpov_specular_bandwidth.md §1.6②）。
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

// ── 单次散射方向反照率 E_ss(μ, α)（GGX + Smith，F = 1）──
//
// 定义：E_ss(μo) = ∫ f_ss(ωo,ωi)·(ωi·n) dωi，f_ss = D·G/(4 μo μi)，F ≡ 1。
// 物理含义：**这个 lobe 到底把多少入射能量反射出去了**。粗糙面上 Smith 遮蔽吃掉的那部分
// (1 − E_ss) 在单次散射模型里永远回不来 ⇒ 远场粗糙水面「变暗」。
//
// 用途：多散射补偿（Turquin 2019 / Kulla–Conty）：
//   ms = 1 + F_avg·(1 − E_ss)/E_ss ，乘到镜面项上（与反射率 F 同一处，见 water_skin_shader.h）。
//
// 表：16（μ，log 0.02→1）× 16（α，log 0.01→1）。按 NDF 重要性采样离线算，
//   3 组 seed × 20 万样本平均（抖动量级 < 0.3%）。α=1、μ=1 时 0.307，与文献
//   （Kulla–Conty：α=1 的 E≈0.31）一致。
// ⚠️ GLSL 端的同一张表由 `GgxAlbedoGlsl()` **从这里格式化生成**（不是手抄）⇒ 不可能漂。
inline constexpr int kGgxAlbedoN = 16;
inline constexpr float kGgxAlbedoMuMin = 0.02f;
inline constexpr float kGgxAlbedoMuRatio = 50.0f;    // μmax/μmin
inline constexpr float kGgxAlbedoAlphaMin = 0.01f;
inline constexpr float kGgxAlbedoAlphaRatio = 100.0f;  // αmax/αmin
inline constexpr float kGgxAlbedoTable[kGgxAlbedoN * kGgxAlbedoN] = {
    
    0.90966f, 0.88970f, 0.88001f, 0.88013f, 0.88627f, 0.89420f, 0.90118f, 0.90566f, 0.90677f, 0.90346f, 0.89394f, 0.87497f, 0.84172f, 0.78813f, 0.70852f, 0.60133f,
    0.93106f, 0.90612f, 0.88747f, 0.87929f, 0.88036f, 0.88670f, 0.89400f, 0.89956f, 0.90194f, 0.89975f, 0.89092f, 0.87233f, 0.83922f, 0.78545f, 0.70550f, 0.59788f,
    0.95191f, 0.92716f, 0.90267f, 0.88531f, 0.87852f, 0.88037f, 0.88630f, 0.89242f, 0.89606f, 0.89502f, 0.88700f, 0.86897f, 0.83600f, 0.78203f, 0.70164f, 0.59346f,
    0.96889f, 0.94839f, 0.92318f, 0.89919f, 0.88325f, 0.87751f, 0.87948f, 0.88476f, 0.88912f, 0.88905f, 0.88207f, 0.86468f, 0.83188f, 0.77771f, 0.69671f, 0.58781f,
    0.98082f, 0.96620f, 0.94463f, 0.91910f, 0.89578f, 0.88077f, 0.87572f, 0.87783f, 0.88132f, 0.88196f, 0.87598f, 0.85922f, 0.82673f, 0.77222f, 0.69044f, 0.58060f,
    0.98854f, 0.97897f, 0.96326f, 0.94078f, 0.91478f, 0.89172f, 0.87802f, 0.87333f, 0.87384f, 0.87408f, 0.86853f, 0.85256f, 0.82025f, 0.76529f, 0.68250f, 0.57148f,
    0.99325f, 0.98735f, 0.97702f, 0.96007f, 0.93632f, 0.91011f, 0.88779f, 0.87408f, 0.86867f, 0.86604f, 0.86018f, 0.84454f, 0.81226f, 0.75661f, 0.67251f, 0.56005f,
    0.99607f, 0.99255f, 0.98603f, 0.97468f, 0.95643f, 0.93184f, 0.90485f, 0.88247f, 0.86809f, 0.85982f, 0.85168f, 0.83532f, 0.80254f, 0.74585f, 0.66013f, 0.54586f,
    0.99770f, 0.99563f, 0.99171f, 0.98443f, 0.97214f, 0.95244f, 0.92633f, 0.89846f, 0.87470f, 0.85809f, 0.84446f, 0.82543f, 0.79111f, 0.73285f, 0.64501f, 0.52853f,
    0.99864f, 0.99743f, 0.99502f, 0.99072f, 0.98272f, 0.96896f, 0.94765f, 0.91940f, 0.88956f, 0.86323f, 0.84098f, 0.81615f, 0.77850f, 0.71765f, 0.62692f, 0.50754f,
    0.99914f, 0.99841f, 0.99707f, 0.99443f, 0.98946f, 0.98055f, 0.96510f, 0.94124f, 0.91025f, 0.87661f, 0.84387f, 0.80988f, 0.76598f, 0.70056f, 0.60561f, 0.48277f,
    0.99942f, 0.99901f, 0.99824f, 0.99663f, 0.99361f, 0.98792f, 0.97758f, 0.95987f, 0.93264f, 0.89666f, 0.85498f, 0.80968f, 0.75560f, 0.68242f, 0.58142f, 0.45407f,
    0.99962f, 0.99936f, 0.99884f, 0.99792f, 0.99605f, 0.99242f, 0.98579f, 0.97353f, 0.95252f, 0.91929f, 0.87394f, 0.81783f, 0.75033f, 0.66514f, 0.55485f, 0.42168f,
    0.99975f, 0.99955f, 0.99922f, 0.99862f, 0.99744f, 0.99518f, 0.99084f, 0.98265f, 0.96733f, 0.94039f, 0.89704f, 0.83487f, 0.75328f, 0.65133f, 0.52718f, 0.38591f,
    0.99981f, 0.99970f, 0.99945f, 0.99900f, 0.99828f, 0.99681f, 0.99394f, 0.98825f, 0.97736f, 0.95683f, 0.91981f, 0.85813f, 0.76623f, 0.64464f, 0.49997f, 0.34740f,
    0.99989f, 0.99980f, 0.99962f, 0.99932f, 0.99880f, 0.99773f, 0.99568f, 0.99170f, 0.98379f, 0.96831f, 0.93848f, 0.88318f, 0.78866f, 0.64794f, 0.47522f, 0.30696f};

// 节点坐标 → [0, N−1] 的连续下标（log 间距；越界钳到端点）。
inline float GgxAlbedoCoordLog(float v, float vmin, float vmax_ratio) {
    if (!(v > 0.0f)) {
        return 0.0f;
    }
    const float t = std::log(v / vmin) / std::log(vmax_ratio);   // [0,1]
    return std::clamp(t, 0.0f, 1.0f) * static_cast<float>(kGgxAlbedoN - 1);
}

// E_ss 查表（双线性）。Pre-condition: no_v > 0；a2 ≥ 0（a2 = 0 ⇒ 完美镜面 ⇒ 返回 1）。
inline float GgxDirectionalAlbedoSS(float no_v, float a2) {
    if (!(a2 > 0.0f)) {
        return 1.0f;   // δ 镜面：没有遮蔽损失
    }
    const float alpha = std::sqrt(a2);
    const float fx = GgxAlbedoCoordLog(std::max(no_v, kGgxAlbedoMuMin), kGgxAlbedoMuMin,
                                       kGgxAlbedoMuRatio);
    const float fy = GgxAlbedoCoordLog(std::max(alpha, kGgxAlbedoAlphaMin),
                                       kGgxAlbedoAlphaMin, kGgxAlbedoAlphaRatio);
    const int x0 = std::min(static_cast<int>(fx), kGgxAlbedoN - 2);
    const int y0 = std::min(static_cast<int>(fy), kGgxAlbedoN - 2);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto at = [&](int x, int y) {
        return kGgxAlbedoTable[x * kGgxAlbedoN + y];
    };
    const float e00 = at(x0, y0);
    const float e01 = at(x0, y0 + 1);
    const float e10 = at(x0 + 1, y0);
    const float e11 = at(x0 + 1, y0 + 1);
    const float e0 = e00 + (e01 - e00) * ty;
    const float e1 = e10 + (e11 - e10) * ty;
    return std::clamp(e0 + (e1 - e0) * tx, 0.0f, 1.0f);
}

// 多散射补偿系数：`f_total = f_ss · ms`（Kulla–Conty / Turquin 的常用简化）。
//   f_avg = 界面的半球平均反射率（见 water_optics.h::FresnelAvgDielectric）。
// 护栏：E_ss 极小时（alpha→1、grazing）比值可以很大，钳到 4 以内 —— 这不是物理上限，
// 是**单次散射模型已经失效**的区域不该被补偿公式放大成过曝。
inline float GgxMultiscatterScale(float no_v, float a2, float f_avg) {
    const float e = std::max(GgxDirectionalAlbedoSS(no_v, a2), 1.0e-2f);
    const float ms = 1.0f + std::max(f_avg, 0.0f) * (1.0f - e) / e;
    return std::min(ms, 4.0f);
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
// ⚠️ 写 θ² 会过宽 4 倍（docs/jpov_specular_bandwidth.md §1.6②）。
float GgxDiskBandwidthA2(float angularRadius) {
    float h = 0.5 * max(angularRadius, 0.0);
    return h * h;
}
)glsl";

// E_ss 表 + 查表（多散射补偿 M2）。表内容由 C++ 的 kGgxAlbedoTable **格式化生成**，
// 保证 CPU/GPU 是同一张表（改表只改上面那一处，这里不手抄）。
inline std::string GgxAlbedoGlsl() {
    std::string s = R"glsl(
const int kAlbedoN = 16;
const float kAlbedoMuMin = 0.02;
const float kAlbedoMuRatio = 50.0;
const float kAlbedoAlphaMin = 0.01;
const float kAlbedoAlphaRatio = 100.0;
const float kAlbedoTable[256] = float[256](
    GLSL_TABLE
);
float GgxAlbedoCoordLog(float v, float vmin, float vmaxRatio) {
    if (!(v > 0.0)) {
        return 0.0;
    }
    float t = log(v / vmin) / log(vmaxRatio);
    return clamp(t, 0.0, 1.0) * float(kAlbedoN - 1);
}
// 单次散射方向反照率 E_ss(NoV, a2)：与 CPU 的 GgxDirectionalAlbedoSS 同口径（双线性查表）。
float GgxAlbedoSS(float NoV, float a2) {
    if (!(a2 > 0.0)) {
        return 1.0;
    }
    float alpha = sqrt(a2);
    float fx = GgxAlbedoCoordLog(max(NoV, kAlbedoMuMin), kAlbedoMuMin, kAlbedoMuRatio);
    float fy = GgxAlbedoCoordLog(max(alpha, kAlbedoAlphaMin), kAlbedoAlphaMin,
                                 kAlbedoAlphaRatio);
    int x0 = min(int(fx), kAlbedoN - 2);
    int y0 = min(int(fy), kAlbedoN - 2);
    float tx = fx - float(x0);
    float ty = fy - float(y0);
    float e00 = kAlbedoTable[x0 * kAlbedoN + y0];
    float e01 = kAlbedoTable[x0 * kAlbedoN + y0 + 1];
    float e10 = kAlbedoTable[(x0 + 1) * kAlbedoN + y0];
    float e11 = kAlbedoTable[(x0 + 1) * kAlbedoN + y0 + 1];
    float e0 = e00 + (e01 - e00) * ty;
    float e1 = e10 + (e11 - e10) * ty;
    return clamp(e0 + (e1 - e0) * tx, 0.0, 1.0);
}
// 多散射补偿：f_total = f_ss · ms（Kulla–Conty / Turquin 简化式），上限 4。
float GgxMultiscatterScale(float NoV, float a2, float fAvg) {
    float e = max(GgxAlbedoSS(NoV, a2), 1e-2);
    float ms = 1.0 + max(fAvg, 0.0) * (1.0 - e) / e;
    return min(ms, 4.0);
}
)glsl";
    // 表字面量从 kGgxAlbedoTable 现场格式化 —— 单一出处，CPU/GPU 不可能漂。
    std::string table;
    table.reserve(16 * 16 * 10);
    char buf[32];
    for (int i = 0; i < kGgxAlbedoN * kGgxAlbedoN; ++i) {
        std::snprintf(buf, sizeof(buf), "%.5ff", kGgxAlbedoTable[i]);
        if (i != 0) {
            table += (i % kGgxAlbedoN == 0) ? ",\n    " : ", ";
        }
        table += buf;
    }
    const std::string token = "GLSL_TABLE";
    const size_t pos = s.find(token);
    if (pos != std::string::npos) {
        s.replace(pos, token.size(), table);
    }
    return s;
}

}  // namespace lighting
}  // namespace jpov

#endif  // JPOV_SRC_LIGHTING_GGX_SPECULAR_H_
