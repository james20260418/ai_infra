// JPOV SkyRenderer — 程序化白天天光（Preetham）背景渲染
//
// 用解析式大气模型（Preetham-Shirley-Smits 1999）计算白天天空背景，
// 与 3D 相机视角一致（相机逆 VP 重建视线方向）。纯程序化、参数少、连续可动画，
// 支持太阳位置/天气(turbidity)/季节(season)/亮度(intensity)。
//
// 设计要点：
//   - 独立轻量 sky shader（含 Preetham 函数），一帧一次 draw，不掺 object3d。
//   - 先画天球（垫 3D FBO 背景）→ 再画 3D 物体（深度测试覆盖）。
//   - 地平线以下（pitch<0）画纯色 ground_color（避免天空倒影）。
//   - HDR：输出原始亮度（可 >1.0），不做 tone map，由后处理统一压缩。
//   - 夜空底色（2026-09-20 引入）：night_zenith_color → night_horizon_color 的
//     垂直渐变（van Rhijn 形状），**加法**叠在白天项上（受 intensity、不受 season）；
//     按 (1−daylight) 淡入（白天零影响、日落后满值）。
//   - 月亮盘（2026-09-21 引入）：moon_dir + moon_* 一组参数，与日盘走**同一推导链**
//     （俯仰角重映射 / 黑体色温 / Beer-Lambert 衰减 / 角度空间盘 mask / 高斯光晕）。
//     月盘默认亮度 0 = 关闭（既有画面逐字节不变）。仍不画月相/星星。
//
// 曲线来源：Preetham 模型实现基于 Erin Catto (box3d, MIT) 的 preetham.glsl，
// 论文 "A Practical Analytic Model for Daylight" (Preetham, Shirley, Smits 1999)。
//   https://github.com/erincatto/box3d/blob/main/samples/shaders/common/preetham.glsl
//
// Pre-condition: GL context 已激活。

#ifndef JPOV_SKYDOME_SKY_RENDERER_H_
#define JPOV_SKYDOME_SKY_RENDERER_H_

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {

class SkyRenderer {
public:
    SkyRenderer() = default;
    ~SkyRenderer() = default;

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    // ---- 天光 shader 源码（独立 program，经 ShaderManager 编译）----
    //   shader_mgr.GetOrCreate("sky", {kSkyVs, kSkyFs})
    //
    // kSkyVs: 全屏三角形（cover full NDC），不做逐顶点方向。
    // kSkyFs: Preetham 大气模型算天空色 + 地平线下地色 + 日月发光圆盘。
    static constexpr const char* kSkyVs = R"glsl(
#version 330 core
// 全屏三角形：三个顶点覆盖整个 NDC（-1..1），无需顶点缓冲。
out vec2 vNDC;   // 该片元的 NDC 坐标（FS 里用于逆 VP 重建视线方向）
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vNDC = pos;
    gl_Position = vec4(pos, 1.0, 1.0);
}
)glsl";

    // kSkyFs: Preetham 程序化天空 + 地平线下地色 + 日月圆盘 + 夜空底色（双色）。    //   NDC 由 gl_FragCoord/uResolution 反推（不用 VS varying：无 VAO 全屏
    //   三角形上 varying 插值不可靠，实测 vNDC 几乎不变导致方向重建错误）。
    static constexpr const char* kSkyFs = R"glsl(
#version 330 core
out vec4 FragColor;

