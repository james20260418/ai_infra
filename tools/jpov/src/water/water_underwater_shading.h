// JPOV 水下照明衰减（照明侧，三个 PBR 主 pass + 条带共用）
//
// 定位：水体的物理分两半 —— **视线侧**（相机→物体的吸收+注入，合成趟里的
// `water_medium_*`）与**照明侧**（光从水面进到物体，本文件）。
// 只做视线侧 ⇒ 水下物体按「空气中的太阳」被照明，5 m 纯水红通道直射会高估 ≈9 倍。
//
// 本版（2026-11-26，Danis 定的「能量基本盘」）只做**衰减**：
//   直射（逐通道）：E_w = E_air · (1−R(θa)) · exp(−σt · d / cosθw)
//   环境（逐通道）：L_w = L_air · 0.94 · exp(−K_d · d)，K_d = σa + (1−g)·σs
//   d = level_y − p.y（> 0 才是水下；水上恒等）
//
// **明确不做**（不是漏了，是这一版的范围）：
//   · 不改光的方向（无折射/无图像位移）；
//   · 不采 CSM（水下阴影仍按水面上方几何的近似，见 known_issue §11.1）；
//   · 不做波面透射率/聚焦（那是下一步：反推水面点 + 局部波法线 + caustic）。
//
// 为什么路径长度用 d/cosθw 而不是 d：与介质（water_medium_math.h 的 L0）同一个
// 口径 —— 同一条太阳光线在水里的长度本来就由 Snell 后的水内角决定；用 d 会整体
// 亮 1/cosθw（45° 太阳 1.18×、低太阳差更多），且与水体散射的颜色对不上。
// 但**方向不折**（PBR 仍用空气侧 L）：所以界面增益里不含 cosθa/cosθw 那一项
// （那一项是「光束变窄后的辐照度」，与 N·L 口径配对时才成立；这里 L 没折，不能乘）。
//
// GLSL / C++ 同源：`kUnderwaterAttenGlsl` 与 `UnderwaterGains()` 是同一个式子，
// 改一处必须改另一处；单测锁住 C++，GPU 由出图验收。

#ifndef JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_
#define JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_

#include <algorithm>
#include <cmath>

#include "tools/jpov/interface/camera.h"                  // Vec3f（header-only，无 GL）
#include "tools/jpov/src/water/water_medium_math.h"        // WaterMediumSample
#include "tools/jpov/src/water/water_optics.h"             // 菲涅尔 / 界面透射率常量

namespace jpov {

class ShaderManager;   // tools/jpov/src/shader_manager.h

namespace water {

// 一帧的水下照明衰减参数（Renderer 从 WaterCommand + WaterMedium 解析一次）。
// 所有颜色量是逐通道**增益**（1 = 不衰减），不是颜色本身。
struct UnderwaterAttenuation {
    bool ambient = false;      // 环境光衰减开关
    bool sun = false;          // 平行光衰减开关
    float level_y = 0.0f;      // 水面世界高度（米，y-up）
    float ior = kWaterIorDefault;
    Vec3f sigma_t = {0.0f, 0.0f, 0.0f};       // σt = σa + σs（1/m，逐通道）
    Vec3f kd = {0.0f, 0.0f, 0.0f};            // K_d = σa + (1−g)·σs（1/m）
    Vec3f sun_toward = {0.0f, 1.0f, 0.0f};    // 指向太阳（空气侧，单位）
    float ambient_t = kAmbientInterfaceTransmittance;  // 界面漫射透射率（≈0.94）

