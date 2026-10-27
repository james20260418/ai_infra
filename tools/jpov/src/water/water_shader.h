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

// ---- 1) 水面顶点着色器（全屏三角形）----
//
// 水是一个**解析平面** `y = uLevel`：不需要任何几何。全屏三角 + 片元里逐像素
// 射线求交即可（见 FS）。这带来的结构性好处：
//   · 没有网格 ⇒ 没有中心洞、没有环间距、没有远场折角、没有远平面裁出来的曲线；
//   · 地平线是**真地平线**：射线越贴近水平，t 越大；FS 里把 t 夹到 max_distance
//     并淡出到地平线天光色，「海天相接」就是无缝的。
//
// 可插拔点（将来做巨浪时只换这里）：命中点/法线现在是「解析平面 + 向上法线」；
// 换成「网格光栅化」或「对高度场迭代求交」时，FS 的菲涅耳/吸收/深度写入一行都不用改。
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

uniform vec2      uResolution;    // HDR FBO 像素尺寸（与抓帧副本一致）
uniform mat4      uInvVP;         // 主相机 逆(Proj*View)
uniform mat4      uViewProj;      // 主相机 Proj*View（写 gl_FragDepth 用）
uniform vec3      uCameraPos;
uniform float     uLevel;         // 水面世界高度（y-up）
uniform float     uMaxDistance;   // 水面最远可见距离（米）—— 超过就淡出到地平线天光色

uniform sampler2D uSceneColor;    // 抓帧：颜色（RGBA16F）
uniform sampler2D uSceneDepth;    // 抓帧：MRT 场景深度（R32F，值 = gl_FragCoord.z）

uniform samplerCube uEnvCube;     // 天光探针（与 env specular 同一张）
uniform float     uEnvMaxLod;
uniform float     uSkyScale;      // 天光消费量 [0,1]
uniform float     uF0;            // 正入射反射率
uniform float     uRoughness;     // 反射 mip 粗糙度

uniform vec3      uAbsorption;    // 1/米
uniform vec3      uScatter;        // 水体散射反照率（无量纲，乘下面的天空照明）
uniform float     uScatterScale;
// 天空照明（上半球平均辐亮度）：水体**内散射**必须跟它走 —— 否则夜里水也会发光。
// 这一项就是"远处天空漫射光把浅层水点亮"的那个量。由下面的 SkyIllum() 从探针现算。
vec3 SkyIllum() {
    // mip=5（128² → 4×4/面）：+Y 面在这个 mip 覆盖 ±45°，正是"上方漫射光"的粗糙平均。
    return textureLod(uEnvCube, vec3(0.0, 1.0, 0.0), 5.0).rgb * uSkyScale;
}
// 空气→水的辐亮度增益：(n_water/n_air)² = 1.777。
// 辐亮度守恒是 L/n² = const ⇒ 水下看到的天空 = 天空辐亮度 × 1.777（Snell 窗之所以"刺眼"）。
uniform float     uAirToWaterGain;
// 水面 RMS 斜率（弧度）。0 = 理想镜面（TIR 硬边）；见 WaterSurface::surface_slope_rms_deg。
uniform float     uSlopeRmsRad;

uniform int       uCamUnderwater;

// 用抓帧深度反推「相机 → 该像素背景」的距离（米）。
float SceneDistance(vec2 uv) {
    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    return length(pw.xyz / pw.w - uCameraPos);
}