uniform vec2  uResolution;    // 当前 FBO 分辨率（像素），用于 gl_FragCoord→NDC
uniform mat4  uInvVP;         // 相机 逆(Proj*View)
uniform vec3  uCamPos;        // 相机世界位置（方向 = 反投影点 - 相机）
uniform vec3  uSunDir;        // 太阳位置单位向量（世界中，y-up）
uniform vec3  uMoonDir;       // 月亮位置单位向量（世界中，y-up；独立于 uSunDir）
uniform float uTurbidity;     // 浊度（天气）：2 清澈…8 霾
uniform vec3  uSeason;        // 季节色温乘子（冬冷/夏暖）
uniform float uIntensity;     // 天光亮度标量
uniform vec3  uGroundColor;   // 地平线以下地色
uniform vec3  uNightZenith;   // 夜空底色：天顶方向（线性 HDR；全 0 = 关闭夜色）
uniform vec3  uNightHorizon;  // 夜空底色：地平线方向（线性 HDR；全 0 = 关闭夜色）
uniform float uSunRadius;     // 太阳盘角半径（弧度，~0.0047~0.015；≤0 不画）
uniform float uSunBrightness; // 太阳盘自发光亮度基数（HDR，~1e6；≤0 不画）
uniform float uSunGlow;       // 太阳盘光晕强度（艺术参数，0=无光晕，~1 默认）
// 日盘俯仰角重映射（只改日盘位置，不碰天空散射/昼夜/色温/衰减）：
// 真实仰角 < uSunSetStartAngle 时，盘俯仰角 = uSunSetStartAngle
//   + uSunSetAngleRatio × (真实仰角 − uSunSetStartAngle)（盘比太阳降得更快）。
uniform float uSunSetStartAngle; // 重映射起始阈值（度），建议 [0,60]，默认 10
uniform float uSunSetAngleRatio;  // 阈值以下盘压速比，默认 1.4
// 月亮盘（与日盘同义的一套参数；uMoonBrightness ≤ 0 或 uMoonRadius ≤ 0 = 不画月盘）
uniform float uMoonRadius;        // 月亮盘角半径（弧度；≤ 0 不画）
uniform float uMoonBrightness;    // 月亮盘自发光亮度基数（HDR；≤ 0 不画）
uniform float uMoonGlow;          // 月亮盘光晕强度（艺术参数；0 = 无光晕）
uniform float uMoonSetStartAngle; // 月盘俯仰角重映射起始阈值（度），默认 10
uniform float uMoonSetAngleRatio; // 月盘阈值以下压速比，默认 1.4

const float PI = 3.14159265358979323846;
// 天光亮度归一化系数：把 Preetham 输出的物理天顶亮度（~几千 cd/m²，即几 kcd/m²）
// 压到 JPOV 的 HDR 亮度标尺，使 `SkyCommand::intensity = 1.0` 时正好对应
// **正午晴天**的蓝天背景。
//
// 定值依据（2026-08-19 定，Danis 确认）：在本系数 + intensity=1.0 + ACES tone
// mapping 下，standard_sunny_day 场景天空呈现正常蓝天渐变（顶部深蓝 → 地平线
// 浅蓝发白），与太阳直射/阴影/环境光（sun=3, ambient=0.3，见 LIGHT_INTENSITY.md
// 晴天基准值）同一 HDR 标尺、协调一致。
//
// 注：此系数与 Preetham 实现的“绝对 radiance”量级相关，但非独立物理常数——
// 它必须与 JPOV 的 HDR 归一化 + ACES tone map 白点配套，方能坐 `intensity=1.0
// = 正午晴天` 这一锚点。不得与其它引擎的 Preetham 乘数直接对标。
const float SKY_LUMINANCE_SCALE = 0.04;