    bool enabled() const { return ambient || sun; }
};

// 从一帧的介质样本生成衰减参数。σt / K_d 在这里算一次（每帧一次，不进片元）。
// Pre-condition: m.ior >= 1（Renderer 侧 CHECK_GE(ior,1)：水必须比空气密，
//   否则 sin²θw ≥ 1 ⇒ 没有折射解 ⇒ 1/cosθw = inf 与 (1−R)=0 相乘得 NaN）；σ 各分量 >= 0。
inline UnderwaterAttenuation BuildUnderwaterAttenuation(const WaterMediumSample& m,
                                                        bool attenuate_ambient,
                                                        bool attenuate_sun) {
    UnderwaterAttenuation a;
    a.ambient = attenuate_ambient;
    a.sun = attenuate_sun;
    a.level_y = m.level_y;
    a.ior = m.ior;
    a.sigma_t = {m.sigma_a.x() + m.sigma_s.x(),
                 m.sigma_a.y() + m.sigma_s.y(),
                 m.sigma_a.z() + m.sigma_s.z()};
    const float ks = 1.0f - m.phase_g;   // 折合衰减：σa + (1−g)·σs
    a.kd = {m.sigma_a.x() + ks * m.sigma_s.x(),
            m.sigma_a.y() + ks * m.sigma_s.y(),
            m.sigma_a.z() + ks * m.sigma_s.z()};
    a.sun_toward = m.sun_toward;
    return a;
}

// C++ 参考：世界点 wp（水下才衰减）→ 逐通道 ambient / sun 增益。
// 与下面的 GLSL 完全同式（单测由 water_underwater_shading_test 锁住）。
inline void UnderwaterGains(const UnderwaterAttenuation& a, const Vec3f& wp,
                            Vec3f* ambient_gain /*output*/,
                            Vec3f* sun_gain /*output*/) {
    if (ambient_gain == nullptr || sun_gain == nullptr) {
        return;
    }
    *ambient_gain = {1.0f, 1.0f, 1.0f};
    *sun_gain = {1.0f, 1.0f, 1.0f};
    if (!a.enabled()) {
        return;
    }
    const float d = a.level_y - wp.y();
    if (!(d > 0.0f)) {
        return;   // 水面之上：恒等（NaN 也走这里 ⇒ 不传播）
    }
    if (a.sun) {
        const float cy = a.sun_toward.y();
        if (!(cy > 1.0e-4f)) {
            // 太阳贴地平线或在下半球：水体里没有直射项。
            *sun_gain = {0.0f, 0.0f, 0.0f};
        } else {
            const float sin2_i = std::max(0.0f, 1.0f - cy * cy);
            const float sin2_w = sin2_i / (a.ior * a.ior);
            const float cos_w = std::sqrt(std::max(1.0e-6f, 1.0f - sin2_w));
            const float path = d / cos_w;   // 水内路径（Snell 后）
            const float t = 1.0f - FresnelDielectric(cy, 1.0f, a.ior);
            sun_gain->x() = t * std::exp(-a.sigma_t.x() * path);
            sun_gain->y() = t * std::exp(-a.sigma_t.y() * path);
            sun_gain->z() = t * std::exp(-a.sigma_t.z() * path);
        }
    }
    if (a.ambient) {
        ambient_gain->x() = a.ambient_t * std::exp(-a.kd.x() * d);
        ambient_gain->y() = a.ambient_t * std::exp(-a.kd.y() * d);
        ambient_gain->z() = a.ambient_t * std::exp(-a.kd.z() * d);
    }
}

// 三个 PBR 片元 + 条带天光共用的 GLSL 片段。按「声明 uniform + 定义函数」拼接，
// 调用方在 main 里拿增益即可（水上 / 开关关 = 恒等）。
inline constexpr const char* kUnderwaterAttenGlsl = R"glsl(
uniform int   uWaterAttenOn;        // 0 = 本帧没有水下衰减（恒等，零开销）
uniform int   uWaterAttenAmbient;   // 1 = 衰减环境光
uniform int   uWaterAttenSun;       // 1 = 衰减平行光
uniform float uWaterAttenLevelY;    // 水面世界高度（米，y-up）
uniform float uWaterAttenIor;       // 界面折射率（与水体/水皮同一个值）
uniform vec3  uWaterAttenSigmaT;    // σt = σa+σs（1/m，逐通道）
uniform vec3  uWaterAttenKd;        // K_d = σa+(1−g)·σs（1/m，逐通道）
uniform vec3  uWaterAttenSunToward; // 指向太阳（空气侧，单位）
uniform float uWaterAttenAmbientT;  // 界面漫射透射率（≈0.94）

// 无偏振电介质菲涅尔（与 water_optics.h / water_medium_shader.h 同一实现）。
float WaterAttenFresnel(float cos_i, float n1, float n2) {
    float eta = n1 / n2;
    float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2_t >= 1.0) {
        return 1.0;   // 全内反射
    }
    float cos_t = sqrt(1.0 - sin2_t);
    float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    float rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i);
    return 0.5 * (rs * rs + rp * rp);
}

// 水下光照衰减：out_ambient / out_sun 是逐通道乘子（水上恒为 1）。
void WaterAttenuation(vec3 wp, out vec3 out_ambient, out vec3 out_sun) {
    out_ambient = vec3(1.0);
    out_sun = vec3(1.0);
    if (uWaterAttenOn == 0) {
        return;
    }
    float d = uWaterAttenLevelY - wp.y;   // > 0 = 该片元在水面以下
    if (!(d > 0.0)) {
        return;
    }
    if (uWaterAttenSun != 0) {
        float cy = uWaterAttenSunToward.y;
        if (!(cy > 1.0e-4)) {
            out_sun = vec3(0.0);   // 太阳在地平线以下：水下没有直射
        } else {
            float sin2_i = max(0.0, 1.0 - cy * cy);
            float sin2_w = sin2_i / (uWaterAttenIor * uWaterAttenIor);
            float cos_w = sqrt(max(1.0e-6, 1.0 - sin2_w));
            float path = d / cos_w;
            float t = 1.0 - WaterAttenFresnel(cy, 1.0, uWaterAttenIor);
            out_sun = t * exp(-uWaterAttenSigmaT * path);
        }
    }
    if (uWaterAttenAmbient != 0) {
        out_ambient = uWaterAttenAmbientT * exp(-uWaterAttenKd * d);
    }
}
)glsl";

// 把上面这组 uniform 上传到某个 program（内部 glUseProgram）。
// Pre-condition: prog != 0（0 时直接返回）；atten 可为 nullptr = 关闭（写确定值的关态）。
// 说明：location 不存在时 glUniform 是 no-op（安全），所以不必逐 program 判断变体。
void UploadUnderwaterAttenuation(ShaderManager& shader_mgr, unsigned int prog,
                                 const UnderwaterAttenuation* atten);

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_
