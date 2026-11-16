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

#include "tools/jpov/interface/camera.h"   // Vec3f（header-only，无 GL）

namespace jpov {
namespace water {

// 水的折射率（20°C 纯水）。全工程唯一一处数值。
inline constexpr float kWaterIorDefault = 1.333f;

// 界面（水平面）对**环境光辐照度**的平均透射率。
//
// 漫射天空从整个上半球入射，菲涅尔平均反射率 ≈ 6%（水 n=1.333）⇒ 透射 ≈ 0.94。
// ⚠️ 这是**常数近似**（不是对天空辐亮度分布做积分）：水下 ambient 取
//    `SkyCommand::AmbientColor()/AmbientIntensity()` 再乘本值。
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

// Henyey-Greenstein 相位函数（mu = 1 为前向散射）。
// Pre-condition: g ∈ [0, 0.95]（超出会被夹住，不 crash）。
inline float HgPhase(float mu, float g) {
    const float gc = std::clamp(g, 0.0f, 0.95f);
    const float denom = std::max(1.0f + gc * gc - 2.0f * gc * mu, 1.0e-4f);
    return (1.0f - gc * gc) / (4.0f * 3.14159265f * denom * std::sqrt(denom));
}

// 折射：空气侧「指向太阳」的单位方向 → 水内「指向太阳」的单位方向（法线 = +Y）。
// 返回 false = 太阳在地平线以下/与水面相切（水下没有直射项）。
// Pre-condition: ior > 0；air_toward_sun 为单位向量。
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
    const float cos_w = std::sqrt(std::max(0.0f, 1.0f - sin2_w));
    // 水平分量缩放 = sinθw / sinθi（θi→0 时极限 = 1/ior，用 sin² 比值避免 0/0）。
    const float k = (sin2_i > 1.0e-12f) ? std::sqrt(sin2_w / sin2_i) : (1.0f / ior);
    *out = {air_toward_sun.x() * k, cos_w, air_toward_sun.z() * k};
    return true;
}

// ∫₀^len exp(−sigma·rate·t) dt = (1 − e^{−sigma·rate·len}) / (sigma·rate)。
// rate 或 sigma → 0 时极限 = len（用级数展开，避免 0/0 与精度损失）。
inline float DecayLength(float sigma, float rate, float len) {
    const float x = sigma * rate * len;
    if (std::fabs(x) < 1.0e-4f) {
        return len * (1.0f - 0.5f * x);
    }
    return (1.0f - std::exp(-x)) / (sigma * rate);
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_OPTICS_H_