// ===== Preetham 大气模型（基于 box3d preetham.glsl，MIT）=====
float preethamPerez(float cos_theta, float cos_gamma, float gamma,
    float A, float B, float C, float D, float E) {
    float term1 = 1.0 + A * exp(B / max(cos_theta, 0.01));
    float term2 = 1.0 + C * exp(D * gamma) + E * cos_gamma * cos_gamma;
    return term1 * term2;
}
vec3 preethamXyY_to_XYZ(float x, float y, float Y) {
    float yy = max(y, 1.0e-5);
    return vec3(Y * x / yy, Y, Y * (1.0 - x - y) / yy);
}
vec3 preethamXYZ_to_linear_srgb(vec3 c) {
    return vec3(
        dot(c, vec3(3.2404542, -1.5371385, -0.4985314)),
        dot(c, vec3(-0.9692660, 1.8760108, 0.0415560)),
        dot(c, vec3(0.0556434, -0.2040259, 1.0572252)));
}
vec3 preethamSky(vec3 view_dir, vec3 sun_dir, float turbidity) {
    // 太阳限制在上半球（Preetham 在太阳低于地平线时发散；地平线以下由 ground_color 处理）
    float sun_y = clamp(sun_dir.y, 0.0, 1.0);
    // 视线钳到略高于地平线，避免低于地平线的发散（此时用 ground_color 的地方被 clamp 成贴着地平线的天）
    vec3 view_clamped = normalize(vec3(view_dir.x, max(view_dir.y, 0.0) + 0.01, view_dir.z));
    float cos_theta = max(view_clamped.y, 0.0);
    float cos_gamma = clamp(dot(sun_dir, view_clamped), -1.0, 1.0);
    float gamma = acos(cos_gamma);
    float sun_theta = acos(sun_y);

    float T = max(turbidity, 1.0);
    float T2 = T * T;
    // Perez 系数（Preetham 表 2），Y/x/y 各一组
    float A_Y = 0.1787*T - 1.4630, B_Y = -0.3554*T + 0.4275, C_Y = -0.0227*T + 5.3251, D_Y = 0.1206*T - 2.5771, E_Y = -0.0670*T + 0.3703;
    float A_x = -0.0193*T - 0.2592, B_x = -0.0665*T + 0.0008, C_x = -0.0004*T + 0.2125, D_x = -0.0641*T - 0.8989, E_x = -0.0033*T + 0.0452;
    float A_y = -0.0167*T - 0.2608, B_y = -0.0950*T + 0.0092, C_y = -0.0079*T + 0.2102, D_y = -0.0441*T - 1.6537, E_y = -0.0109*T + 0.0529;

    // 天顶点色度（Preetham 式 A.3）
    float ts = sun_theta, ts2 = ts*ts, ts3 = ts2*ts;
    float x_z =
        (0.00166*ts3 - 0.00375*ts2 + 0.00209*ts) * T2 +
        (-0.02903*ts3 + 0.06377*ts2 - 0.03202*ts + 0.00394) * T +
        (0.11693*ts3 - 0.21196*ts2 + 0.06052*ts + 0.25886);
    float y_z =
        (0.00275*ts3 - 0.00610*ts2 + 0.00317*ts) * T2 +
        (-0.04214*ts3 + 0.08970*ts2 - 0.04153*ts + 0.00516) * T +
        (0.15346*ts3 - 0.26756*ts2 + 0.06670*ts + 0.26688);
    // 天顶亮度（式 A.2）
    float chi = (4.0 / 9.0 - T / 120.0) * (PI - 2.0 * sun_theta);
    float Y_z = (4.0453*T - 4.9710) * tan(chi) - 0.2155*T + 2.4192;

    float cos_zen_gamma = sun_y;
    float pY_v = preethamPerez(cos_theta, cos_gamma, gamma, A_Y,B_Y,C_Y,D_Y,E_Y);
    float px_v = preethamPerez(cos_theta, cos_gamma, gamma, A_x,B_x,C_x,D_x,E_x);
    float py_v = preethamPerez(cos_theta, cos_gamma, gamma, A_y,B_y,C_y,D_y,E_y);
    float pY_z = preethamPerez(1.0, cos_zen_gamma, sun_theta, A_Y,B_Y,C_Y,D_Y,E_Y);
    float px_z = preethamPerez(1.0, cos_zen_gamma, sun_theta, A_x,B_x,C_x,D_x,E_x);
    float py_z = preethamPerez(1.0, cos_zen_gamma, sun_theta, A_y,B_y,C_y,D_y,E_y);

    float Y = max(Y_z * pY_v / max(pY_z, 1.0e-5), 0.0);
    float x = x_z * px_v / max(px_z, 1.0e-5);
    float y = y_z * py_v / max(py_z, 1.0e-5);

    vec3 XYZ = preethamXyY_to_XYZ(x, y, Y);
    vec3 rgb = preethamXYZ_to_linear_srgb(XYZ);
    return max(rgb, vec3(0.0));
}

// 色温（开尔文）→ 线性 sRGB。黑体辐射到 sRGB 的近似（Tanner Helland 拟合 +
// 白平衡到 ~5600K 中性，再归一化）。用于太阳盘自发光颜色：
//   2000K（日出日落）→ 橙红；5600K（正午）→ 接近中性白。
vec3 colorTempToLinear(float kelvin) {
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    vec3 c;
    // 红通道
    if (t <= 66.0) c.r = 1.0;
    else c.r = clamp(1.29293618606 * pow(t - 60.0, -0.1332047592), 0.0, 1.0);
    // 绿通道
    if (t <= 66.0)
        c.g = clamp(0.3900815787697696 * log(t) - 0.6318414437886277, 0.0, 1.0);
    else
        c.g = clamp(1.1298908608951798 * pow(t - 60.0, -0.0755148492), 0.0, 1.0);
    // 蓝通道
    if (t >= 66.0) c.b = 1.0;
    else if (t <= 19.0) c.b = 0.0;
    else c.b = clamp(0.543206789110196 * log(t - 10.0) - 1.19625408914, 0.0, 1.0);
    return c;
}

