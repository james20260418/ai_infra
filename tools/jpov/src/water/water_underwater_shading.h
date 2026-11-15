// JPOV 水下光照衰减（三个 PBR 主 pass 共用）
//
// 定位：**主 pass 的照明侧**。水体的“视线侧”（吸收 + 注入）在合成趟里（water_medium_*）；
// 这里补的是另一半：
//   · 太阳平行光进水后的**方向折射**与**路径衰减**（逐通道）；
//   · 环境光的**界面透射**与**随深度的衰减**（逐通道）。
//
// 为什么必须做（2026-11-15 Danis 定）：不做的话，水下物体会按“空气中的太阳”被照明，
// 而只有相机→物体那一段被介质衰减 ⇒ 物体偏亮且偏暖（5m 纯水：红通道直射被高估 ≈9 倍）。
// 补上之后“水下物体发青”是算出来的，不是后期染色。
//
// 与介质的一致性（同一组参数、同一条公式，见 water_medium_math.h）：
//   直射：L_w = refracted(sun_toward, ior)；E_w(λ) = E(λ)·(1−R)·exp(−σt(λ)·d/cosθw)
//   环境：L_amb,w(λ) = L_amb(λ)·(1−R̄)·exp(−K_d(λ)·d)，K_d = σa + (1−g)·σs
//   d = level_y − p.y（>0 才在水下；水上恒等）
//
// 已知边界：
//   · K_d 用“折合衰减”近似（σa+(1−g)σs），不是解 RTE 的渐近本征值 ⇒ 浑浊水里有 1~2× 误差；
//   · 不含多次散射（与介质同一档次）；不含水下 ambient occlusion；
//   · 阴影仍按水面出射点采样 CSM（水下→水下的遮挡原理上不对，见 known_issue §11.1）。

#ifndef JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_
#define JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_medium_math.h"

namespace jpov {
namespace water {

// 主 pass 共用的 GLSL 片段：拼在三份 PBR 片元着色器里（紧跟 kEnvSpecularGlsl）。
//
// 用法（三份 main 逐字相同）：
//     vec3 w_sun_prop; vec3 w_sun_gain; vec3 w_amb_gain;
//     WaterShade(vWorldPos, uSunDir, w_sun_prop, w_sun_gain, w_amb_gain);
// 之后：直射方向用 w_sun_prop，`uSunColor*uSunIntensity` 乘 w_sun_gain，
//       ambient 乘 w_amb_gain。水上时三者都是恒等 ⇒ 逐字节零回归。
inline constexpr const char* kUnderwaterShadingGlsl = R"glsl(
uniform int   uWaterShadeOn;       // 0 = 本帧无水体（恒等，零开销）
uniform float uWaterShadeLevelY;   // 水面世界高度（米，y-up）
uniform float uWaterShadeIor;      // 界面折射率（与水体/水皮同一个值）
uniform vec3  uWaterShadeSigmaT;   // σt = σa+σs（1/m，逐通道）
uniform vec3  uWaterShadeKd;       // 环境折合衰减 K_d = σa+(1−g)σs（1/m，逐通道）
uniform vec3  uWaterShadeSunToward;// 指向太阳（空气侧，单位）
uniform float uWaterShadeAmbientT; // 界面漫射透射率（≈0.94）

// 无偏振电介质菲涅尔（与 water_skin_shader.h / water_medium_shader.h 同一实现）。
float WaterShadeFresnel(float cos_i, float n1, float n2) {
    float eta = n1 / n2;
    float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2_t >= 1.0) {
        return 1.0;
    }
    float cos_t = sqrt(1.0 - sin2_t);
    float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    float rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i);
    return 0.5 * (rs * rs + rp * rp);
}

// 水下光照衰减。out_sun_prop 是**水内光传播方向**（水上 = 传入的原方向）。
void WaterShade(vec3 wp, vec3 sun_prop, out vec3 out_sun_prop,
                out vec3 out_sun_gain, out vec3 out_amb_gain) {
    out_sun_prop = sun_prop;
    out_sun_gain = vec3(1.0);
    out_amb_gain = vec3(1.0);
    if (uWaterShadeOn == 0) {
        return;
    }
    float d = uWaterShadeLevelY - wp.y;   // >0 = 该片元在水面以下
    if (d <= 0.0) {
        return;
    }
    // ── 直射：折射方向 + 界面透射 + 太阳路径衰减 ──
    vec3 toward = uWaterShadeSunToward;
    float cy = toward.y;
    if (cy <= 1.0e-4) {
        out_sun_gain = vec3(0.0);   // 太阳在地平线以下：水下没有直射
    } else {
        float sin2_i = max(0.0, 1.0 - cy * cy);
        float sin2_w = sin2_i / (uWaterShadeIor * uWaterShadeIor);
        float cos_w = sqrt(max(0.0, 1.0 - sin2_w));
        float k = (sin2_i > 1.0e-12) ? sqrt(sin2_w / sin2_i) : (1.0 / uWaterShadeIor);
        vec3 toward_w = vec3(toward.x * k, cos_w, toward.z * k);
        out_sun_prop = -toward_w;   // 传播方向 = 指向太阳的反向
        float gain = 1.0 - WaterShadeFresnel(cy, 1.0, uWaterShadeIor);
        // 太阳到该点的水内路径 = d / cosθw；σt 逐通道 ⇒ exp 逐通道（红先没）
        out_sun_gain = gain * exp(-uWaterShadeSigmaT * (d / cos_w));
    }
    // ── 环境：界面漫射透射 + 随深度衰减 ──
    out_amb_gain = uWaterShadeAmbientT * exp(-uWaterShadeKd * d);
}
)glsl";

// 上传上面的 uniform 到某个 PBR program。
//
// Pre-condition: shader_mgr != nullptr；medium 可为 nullptr（= 关水体，全部置恒等）。
// 说明：location 不存在时 glUniform 是 no-op（安全），所以不必逐 program 判断变体。
void UploadUnderwaterShading(ShaderManager& shader_mgr, unsigned int prog,
                             const WaterMediumSample* medium);

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_UNDERWATER_SHADING_H_
