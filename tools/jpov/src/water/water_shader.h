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

// ---- 1) 顶点：全屏三角形 ----
//
// 水是一个**解析平面** `y = uLevel`：不需要任何几何。全屏三角 + 片元里逐像素
// 射线求交即可（见 FS）。结构性好处：没有网格 ⇒ 没有中心洞、环间距、远场折角、
// 远平面裁出来的曲线；地平线是真地平线（t 夹到 max_distance 后淡出到天光色）。
//
// 可插拔点（将来做巨浪时只换这里）：命中点/法线现在是「解析平面 + 向上法线」。
static constexpr const char* kWaterVs = R"glsl(
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

// ---- 2) 水面片元 ----
inline const std::string kWaterSurfaceFs =
    std::string(R"glsl(
#version 330 core

layout(location = 0) out vec4 FragColor;
// MRT #1：场景深度（gl_FragCoord.z）。水面必须写它，否则该像素的"场景深度"未定义，
// 会让后面的雾/粒子软深度读到垃圾。
layout(location = 1) out vec4 FragSceneDepth;

uniform vec2      uResolution;
uniform mat4      uInvVP;
uniform mat4      uViewProj;
uniform vec3      uCameraPos;
uniform float     uLevel;
uniform float     uMaxDistance;   // 水面最远可见距离（米）—— 超过就淡出到地平线天光色
uniform sampler2D uSceneColor;    // 抓帧：颜色
uniform sampler2D uSceneDepth;    // 抓帧：MRT 场景深度（R32F，值 = gl_FragCoord.z）
uniform float     uEnvMaxLod;
uniform float     uF0;
uniform float     uRoughness;
uniform float     uAirToWaterGain;  // (n_w/n_a)² = 1.777（辐亮度守恒 L/n²）
uniform float     uSlopeRmsRad;     // 水面 RMS 斜率（水面不平 ⇒ 临界角被卷积开）
uniform float     uSunImageRad;     // 日盘角展宽（弧度）——只用于"太阳的折射像"
uniform int       uCamUnderwater;
)glsl") + kWaterScatterGlsl + R"glsl(
// 用抓帧深度反推「相机 → 该像素背景」的距离（米）。
float SceneDistance(vec2 uv) {
    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    return length(pw.xyz / pw.w - uCameraPos);
}

// 地平线方向的天光色（远端淡出用）。
// ⚠️ 必须用低 mip：cubemap 的 ±X/±Z 面在最高 mip 只有 1×1 texel，那个值平均了
//    "上半天空 + 下半地色" ⇒ 采出来是暗色，会在地平线压出一条 1 px 暗线（实测 Δ=70）。
vec3 HorizonSky(vec3 dir) {
    vec3 h = normalize(vec3(dir.x, 0.04, dir.z));
    return textureLod(uEnvCube, h, 2.0).rgb * uSkyScale;
}

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;

    // ── 命中点：每像素射线 ∩ 平面 y = uLevel ──
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 rd = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    if (abs(rd.y) < 1.0e-6) {
        discard;
    }
    float t_hit = (uLevel - uCameraPos.y) / rd.y;
    if (t_hit <= 0.0) {
        discard;                          // 水面在相机背后（上半屏几乎全是这条）
    }
    float t = min(t_hit, uMaxDistance);   // 贴地平线的射线 t→∞，夹住后淡出
    vec3 world = uCameraPos + rd * t;

    // 深度：写 gl_FragDepth + MRT#1（两者同值）。
    vec4 clip = uViewProj * vec4(world, 1.0);
    float ndc_z = clip.z / max(abs(clip.w), 1.0e-9) * sign(clip.w);
    float depth = clamp(ndc_z * 0.5 + 0.5, 0.0, 1.0);
    gl_FragDepth = depth;
    FragSceneDepth = vec4(depth, 0.0, 0.0, 1.0);

    vec3 N = vec3(0.0, 1.0, 0.0);         // 本版水面法线恒向上（无波浪）
    vec3 V = normalize(uCameraPos - world);
    float cos_sun = dot(uSunDirWater, -rd);

    vec3 color;
    if (uCamUnderwater == 1) {
        // ── 从水下往上看：Snell 窗（×n² 增益）+ 全内反射 ──
        // 界面反射率用精确 Fresnel（未极化）；临界角附近用 RMS 斜率把台阶抹开。
        float ci = clamp(abs(dot(N, V)), 1.0e-4, 1.0);
        const float kEta = 1.333;
        const float kCosCrit = 0.6614;
        float sin2t = kEta * kEta * (1.0 - ci * ci);
        float R;
        if (sin2t >= 1.0) {
            R = 1.0;                       // 全内反射
        } else {
            float ct = sqrt(1.0 - sin2t);
            float rs = (kEta * ci - ct) / (kEta * ci + ct);
            float rp = (kEta * ct - ci) / (kEta * ct + ci);
            R = 0.5 * (rs * rs + rp * rp);
        }
        float soft = 0.75 * max(uSlopeRmsRad, 0.0);
        float Rs = (soft > 1.0e-4)
                     ? mix(R, 1.0, 1.0 - smoothstep(kCosCrit - soft, kCosCrit + soft, ci))
                     : R;
        // 窗口内：整片天空被压进 97.2° 锥 + 辐亮度 ×n²（Snell 窗"刺眼"的来源）。
        vec3 win;
        float lod = clamp(uRoughness, 0.0, 1.0) * uEnvMaxLod;
        if (sin2t < 1.0) {
            vec3 rdir = normalize(refract(-V, -N, kEta));
            win = uAirToWaterGain * textureLod(uEnvCube, rdir, lod).rgb * uSkyScale;
            // 太阳的**折射像**（透射，不是界面镜面高光 —— 界面高光另算）。
            // 天光探针按设计不含日盘（避免与直射光照双计），所以这里单独补：
            //   角展宽 = 日盘展宽（高频微起伏把日盘抹开，见 WaterSurface::sun_image_deg）；
            //   峰值辐亮度 = E_sun / (π·σ²)（保证 ∫L dω = E_sun，能量守恒）。
            float sigma = max(uSunImageRad, 0.0047);   // 下限 = 太阳自身角半径
            float cos_a = clamp(dot(rdir, -uSunDirWater), -1.0, 1.0);
            float ang2 = 2.0 * (1.0 - cos_a);          // ≈ θ²
            float img = exp(-ang2 / (sigma * sigma));
            win += uSunIrradiance * uAirToWaterGain * (img / (kWPi * sigma * sigma));
        } else {
            // 粗糙界面在临界角附近仍有少量透过：用地平线方向的天空当代表。
            vec3 rh = reflect(-V, -N);
            win = uAirToWaterGain * textureLod(uEnvCube, normalize(vec3(rh.x, 0.06, rh.z)), lod).rgb
                * uSkyScale;
        }
        // 锥外：全内反射 —— 看到的是水下环境。近似为"半无限深水体的渐近散射辐亮度"
        // σs·L/σt（比"常数色"有依据：它随天光与介质参数变化）。镜面方向取反射方向。
        vec3 sig_t = uSigmaA + uSigmaS;
        vec3 rh_refl = reflect(-V, -N);
        vec3 refl = uSigmaS * SkyIllumRadiance() * AmbMod(rh_refl.y, uPhaseGAmb)
                  / max(sig_t, vec3(1.0e-4));
        color = Rs * refl + (1.0 - Rs) * win;
    } else {
        // ── 从水上往下看：吸收 + 内散射（闭式积分）+ 界面反射 ──
        vec3 bg = texture(uSceneColor, uv).rgb;      // 水下物体（按空气光照渲出）
        float d_surf = length(world - uCameraPos);
        float d_bg   = SceneDistance(uv);
        float L      = max(d_bg - d_surf, 0.0);      // 水下段长度
        vec3 sig_t   = uSigmaA + uSigmaS;
        vec3 T       = exp(-sig_t * L);
        // 入水点就在水面 ⇒ z0 = 0；视线 y 分量 = rd.y（俯视 < 0）
        vec3 insc    = InScatter(rd.y, 0.0, L, cos_sun);
        // 物体的辐亮度与内散射都是"水中的辐亮度"，出射到空气要除以 n²。
        vec3 under   = (bg * T + insc) / uAirToWaterGain;

        float cos_t = clamp(dot(N, V), 0.0, 1.0);
        float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_t, 5.0);
        vec3 refl = textureLod(uEnvCube, reflect(-V, N),
                               clamp(uRoughness, 0.0, 1.0) * uEnvMaxLod).rgb * uSkyScale;
        color = under * (1.0 - F) + refl * F;
    }

    // 远端淡出：贴地平线的水面收敛到「地平线天光色」。
    float far_fade = smoothstep(0.75 * uMaxDistance, uMaxDistance, t);
    color = mix(color, HorizonSky(rd), far_fade);

    FragColor = vec4(color, 1.0);
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