// ===== 夜空底色：气辉的垂直渐变（van Rhijn 增亮）=====
//
// 夜空底色由两个颜色给出（uNightZenith / uNightHorizon）。它们之间的插值
// **形状不是自由参数**：气辉发在 ~90 km 的薄层里，视线越贴地平、穿过的发
// 光层越厚 → 地面看到的柱亮度越高。这个纯几何关系就是 van Rhijn 函数：
//
//     I(θ)/I(天顶) = 1 / sqrt(1 − (R/(R+h))² · sin²θ)
//
// 代入 R=6371 km、h=90 km，地平线处因子 ≈ 6.0（几何上限；真实观测常在 2~3×，
// 因为消光把它压回来一部分）。这里把它归一化到 [0,1]（天顶=0、地平=1）作为
// 两色的插值参数 t —— 于是：两个颜色定端点，形状由物理定，**不需要第三个
// "渐变陡不陡"的旋钮**；两个颜色给相同值即退化为纯色底色。
//
// 方位无关：气辉是自发光层（外加城市光污染的近似均匀散射），不是被太阳/
// 月亮照亮的散射体，所以 t 只看仰角，不看水平方向。
const float kAirglowLayerKm = 90.0;
const float kEarthRadiusKm  = 6371.0;

float airglowVanRhijn(float cos_zenith) {
    const float ratio = kEarthRadiusKm / (kEarthRadiusKm + kAirglowLayerKm);
    const float r2 = ratio * ratio;
    return 1.0 / sqrt(max(1.0 - r2 * (1.0 - cos_zenith * cos_zenith), 1.0e-6));
}

// 归一化插值参数：天顶（cos=1）→ 0，地平（cos=0）→ 1。
// 注：此处不能用 const 局部变量——本工程 shader 的 GLSL 版本下，const 局部变量的
// 初始化必须是编译期常量表达式，而 airglowVanRhijn() 是函数调用（非 constexpr）。
float nightGradientT(float cos_zenith) {
    float at_horizon = airglowVanRhijn(0.0);   // ≈ 6.01
    return (airglowVanRhijn(cos_zenith) - 1.0) / (at_horizon - 1.0);
}

vec3 nightSkyColor(float cos_zenith) {
    return mix(uNightZenith, uNightHorizon, nightGradientT(cos_zenith));
}

// ===== 天体发光盘（日盘与月盘共用）=====
//
// 日盘与月盘走**同一套推导链**：俯仰角重映射 → 黑体色温 → Beer-Lambert 衰减 →
// 角度空间盘 mask → 高斯光晕。差别只在“数值 + 色温来源”，故全部抽为共用函数——
// 避免两份拷贝各自漂移（尤其压盘角那套三角函数，写两遍极易分叉）。

// 俯仰角重映射：把低仰角的盘提前“压进地平线”。真实仰角 ≥ start 时盘在真实位置；
// 低于 start 时盘俯仰角(°) = start + ratio × (真实仰角° − start)（>1 = 盘降得更快）。
// 只改盘的**俯仰**，保留原水平方位（盘绕天顶落到另一仰角，而非绕天体方向）。
// 传入真实方向单位向量与真实仰角（弧度）；返回重映射后的盘方向单位向量。
vec3 discDirRemap(vec3 body_dir, float body_elev, float start_angle_deg,
                  float angle_ratio) {
    const float rad2deg = 180.0 / PI;
    const float deg2rad = PI / 180.0;
    float elev_deg = body_elev * rad2deg;
    if (elev_deg < start_angle_deg) {
        elev_deg = start_angle_deg + angle_ratio * (elev_deg - start_angle_deg);
    }
    float disc_elev = elev_deg * deg2rad;
    float sy = sin(disc_elev);
    float sh = sqrt(max(1.0 - sy * sy, 0.0));   // 水平分量长度
    // 保持原水平方位（x,z）方向不变，只改俯仰（y）：
    // 水平单位向量 = (x0,z0)/|水平|，乘 sh 得新的水平分量。
    float horiz_len = length(body_dir.xz);
    if (horiz_len > 1.0e-6) {
        return vec3(body_dir.x / horiz_len * sh, sy, body_dir.z / horiz_len * sh);
    }
    // 原方向几乎垂直向上/下（无水平分量）：退化为只留 y。
    return vec3(0.0, sy, 0.0);
}

