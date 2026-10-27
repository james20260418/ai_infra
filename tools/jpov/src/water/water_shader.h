// JPOV 水面（全局水/海）—— GLSL
//
// 三个 program：
//   1) kWaterVs          —— 全屏三角形（水面是解析平面，命中点在 FS 里解出来）
//   2) kWaterSurfaceFs   —— 水面片元：界面（Fresnel/TIR）+ 水柱内散射（单次散射闭式积分）
//   3) kWaterMediumFs    —— 整屏介质（相机在水下）：同一条积分，视线穿过整个水柱
//
// 坐标约定：世界 y-up，水面 = { y == uLevel }。
//
// 物理口径（2026-10-27 定，公式与 src/water/water_scattering.h 完全一致，有 CPU 对照测试）：
//   σa（吸收，1/m，逐通道）· σs（散射，1/m，近中性）· σt = σa+σs · K_d = σa+(1−g_amb)·σs
//   两条光源：QSSA 环境（等效竖直光束，软相位）+ 水下折射太阳（HG/Schlick 相位）
//   两项对线段长度都是 exp(a·u) ⇒ 闭式积分，不需要 raymarch / froxel。

#ifndef JPOV_SRC_WATER_WATER_SHADER_H_
#define JPOV_SRC_WATER_WATER_SHADER_H_

#include <string>

namespace jpov {

// ─────────────────────────────────────────────────────────────────────────────
// 共享块：介质参数 + 单次散射闭式积分（水面 FS 与介质 FS 用**同一份**，避免两处漂移）
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr const char* kWaterScatterGlsl = R"glsl(
// ---- 介质（1/m）----
uniform vec3  uSigmaA;        // 吸收系数（逐通道）
uniform vec3  uSigmaS;        // 散射系数（近中性；水下的青蓝来自吸收，不来自这里）
uniform float uPhaseGSun;     // 太阳相位各向异性（强前向 0.8~0.95）
uniform float uPhaseGAmb;     // 环境项软相位（散射场被 Snell 锥抹宽，0.2~0.4）
// ---- 光源 ----
uniform vec3  uSunIrradiance; // 水面**之下**的太阳辐照度（已含 Fresnel 透射 + cosθa/cosθw）
uniform vec3  uSunDirWater;   // 折射后的太阳传播方向（指向下，y<0）
uniform samplerCube uEnvCube; // 天光探针（环境项与反射共用）
uniform float uSkyScale;

const float kWPi = 3.14159265358979323846;

// Henyey-Greenstein，归一化 ∫p dω = 1。
float HgPhase(float c, float g) {
    float gg = g * g;
    float d = max(1.0 + gg - 2.0 * g * c, 1.0e-12);
    return (1.0 - gg) / (4.0 * kWPi * d * sqrt(d));
}
// 环境项方向调制 M = 4π·p：各向同性时恒为 1，前向时 >1（不会把量级带偏）。
float AmbMod(float c, float g) {
    return 4.0 * kWPi * HgPhase(c, g);
}
// 上半球平均辐亮度（+Y 面 mip5 覆盖 ±45°，即"上方漫射光"的粗糙平均）。
vec3 SkyIllumRadiance() {
    return textureLod(uEnvCube, vec3(0.0, 1.0, 0.0), 5.0).rgb * uSkyScale;
}
// 逐通道 ∫₀^len exp(a·u) du（a≈0 时取极限 len）。
vec3 IntegralExp(vec3 a, float len) {
    bvec3 tiny = lessThan(abs(a), vec3(1.0e-5));
    vec3 safe_a = mix(a, vec3(1.0), tiny);
    return mix((exp(a * len) - 1.0) / safe_a, vec3(len), tiny);
}

// 单次散射（闭式）。
//   omega_y : 视线方向的 y 分量（俯视 < 0，仰视 > 0）
//   z0      : 入水点深度（米）；水上入水 = 0，水下相机 = 相机深度
//   len     : 水柱段长度（米，必须整段在水里）
//   cos_sun : dot(太阳传播方向, 指向相机的方向)；朝太阳看 ⇒ 1
vec3 InScatter(float omega_y, float z0, float len, float cos_sun) {
    vec3 sig_t = uSigmaA + uSigmaS;
    // 漫衰减：σs→0 / g→1 / g→0 三个极限都对（见 CPU 单测）。
    vec3 kd = uSigmaA + (1.0 - clamp(uPhaseGAmb, 0.0, 1.0)) * uSigmaS;

    // 环境项：E_d0/π = 上半球平均辐亮度（π 约掉了）。
    float m_amb = AmbMod(omega_y, uPhaseGAmb);
    vec3 a_amb = kd * omega_y - sig_t;
    vec3 amb = uSigmaS * SkyIllumRadiance() * m_amb * exp(-kd * z0)
             * IntegralExp(a_amb, len);

    // 太阳项：斜程 τ = z/cosθw，cosθw = |sun_dir.y| ≥ 0.661。
    float c_sun = max(abs(uSunDirWater.y), 0.661);
    float p_sun = HgPhase(cos_sun, uPhaseGSun);
    vec3 a_sun = sig_t * (omega_y / c_sun - 1.0);
    vec3 sun = uSigmaS * p_sun * uSunIrradiance * exp(-sig_t * z0 / c_sun)
             * IntegralExp(a_sun, len);

    return amb + sun;
}
)glsl";

