// JPOV 水面（全局水/海）—— GLSL
//
// 三个 program：
//   1) kWaterVs          —— 极坐标 LOD 网格的顶点位移（波浪；amp=0 时输出严格平面）
//   2) kWaterSurfaceFs   —— 水面片元：天光反射（菲涅尔）+ 水下段 Beer-Lambert 吸收/散射
//   3) kWaterMediumFs    —— 整屏介质（相机在水下时）：整条视线上的吸收/散射
//
// 坐标约定：世界 y-up，水面 = { y == uLevel }。网格顶点是**相机为中心的局部 XZ**，
// VS 里加上相机 XZ 变成世界坐标 —— 于是"网格跟着相机走、波浪留在世界里不游动"。

#ifndef JPOV_SRC_WATER_WATER_SHADER_H_
#define JPOV_SRC_WATER_WATER_SHADER_H_

namespace jpov {

// ---- 1) 水面顶点着色器 ----
//
// 输入 aXZ = 平面局部坐标（米，以相机 XZ 为原点；极坐标 LOD 网格生成器给出）。
// 输出 vWorld / vNormal 供片元用。
//
// 波浪：三波正弦叠加（方向按 0 / ±40° 旋开、波长与振幅递减）。
//   · 位移**只作用在 y**（不做 Gerstner 的水平挤压/尖峰），但法线是按**解析梯度**算的，
//     所以光照/反射的抖动是对的，只是浪尖不够"卷"。Gerstner 的水平项留给下一版。
//   · uWaveAmp == 0 时整段跳过（编译期外的运行期分支，代价 = 一次比较）。
// 相位用**世界坐标**算 ⇒ 相机移动时浪形固定在世界里，不会"跟着相机游"。
static constexpr const char* kWaterVs = R"glsl(
#version 330 core

layout(location = 0) in vec2 aXZ;   // 平面局部坐标（相机为中心的 XZ，米）

uniform mat4  uViewProj;
uniform vec3  uCameraPos;
uniform float uLevel;

uniform float uWaveAmp;
uniform float uWaveLength;
uniform vec2  uWaveDir;      // 已归一化的水平方向
uniform float uWaveTime;

out vec3 vWorld;
out vec3 vNormal;

const float kTau = 6.28318530717958647692;
const float kG = 9.81;       // 重力加速度：深水色散 ω = sqrt(g·k)

void main() {
    vec2 p = aXZ + uCameraPos.xz;     // 世界 XZ
    float height = 0.0;
    vec2  grad = vec2(0.0);

    if (uWaveAmp > 0.0) {
        for (int i = 0; i < 3; ++i) {
            float fi = float(i);
            // 方向旋开：0° / +40° / −40°
            float ang = (i == 0) ? 0.0 : ((i == 1) ? 0.6981317 : -0.6981317);
            float ca = cos(ang);
            float sa = sin(ang);
            vec2 dir = vec2(uWaveDir.x * ca - uWaveDir.y * sa,
                            uWaveDir.x * sa + uWaveDir.y * ca);
            float len = uWaveLength / (1.0 + 0.6 * fi);      // 波长递减
            float k = kTau / max(len, 0.001);
            float amp = uWaveAmp / (1.0 + 1.2 * fi);         // 振幅递减
            float phase = k * dot(dir, p) - sqrt(kG * k) * uWaveTime + fi * 1.7;
            height += amp * sin(phase);
            grad   += amp * k * cos(phase) * dir;            // d(height)/d(xz)
        }
    }

    vec3 world = vec3(p.x, uLevel + height, p.y);
    vWorld  = world;
    // 法线 = normalize(-dh/dx, 1, -dh/dz)
    vNormal = normalize(vec3(-grad.x, 1.0, -grad.y));

    gl_Position = uViewProj * vec4(world, 1.0);
}
)glsl";

// ---- 2) 水面片元（相机在水面上方）----
//
// 输入是**抓帧副本**（水面画之前的主画面）：颜色 + MRT 场景深度。
// 输出 final color **直接写**（关混合），因为背景已经在 src 里、且要被逐通道吸收：
//   out = 反射·F + (背景·T + 散射·(1−T))·(1−F)
//   T = exp(−σ·L)，L = 该像素视线在水下的长度（场景距离 − 水面距离）
//
// 相机在水下（uCamUnderwater==1）时改走"从下看"分支：全内反射 + Snell 窗，
// 同时**不做**吸收（整屏介质由 kWaterMediumFs 负责，避免双重衰减）。
static constexpr const char* kWaterSurfaceFs = R"glsl(
#version 330 core

layout(location = 0) out vec4 FragColor;
// MRT #1：场景深度（gl_FragCoord.z）。水面必须写它，否则该像素的"场景深度"未定义，
// 会让后面的雾/粒子软深度读到垃圾。
layout(location = 1) out vec4 FragSceneDepth;