// Beer-Lambert 大气衰减（天体辉度穿大气）：AM ≈ 1/sin(仰角)（低角度穿更多大气），
// tau 从浊度近似（简化光学厚度）。返回衰减系数 ∈ (0,1]。
float beerLambertAttenuation(float body_elev, float turbidity) {
    float sin_elev = max(sin(body_elev), 0.05);
    float AM = 1.0 / sin_elev;
    float tau = 0.1 + 0.1 * turbidity;   // 简化光学厚度
    return exp(-tau * AM);
}

// 角盘 mask：在**角度空间**做 smoothstep（而非 cos 空间）。
// 经典写法 smoothstep(cosSA, 1.0, cos_theta) 在 cosSA 接近 1（小盘）时失效：
// cos 在 θ→0 处斜率→0，cos 空间分辨率极不均匀，过渡被挤成 1 像素硬边。
// 正确：先算角距离 ang=acos(dot)，再在角度空间 smoothstep，过渡覆盖整个盘半径。
// 同时返回角距离 ang（光晕需要在它基础上算距盘边缘的角距）。
float discMaskAndAngle(vec3 view_dir, vec3 disc_dir, float disc_radius,
                       out float ang) {
    float cos_ang = dot(view_dir, disc_dir);
    ang = acos(clamp(cos_ang, -1.0, 1.0));   // 到盘中心的角距离（弧度）
    return 1.0 - smoothstep(0.0, disc_radius, ang);
}

// 高斯光晕（盖锯齿 + 大气辉光感）：glow = strength × exp(−d²/2σ²)，σ ∝ 盘半径。
// 关键：光晕是大气散射，物理强度远低于盘，故 strength 是**独立的绝对 HDR 强度**
//（量级 ~几），不与盘亮度同源。d = 距盘边缘的角距离（盘内为 0）。
// glow_width_scale：光晕宽度相对盘半径的倍数（日盘固定 2.0；月盘随浊度变宽，见下）。
float discGlow(float ang, float disc_radius, float strength, float glow_width_scale) {
    float glow_sigma = disc_radius * glow_width_scale;
    float d = max(ang - disc_radius, 0.0);
    return strength * exp(-(d * d) / (2.0 * glow_sigma * glow_sigma));
}