// 地平线方向的天光色（远端淡出用；与反射探针同源，保证"海天相接"无接缝）。
//
// ⚠️ 必须用**低 mip（2~3）**、不能取 uEnvMaxLod：cubemap 的 ±X/±Z 面在最高 mip
//    是 1×1，那一个 texel 平均了"上半天空 + 下半地色" ⇒ 采出来是暗色，
//    在地平线上就会留下一条 1 px 的暗线（实测 y=168 那行比邻居暗 70）。
//    另外把方向抬到 +Y 一点点（0.04），避免正好压在天空/地色的分界上。
vec3 HorizonSky(vec3 dir) {
    vec3 h = normalize(vec3(dir.x, 0.04, dir.z));
    return textureLod(uEnvCube, h, 2.0).rgb * uSkyScale;
}

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;

    // ── 命中点：每像素射线 ∩ 平面 y = uLevel（解析解，无需任何几何）──
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pn = uInvVP * vec4(ndc, -1.0, 1.0);
    vec4 pf = uInvVP * vec4(ndc,  1.0, 1.0);
    vec3 rd = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    if (abs(rd.y) < 1.0e-6) {
        discard;                       // 视线与水面平行
    }
    float t_hit = (uLevel - uCameraPos.y) / rd.y;
    if (t_hit <= 0.0) {
        discard;                       // 水面在相机背后（上半屏几乎全是这条）
    }
    // 夹到最远距离：贴地平线的射线 t→∞，夹住之后淡出到天光色，不会出现硬边。
    float t = min(t_hit, uMaxDistance);
    vec3 world = uCameraPos + rd * t;

    // 深度：写 gl_FragDepth（硬件深度，后续 pass 靠它遮挡）+ MRT#1（场景深度契约）。
    vec4 clip = uViewProj * vec4(world, 1.0);
    float ndc_z = clip.z / max(abs(clip.w), 1.0e-9) * sign(clip.w);
    float depth = clamp(ndc_z * 0.5 + 0.5, 0.0, 1.0);
    gl_FragDepth = depth;
    FragSceneDepth = vec4(depth, 0.0, 0.0, 1.0);

    vec3 N = vec3(0.0, 1.0, 0.0);      // 本版水面法线恒向上（无波浪）
    vec3 V = normalize(uCameraPos - world);

    vec3 color;
    float roughness = clamp(uRoughness, 0.0, 1.0);

    if (uCamUnderwater == 1) {
        // ── 从水下往上看：全内反射 + Snell 窗 ──
        // 入射角按 |dot(N,V)| 取（法线朝上、相机在下 ⇒ dot < 0）。
        // ── 从水下往上看：Snell 窗 + 全内反射（TIR）──
        //
        // 物理要素（2026-10-27 联网核对，之前漏了两条）：
        //   ① 辐亮度守恒 L/n² = const ⇒ 水下看到的天空 = 天空辐亮度 × (1.333)² = 1.777
        //      （这就是真实 Snell 窗"刺眼"的来源）；
        //   ② 窗口**边缘是暗的**：边缘对应掠射入射（θ_air→90°），透射率 T→0，
        //      所以亮度在锥边缘收敛到 0（不是我上一版的"亮环"）；
        //   ③ 锥外 = 全内反射：看到的是水下环境（这里近似成水体内散射）。
        //
        // 界面反射率 R 用未极化的精确 Fresnel；临界角附近再用**水面 RMS 斜率**把台阶
        // 抹开（真实水面不平 ⇒ 斜率分布对 R 做卷积；σ=0 时退化为纯物理硬边）。
        float ci = clamp(abs(dot(N, V)), 1.0e-4, 1.0);
        const float kEta = 1.333;                       // n_water / n_air
        const float kCosCrit = 0.6614;                  // cosθc，sinθc = 1/1.333
        float sin2t = kEta * kEta * (1.0 - ci * ci);    // sin²θt
        float R;
        if (sin2t >= 1.0) {
            R = 1.0;                                    // 全内反射
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

        // 透射侧：整个天空被压进 97.2° 锥 + n² 增益。
        vec3 win;
        float lod = roughness * uEnvMaxLod;
        if (sin2t < 1.0) {
            vec3 rd = refract(-V, -N, kEta);
            win = uAirToWaterGain * textureLod(uEnvCube, normalize(rd), lod).rgb * uSkyScale;
        } else {
            // 粗糙界面在临界角附近仍会有少量"透过"的抽样：用地平线方向的天空当代表。
            vec3 rh = reflect(-V, -N);
            win = uAirToWaterGain *
                  textureLod(uEnvCube, normalize(vec3(rh.x, 0.06, rh.z)), lod).rgb * uSkyScale;
        }

        // 反射侧（锥外）：看到的是水下环境 —— 近似为水体内散射（随天光强度走）。
        vec3 refl = uScatter * uScatterScale * SkyIllum();

        color = Rs * refl + (1.0 - Rs) * win;
    } else {
        // ── 从水上往下看：反射 + 逐通道吸收背景 ──
        // 背景是"水下物体按空气光照渲出来的辐亮度"。光从水出射到空气时，
        // 辐亮度守恒式是 L/n² = const ⇒ 出射后再乘 1/n² = 0.5625（这是"水下物体
        // 看起来更暗"的物理来源之一，另一半是吸收）。
        vec3 bg = texture(uSceneColor, uv).rgb / uAirToWaterGain;
        float d_surf = length(world - uCameraPos);
        float d_bg   = SceneDistance(uv);
        float L      = max(d_bg - d_surf, 0.0);      // 水下段的视线长度（米）

        vec3 T = exp(-uAbsorption * L);
        // 内散射 = 散射反照率 × 天空照明（不是常数色：夜里必须跟着变暗）
        vec3 in_scatter = uScatter * uScatterScale * SkyIllum();
        vec3 under = bg * T + in_scatter * (1.0 - T);

        float cos_t = clamp(dot(N, V), 0.0, 1.0);
        float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_t, 5.0);
        vec3 refl = textureLod(uEnvCube, reflect(-V, N), roughness * uEnvMaxLod).rgb * uSkyScale;

        color = under * (1.0 - F) + refl * F;
    }

    // 远端淡出：贴地平线的水面收敛到「地平线天光色」——把网格时代那条硬缝做掉。
    float far_fade = smoothstep(0.75 * uMaxDistance, uMaxDistance, t);
    color = mix(color, HorizonSky(rd), far_fade);

    FragColor = vec4(color, 1.0);
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

uniform vec3        uAbsorption;
uniform vec3        uScatter;
uniform float       uScatterScale;
// 天空照明（上半球平均辐亮度）：浅层水被上方漫射光照亮，夜里必须跟着变暗。
uniform samplerCube uEnvCube;
uniform float       uSkyScale;

void main() {
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec3 c = texture(uSrcColor, uv).rgb;

    float w = texture(uSceneDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 pw = uInvVP * vec4(ndc, w * 2.0 - 1.0, 1.0);
    float dist = length(pw.xyz / pw.w - uCameraPos);

    vec3 T = exp(-uAbsorption * dist);
    // mip=5：+Y 面覆盖 ±45°，是"上方漫射光"的粗糙平均（与水面 FS 同一口径）。
    vec3 sky_illum = textureLod(uEnvCube, vec3(0.0, 1.0, 0.0), 5.0).rgb * uSkyScale;
    vec3 in_scatter = uScatter * uScatterScale * sky_illum;
    FragColor = vec4(c * T + in_scatter * (1.0 - T), 1.0);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SHADER_H_
