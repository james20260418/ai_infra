// JPOV 水体光学——**GL-free 的物理原语唯一出处**（CPU 侧）
//
// 定位：所有和水有关的公式（菲涅尔、临界角、折射方向、相位、指数段积分、界面
// 透射率常量）都只在这里写一份。消费者：
//   · src/water/water_medium_math.h   —— 水体介质的 CPU 参考（与 GPU 对拍用）
//   · （后续）水皮 / 水下光照衰减     —— 同一套原语，不再各自复制
//
// ⚠️ 与 GLSL 的关系：GLSL 侧不能 include C++ 头，所以 `water_medium_shader.h` 里
// 仍有一份等价实现（`Wm*` 前缀）。**两者由 GPU 对拍测试兜底**（同一个像素用 CPU
// 参考算一遍、GPU 算一遍，逐位比较）—— 这是唯一的防漂移机制，加公式必须同时加对拍。
//
// 口径（见 docs/jpov_water_contract.md §5）：过界面时
//   · 辐照度 E      × (1−R)
//   · 辐射度 L      × n²      （水→空气则 ÷n²）
//   · 法向辐照度 E⊥ × (1−R)·cosθa/cosθw
// 三个 cos 不要串：`cosθa` / `cosθw` 是**视线**的，`toward.y` 是**太阳**在水内的。

#ifndef JPOV_SRC_WATER_WATER_OPTICS_H_
#define JPOV_SRC_WATER_WATER_OPTICS_H_

#include <algorithm>
#include <cmath>
#include <limits>

#include "tools/jpov/interface/camera.h"   // Vec3f（header-only，无 GL）