void main() {
    // gl_FragCoord → NDC：frag 坐标在 [0,w]×[0,h]，NDC 在 [-1,1]。
    //（不用 VS 的 vNDC varying —— 无 VAO 全屏三角形上插值不可靠）
    vec2 ndc_xy = (gl_FragCoord.xy / uResolution) * 2.0 - 1.0;
    // ndc → 逆 VP → 世界 far 平面一点；视线方向 = 世界点 - 相机位置。
    vec4 wp = uInvVP * vec4(ndc_xy, 1.0, 1.0);
    vec3 dir = normalize(wp.xyz / wp.w - uCamPos);

    float sun_y = normalize(uSunDir).y;
    // 昼夜过渡：0=夜 1=日（太阳仰角连续）。
    float daylight = clamp((sun_y - 0.03) / 0.10, 0.0, 1.0);

    // ── 太阳盘（自发光天体）：角盘 + 色温推导色 + Beer-Lambert 衰减 ──
    // 参数：uSunRadius（角半径）+ uSunBrightness（亮度基数）+ uSunGlow（光晕）由用户给；
    // 颜色色温（2000K 日出→5600K 正午）与随仰角衰减（Beer-Lambert）在此推导。
    // uSunRadius ≤ 0 或 uSunBrightness ≤ 0 时不画太阳盘。
    vec3 sun_dir = normalize(uSunDir);
    float sun_elev = asin(clamp(sun_dir.y, -1.0, 1.0));   // 仰角（弧度）

    // 日盘俯仰角重映射（只改日盘位置，不碰天空散射/昼夜/色温/衰减）——
    // 真实仰角 < uSunSetStartAngle 时压盘，使低仰角时盘提前没入地平线（0° 时半拉盘
    // 不再露出），高仰角（尤为正午 90°）盘仍钉在真实位置（≥ 阈值处连续，无突变）。
    // 仅用于盘的角距余弦；其它推导（散射/色温/Beer-Lambert 衰减）仍用真实 sun_dir。
    vec3 sun_disc_dir = discDirRemap(sun_dir, sun_elev, uSunSetStartAngle,
                                     uSunSetAngleRatio);

    // 太阳盘色温（开尔文）：仰角越高色温越高（正午白~5600K，日出日落红~2000K）。
    float sun_temp = mix(2000.0, 5600.0, clamp(sun_elev / 0.3, 0.0, 1.0));
    vec3 sun_body_color = colorTempToLinear(sun_temp);   // 黑体色，线性 RGB

    // HDR 亮度：sun_brightness × Beer-Lambert 衰减（只有太阳在地平线上时才画）。
    float sun_brightness = uSunBrightness * beerLambertAttenuation(sun_elev, uTurbidity);

    // 日盘 mask + 光晕（共用函数）。
    float sun_ang = 0.0;
    float disk = discMaskAndAngle(dir, sun_disc_dir, uSunRadius, sun_ang);

    // sun_brightness ≤ 0 时不画（disk 为 0）；否则叠加自发光盘。
    float sun_enable = (uSunBrightness > 0.0 && uSunRadius > 0.0) ? 1.0 : 0.0;
    vec3 sun_contrib = sun_body_color * (sun_brightness * disk * sun_enable);

    // 太阳光晕：σ = 2×盘半径（日盘固定宽度，不随浊度变）。
    float sun_glow = (uSunGlow > 0.0 && sun_enable > 0.0)
        ? discGlow(sun_ang, uSunRadius, uSunGlow, 2.0) : 0.0;
    vec3 sun_glow_contrib = sun_body_color * (sun_glow * sun_enable);

    // ── 月亮盘（自发光天体）：与日盘同一套推导链，只是参数/色温按月亮调 ──
    // 方向 uMoonDir 是**独立输入**（日月反向只是调用方的摆放选择，不是接口契约）。
    // uMoonRadius ≤ 0 或 uMoonBrightness ≤ 0 时不画（默认亮度 0 = 关闭，零回归）。
    vec3 moon_dir = normalize(uMoonDir);
    float moon_elev = asin(clamp(moon_dir.y, -1.0, 1.0));

    // 月盘俯仰角重映射：与日盘同义、同一套默认值（10°/1.4），使 0° 时整盘淹没在地平线下。
    vec3 moon_disc_dir = discDirRemap(moon_dir, moon_elev, uMoonSetStartAngle,
                                      uMoonSetAngleRatio);

    // 月亮色温：月亮不发光，是反射日光，实际偏冷白/淡黄（~4100K），且**不随仰角变红**
    // （复现日盘的仰角色温曲线会把月亮染成落日般的橙红，不符合直觉）。故取常量色温。
    vec3 moon_body_color = colorTempToLinear(4100.0);

    // HDR 亮度：moon_brightness × Beer-Lambert 衰减（与日盘同一公式，含浊度）。
    float moon_brightness = uMoonBrightness * beerLambertAttenuation(moon_elev, uTurbidity);

    float moon_ang = 0.0;
    float moon_disk = discMaskAndAngle(dir, moon_disc_dir, uMoonRadius, moon_ang);
    float moon_enable = (uMoonBrightness > 0.0 && uMoonRadius > 0.0) ? 1.0 : 0.0;
    vec3 moon_contrib = moon_body_color * (moon_brightness * moon_disk * moon_enable);

    // ── 月晕（本 PR 重点：**宽度受 turbidity 影响**）──
    // 物理依据：月晕/华是月光经大气中水汽/气溶胶的散射（日晕同理）。浊度越大（水汽/
    // 气溶胶越多），散射越强 → 晕越**宽、越散**。故把光晕宽度倍数（σ/半径）随浊度线性
    // 增长：turb=2（极清澈）→ 2.0×（同日盘）；turb=8（重霾）→ 4.0×（晕宽一倍）。
    // 同时晕的**峰值强度**不变（moon_glow 只给绝对峰值），保证“晕变宽”而非“变亮”。
    float moon_glow_width = mix(2.0, 4.0, clamp((uTurbidity - 2.0) / 6.0, 0.0, 1.0));
    float moon_glow = (uMoonGlow > 0.0 && moon_enable > 0.0)
        ? discGlow(moon_ang, uMoonRadius, uMoonGlow, moon_glow_width) : 0.0;
    vec3 moon_glow_contrib = moon_body_color * (moon_glow * moon_enable);


    vec3 sky;
    if (dir.y < 0.0) {
        // ── 地平线以下：纯色地色（避免天空倒影感），不采样大气模型 ──
        sky = uGroundColor;
    } else {
        // ── 上半球：Preetham 大气模型算天空色 ──
        sky = preethamSky(dir, sun_dir, uTurbidity) * SKY_LUMINANCE_SCALE;

        // ── 昼夜过渡：太阳低于地平线时，日光淡出直到接近黑（(0,0,0)） ──
        // 太阳落山后白天项→黑，"夜空"由本 shader 末尾的夜色双色加法叠加
        // （night_zenith_color / night_horizon_color，默认 0 = 关闭）。
        sky = mix(vec3(0.0), sky, daylight);

        // ── 叠太阳盘（加法，和天空散射色在同一 HDR 域）──
        // 太阳盘是自发光天体，与天空散射色加法叠加，不受 season 染色（见下）。
        sky += sun_contrib;
        sky += sun_glow_contrib;

        // ── 叠月亮盘（同样加法、同 HDR 域；默认亮度 0 时不贡献任何值）──
        sky += moon_contrib;
        sky += moon_glow_contrib;
    }

    // 季节色温 + 亮度。
    // 注意：这里**不做 tone map**（不再 sky/(1+sky)）——为了后续统一后处理管线，
    // 天空输出保持 HDR 原始亮度（可 >1.0），由最终的后处理 pass 统一压缩到 [0,1]。
    // （2026-08-19：为引入统一后处理，去掉天空 shader 里的前置 Reinhard。）
    //
    // ⚠️ 顺序很重要：夜色项必须加在本行**之后** —— 它**不受 season 染色**
    //   （season 是"日光散射的季节色温"，气辉/城市光污染不是散射日光），
    //   但**受 intensity 缩放**（intensity = 天光总强度开关；日落后白天项已为 0，
    //   于是它在夜间自动成为夜色总开关）。
    sky *= uSeason * uIntensity;

    // ── 夜空底色（加法叠加，仅地平线以上；**按天黑程度 (1−daylight) 淡入**）──
    // 算子：加法，但夜间项乘 (1−daylight) —— 物理上气辉/城市光污染白天被日光完全
    // 淹没，不应给日间天空抬底；日落后 (daylight→0) 淡入到满值。
    //   为何不像白天项那样“只靠加法自然退化”：白天项的 daylight 因子只能把**白天项**
    //   压到 0（日落即无日散射），但反过来不能阻止**夜色项**在白天被加上去；两层的
    //   时间边界需各自门控。用同一个 daylight 因子 ⇒ 与 AmbientColor 的夜色叠加、
    //   与 shader 的昼夜过渡是同一条时间轴（不会一个已入夜另一个还在黄昏）。
    //   注：本行也是 sky *= uSeason * uIntensity 的**之后**，故夜色不受 season 染，
    //   但受 intensity 缩放（intensity = 天光总开关）。
    // 地平线以下**不加**：下半球由 ground_color 独占。
    // (1−daylight) 在 sun_y ≥ 0.13 时为精确 0.0（白天零影响）；
    // 若显式把夜色两色设为 0，本行在任何情况下都是 +0.0。
    if (dir.y >= 0.0) {
        sky += nightSkyColor(clamp(dir.y, 0.0, 1.0)) * uIntensity * (1.0 - daylight);
    }

    FragColor = vec4(sky, 1.0);
}
)glsl";

    // ---- DrawSky ----
    // 在**当前绑定 FBO** 上画全屏程序化天光（背景垫底）。
    //
    // 参数：
    //   sky_cmd:  SkyCommand（程序化参数）。
    //   cam:      当前 Camera（取 position/target/up/fov/近远/fbo 尺寸算逆 VP）。
    //   fbo_w/h:  当前 3D FBO 尺寸。
    //   shader_mgr: 共享资源。
    //
    // GL 状态前置要求（调用方负责）：
    //   - 目标 FBO 已绑定（天光垫底的 3D FBO），viewport 已设置
    //   - 深度测试已禁用（天空永远垫底，不写深度）
    // Pre-condition: cam.up 非零、target != position
    static void DrawSky(const SkyCommand& sky_cmd,
                        const Camera& cam, int fbo_w, int fbo_h,
                        ShaderManager& shader_mgr);
};

}  // namespace jpov

#endif  // JPOV_SKYDOME_SKY_RENDERER_H_