// ---- 1) 顶点：一块**水平巨大 quad**（10 km，跟随相机 XZ）----
//
// Danis 2026-10-27 定的实验版本：不用全屏射线求交，就用一块真实的四边形。
// 好处是深度由光栅器给出（early-Z 正常、不需要 gl_FragDepth），坏处是它**有边界**
// （远平面会把它切掉，见 FS 里的距离淡出）。
//
// 6 个顶点由 gl_VertexID 生成，无 VBO；中心 = (相机 x, 水面高度 level, 相机 z)。
static constexpr const char* kWaterVs = R"glsl(
#version 330 core
uniform mat4  uViewProj;
uniform vec3  uCenter;      // (cam.x, level, cam.z)
uniform float uHalfSize;    // 半边长（米）

out vec3 vWorld;

void main() {
    vec2 c;
    if (gl_VertexID == 0) {
        c = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        c = vec2( 1.0, -1.0);
    } else if (gl_VertexID == 2) {
        c = vec2(-1.0,  1.0);
    } else if (gl_VertexID == 3) {
        c = vec2(-1.0,  1.0);
    } else if (gl_VertexID == 4) {
        c = vec2( 1.0, -1.0);
    } else {
        c = vec2( 1.0,  1.0);
    }
    vec3 world = vec3(uCenter.x + c.x * uHalfSize,
                      uCenter.y,
                      uCenter.z + c.y * uHalfSize);
    vWorld = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
)glsl";

// ---- 2) 水面片元：**只做天光反射**（水皮第一版）----
//
// 输出 (天空反射色, alpha = F)：配合 GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA
//   out = sky(reflect)·F + dst·(1−F)
// 也就是"反射叠加在已有画面上、剩下的透过去"，不做吸收、不做折射、不做 TIR。
static constexpr const char* kWaterSurfaceFs = R"glsl(
#version 330 core

in vec3 vWorld;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 FragSceneDepth;

uniform vec3        uCameraPos;
uniform samplerCube uEnvCube;
uniform float       uEnvMaxLod;
uniform float       uSkyScale;
uniform float       uF0;
uniform float       uRoughness;
uniform float       uFarDistance;   // 远平面距离（做 quad 边缘淡出用）

void main() {
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 V = normalize(uCameraPos - vWorld);

    // Schlick 菲涅尔：正入射 2%（IOR 1.333），掠射 → 1。
    float cos_t = clamp(dot(N, V), 0.0, 1.0);
    float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_t, 5.0);

    // 天光反射（探针按 roughness → mip 做模糊）。
    vec3 refl = textureLod(uEnvCube, reflect(-V, N),
                           clamp(uRoughness, 0.0, 1.0) * uEnvMaxLod).rgb * uSkyScale;

    // quad 会被远平面裁掉 ⇒ 用距离淡出把边界藏掉（否则地平线附近一条硬边）。
    float dist = length(vWorld - uCameraPos);
    float fade = 1.0 - smoothstep(0.80 * uFarDistance, uFarDistance, dist);

    FragColor = vec4(refl, F * fade);
    FragSceneDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
)glsl";

// ---- 3) 整屏介质（相机在水下）----
//
// 无顶点缓冲：gl_VertexID 生成覆盖全屏的三角形（与天空/雾同一套写法）。
static constexpr const char* kWaterMediumVs = R"glsl(
#version 330 core
void main() {
    vec2 pos;
    if (gl_VertexID == 0) {
        pos = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        pos = vec2(3.0, -1.0);
    } else {
        pos = vec2(-1.0, 3.0);
    }
    gl_Position = vec4(pos, 1.0, 1.0);
}
)glsl";

inline const std::string kWaterMediumFs =
    std::string(R"glsl(
#version 330 core

layout(location = 0) out vec4 FragColor;

uniform vec2      uResolution;
uniform mat4      uInvVP;
uniform vec3      uCameraPos;
uniform sampler2D uSrcColor;      // 抓帧：颜色
uniform sampler2D uSceneDepth;    // 抓帧：MRT 场景深度
uniform float     uLevel;         // 水面高度（算入水点深度用）
)glsl") + kWaterScatterGlsl + R"glsl(
void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec3 c = texture(uSrcColor, uv).rgb;

    // 该像素的可见点距离（由 MRT 深度反投影）。
    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    vec3 hit = pw.xyz / pw.w;
    vec3 delta = hit - uCameraPos;
    float dist = length(delta);
    vec3 rd = delta / max(dist, 1.0e-6);

    // 相机在水下 ⇒ 整段都是水：z0 = 相机深度，段长 = 到可见点的距离。
    float z0 = max(uLevel - uCameraPos.y, 0.0);
    float cos_sun = dot(uSunDirWater, -rd);
    vec3 insc = InScatter(rd.y, z0, dist, cos_sun);

    vec3 sig_t = uSigmaA + uSigmaS;
    vec3 T = exp(-sig_t * dist);
    FragColor = vec4(c * T + insc, 1.0);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SHADER_H_