namespace jpov {
namespace water {


// 水的折射率（20°C 纯水）。全工程唯一一处数值。
inline constexpr float kWaterIorDefault = 1.333f;

// HG 各向异性的上限（g→1 时相位函数变成 δ，离散采样会爆）。
// CPU 的 clamp / Renderer 的 CHECK / GLSL 的 clamp 必须**同一个数**，由
// test/water:water_medium_math_test 的 GlslConstantsMatchCpu 守住。
inline constexpr float kMaxPhaseG = 0.95f;

// 界面（水平面）对**环境光辐照度**的平均透射率。
//
// 漫射天空从整个上半球入射，菲涅尔平均反射率 ≈ 6%（水 n=1.333）⇒ 透射 ≈ 0.94。
// ⚠️ 这是**常数近似**（不是对天空辐亮度分布做积分）：水下 ambient 取
//    `RenderCommandList::ambient`（三色时先经 AmbientLight::IsotropicColor() 塔成纯色）
//    再乘本值。不从 SkyCommand 推导 —— 没配 ambient 就是没有环境注入。
inline constexpr float kAmbientInterfaceTransmittance = 0.94f;

// 水→空气的临界角余弦：cosθc = sqrt(1 − 1/ior²)。ior=1.333 ⇒ θc ≈ 48.6°（从法线量）。
// Pre-condition: ior > 0（ior <= 1 时不存在全内反射，返回 1 = 只有正入射能透射）。
inline float CriticalCos(float ior) {
    if (ior <= 1.0f) {
        return 1.0f;
    }
    return std::sqrt(1.0f - 1.0f / (ior * ior));
}

// 无偏振电介质菲涅尔反射率（含全内反射）。
//   n1/n2 = 入射侧/出射侧折射率，cos_i = 入射角余弦（>= 0，取绝对值）。
// 返回 1.0 = 全内反射。水上（1→ior）与水下（ior→1）都调这一个函数。
inline float FresnelDielectric(float cos_i, float n1, float n2) {
    const float eta = n1 / n2;
    const float sin2_t = eta * eta * (1.0f - cos_i * cos_i);
    if (sin2_t >= 1.0f) {
        return 1.0f;   // 全内反射
    }
    const float cos_t = std::sqrt(1.0f - sin2_t);
    const float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    const float rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i);
    return 0.5f * (rs * rs + rp * rp);
}

// 粗糙界面的**反射方向反照率**（角度平均菲涅尔）：
//   R_eff(μo, α) = ∫ f_ss(ωo,ωi)·F(ωo·h)·cosθi dωi
// 掠射时远低于平面菲涅尔（2°、MSS=0.017 时 0.80 → 0.36）；差额进透射（T_eff = 1 − R_eff）。
// 解析近似（对 12×6 Monte-Carlo 表拟合，平均误差 0.026）：
//   mix( F(cosθ + 0.52·√mss^0.72), F(cosθ), smoothstep(0.05, 0.5, cosθ) )
// ⚠️ 与 water_skin_shader.h 的 GLSL 版**同源**，改一处必须改另一处。
inline float RoughFresnelAlbedo(float cos_i, float mss, float ior) {
    const float s = std::sqrt(std::max(mss, 0.0f));
    const float shifted_cos = std::min(std::max(cos_i + 0.52f * std::pow(std::max(s, 1.0e-6f), 0.72f),
                                               1.0e-4f), 0.999f);
    const float shifted = FresnelDielectric(shifted_cos, 1.0f, ior);
    const float plain = FresnelDielectric(std::min(std::max(cos_i, 1.0e-4f), 0.999f), 1.0f, ior);
    const float t = std::min(std::max((cos_i - 0.05f) / 0.45f, 0.0f), 1.0f);
    const float w = t * t * (3.0f - 2.0f * t);
    return shifted + (plain - shifted) * w;
}


// Henyey-Greenstein 相位函数（mu = 1 为前向散射）。
// Pre-condition: g ∈ [0, 0.95]（超出会被夹住，不 crash）。
inline float HgPhase(float mu, float g) {
    const float gc = std::clamp(g, 0.0f, kMaxPhaseG);
    const float denom = std::max(1.0f + gc * gc - 2.0f * gc * mu, 1.0e-4f);
    return (1.0f - gc * gc) / (4.0f * 3.14159265f * denom * std::sqrt(denom));
}

// 折射：空气侧「指向太阳」的单位方向 → 水内「指向太阳」的单位方向（法线 = +Y）。
// 返回 false = 太阳在地平线以下/与水面相切（水下没有直射项）。
// Pre-condition: ior > 0（Renderer 端另 CHECK > 1：水必须比空气密）；air_toward_sun 为单位向量。
inline bool RefractTowardSun(const Vec3f& air_toward_sun, float ior,
                             Vec3f* out /*output*/) {
    if (out == nullptr) {
        return false;
    }
    if (!(ior > 0.0f)) {
        return false;
    }
    const float cy = air_toward_sun.y();
    if (cy <= 1.0e-4f) {
        return false;   // 太阳贴地平线或在下半球：水体里没有直射
    }
    const float sin2_i = std::max(0.0f, 1.0f - cy * cy);
    const float sin2_w = sin2_i / (ior * ior);
    // ior <= 1（比空气还疏）在物理上不存在，但 CHECK_GT(ior, 0) 会放行；
    // 此时 sin²θw 可以 ≥ 1 ⇒ 没有折射解 ⇒ 必须返回 false。
    // 若放过去：cosθw = 0 ⇒ 增益 1/toward.y = inf ⇒ 与 (1−R)=0 相乘得 **NaN**。
    if (!(sin2_w < 1.0f)) {
        return false;
    }
    const float cos_w = std::sqrt(std::max(0.0f, 1.0f - sin2_w));
    // 水平分量缩放 = sinθw / sinθi（θi→0 时极限 = 1/ior，用 sin² 比值避免 0/0）。
    const float k = (sin2_i > 1.0e-12f) ? std::sqrt(sin2_w / sin2_i) : (1.0f / ior);
    *out = {air_toward_sun.x() * k, cos_w, air_toward_sun.z() * k};
    return true;
}

// ∫₀^len exp(−sigma·rate·t) dt = (1 − e^{−sigma·rate·len}) / (sigma·rate)。
// rate 或 sigma → 0 时极限 = len（用级数展开，避免 0/0 与精度损失）。
// 「沿视线走过 len 米后，衰减积分的有效长度」：∫₀^len e^{−σ·rate·t} dt
//   = (1 − e^{−σ·rate·len}) / (σ·rate)，小 x 用泰勒展开保精度。
// 无穷远（len = +inf）：e^{−∞} = 0 ⇒ 极限是 1/(σ·rate)。
//   ⚠️ 必须先挡住 σ·rate ≤ 0：否则 0·∞ = NaN 会把整条公式污染成 NaN。
//   （σ·rate = 0 意味着既不吸收也不散射，调用方那份乘积本来就是 0。）
inline float DecayLength(float sigma, float rate, float len) {
    const float denom = sigma * rate;
    if (!(denom > 0.0f)) {
        // σ = 0：积分退化成 ∫1 dt = len（旧的语义，测试锁着）。
        // len = +inf 时返回 0：调用方的 σs 此时必然也是 0，乘积必须是 0（不能 0·∞）。
        return std::isfinite(len) ? len : 0.0f;
    }
    const float x = denom * len;          // len 可以是 +inf ⇒ x = +inf ⇒ exp(−x) = 0
    if (std::fabs(x) < 1.0e-4f) {
        return len * (1.0f - 0.5f * x);
    }
    return (1.0f - std::exp(-x)) / denom;
}

// 「无穷远」的射线参数：**真无穷**（不是 1e6 那种近似）。
// 介质是解析闭式，所有项在 len → ∞ 时都收敛（指数项 → 0，衰减长度 → 1/σt），
// 所以可以直接用 +inf，不需要挑一个"足够大"的数。调用方要保证 σt > 0 的那些通道
// 不会出现 0·∞（见上面的 guard 和 WaterAt 里的逐通道判据）。
inline constexpr float kWaterInfiniteZ = std::numeric_limits<float>::infinity();

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_OPTICS_H_