in vec3 vWorld;
in vec3 vNormal;

uniform vec2      uResolution;    // HDR FBO 像素尺寸（与抓帧副本一致）
uniform mat4      uInvVP;         // 主相机 逆(Proj*View)
uniform vec3      uCameraPos;

uniform sampler2D uSceneColor;    // 抓帧：颜色（RGBA16F）
uniform sampler2D uSceneDepth;    // 抓帧：MRT 场景深度（R32F，值 = gl_FragCoord.z）

uniform samplerCube uEnvCube;     // 天光探针（与 env specular 同一张）
uniform float     uEnvMaxLod;
uniform float     uSkyScale;      // 天光消费量 [0,1]
uniform float     uF0;            // 正入射反射率
uniform float     uRoughness;     // 反射 mip 粗糙度

uniform vec3      uAbsorption;    // 1/米
uniform vec3      uScatter;
uniform float     uScatterScale;

uniform int       uCamUnderwater;

// 用抓帧深度反推「相机 → 该像素背景」的距离（米）。
float SceneDistance(vec2 uv) {
    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    return length(pw.xyz / pw.w - uCameraPos);
}

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorld);

    vec3 color;
    float roughness = clamp(uRoughness, 0.0, 1.0);

    if (uCamUnderwater == 1) {
        // ── 从水下往上看：全内反射 + Snell 窗 ──
        // 入射角按 |dot(N,V)| 取（法线朝上、相机在下 ⇒ dot < 0）。
        float ci = clamp(abs(dot(N, V)), 0.0, 1.0);
        float si = sqrt(max(0.0, 1.0 - ci * ci));
        float st = 1.333 * si;                       // 水→空气：sinθt = 1.333·sinθi
        // 全内反射侧：探针下半球是地色（≈水下环境的低频替身），再叠一点水体散射色，
        // 否则整片"水做的天花板"会读成一块死黑（实测：直接采样探针看着像个洞）。
        vec3 refl = textureLod(uEnvCube, reflect(-V, -N), 0.35 * uEnvMaxLod).rgb * uSkyScale;
        refl = mix(uScatter * uScatterScale, refl, 0.55);
        // 窗口边沿软化：sinθt 过 1 的那一圈是锐利边界，硬切会出现一圈"LED 环"。
        float window = 1.0 - smoothstep(0.95, 1.0, st);
        vec3 refr_dir = refract(-V, -N, 1.333);      // 折射进空气 = 透过水面看天
        vec3 win = textureLod(uEnvCube, normalize(refr_dir), roughness * uEnvMaxLod).rgb * uSkyScale;
        color = mix(refl, win, window);
    } else {
        // ── 从水上往下看：反射 + 逐通道吸收背景 ──
        vec3 bg = texture(uSceneColor, uv).rgb;
        float d_surf = length(vWorld - uCameraPos);
        float d_bg   = SceneDistance(uv);
        float L      = max(d_bg - d_surf, 0.0);      // 水下段的视线长度（米）

        vec3 T = exp(-uAbsorption * L);
        vec3 under = bg * T + uScatter * uScatterScale * (1.0 - T);

        float cos_t = clamp(dot(N, V), 0.0, 1.0);
        float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_t, 5.0);
        vec3 refl = textureLod(uEnvCube, reflect(-V, N), roughness * uEnvMaxLod).rgb * uSkyScale;

        color = under * (1.0 - F) + refl * F;
    }

    FragColor = vec4(color, 1.0);
    FragSceneDepth = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
)glsl";

// ---- 3) 整屏介质（相机在水下）----
//
// 无顶点缓冲：用 gl_VertexID 生成覆盖全屏的三角形（与天空/雾同一套写法）。
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

//
// 相机在水下时，整条"相机 → 可见面"的视线都在水里 ⇒ 对**已成画面**做一次
// 逐通道吸收 + 散射。输入同样是抓帧副本（颜色 + 深度），输出直接写（关混合）。
// 放在所有 3D 内容（含水下水面本身）之后、雾之前。
static constexpr const char* kWaterMediumFs = R"glsl(
#version 330 core

layout(location = 0) out vec4 FragColor;

uniform vec2      uResolution;
uniform mat4      uInvVP;
uniform vec3      uCameraPos;
uniform sampler2D uSrcColor;
uniform sampler2D uSceneDepth;

uniform vec3      uAbsorption;
uniform vec3      uScatter;
uniform float     uScatterScale;

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec3 c = texture(uSrcColor, uv).rgb;

    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    float dist = length(pw.xyz / pw.w - uCameraPos);

    vec3 T = exp(-uAbsorption * dist);
    FragColor = vec4(c * T + uScatter * uScatterScale * (1.0 - T), 1.0);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SHADER_H_
