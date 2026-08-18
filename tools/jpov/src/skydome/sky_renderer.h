// JPOV SkyRenderer — 程序化天光（Preetham 天空 + 日月圆盘）背景渲染
//
// 用解析式大气模型（Preetham-Shirley-Smits 1999）计算纯色天空背景，
// 与 3D 相机视角一致（相机逆 VP 重建视线方向）。非 HDRI：纯程序化、
// 参数少、连续可动画，支持时间(太阳位置)/天气(turbidity)/季节(season)。
//
// 设计要点：
//   - 独立轻量 sky shader（含 Preetham 函数），一帧一次 draw，不掺 object3d。
//   - 先画天球（垫 3D FBO 背景）→ 再画 3D 物体（深度测试覆盖）。
//   - 地平线以下（pitch<0）画纯色 ground_color（避免天空倒影）。
//   - 太阳/月亮：程序化天空上叠一个高斯发光圆盘（可切日月）。
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

    // kSkyFs: Preetham 程序化天空 + 地平线下地色 + 日月圆盘。
    //   NDC 由 gl_FragCoord/uResolution 反推（不用 VS varying：无 VAO 全屏
    //   三角形上 varying 插值不可靠，实测 vNDC 几乎不变导致方向重建错误）。
    static constexpr const char* kSkyFs = R"glsl(
#version 330 core
out vec4 FragColor;

uniform vec2  uResolution;    // 当前 FBO 分辨率（像素），用于 gl_FragCoord→NDC
uniform mat4  uInvVP;         // 相机 逆(Proj*View)
uniform vec3  uCamPos;        // 相机世界位置（方向 = 反投影点 - 相机）
uniform vec3  uSunDir;        // 太阳位置单位向量（世界中，y-up）
uniform float uTurbidity;     // 浊度（天气）：2 清澈…8 霾
uniform vec3  uSeason;        // 季节色温乘子（冬冷/夏暖）
uniform float uIntensity;     // 天光亮度标量
uniform vec3  uGroundColor;   // 地平线以下地色
uniform int   uIsMoon;        // 0=太阳 1=月亮
uniform vec3  uBodyColor;     // 日月颜色
uniform float uBodyRadius;    // 日月角半径（弧度）
uniform float uBodyGlow;      // 光晕模糊度（0=硬边）

const float PI = 3.14159265358979323846;
// Preetham 输出天顶亮度 ~几十到几百 kcd/m²。用亮度缩放把值压回可显示区间；
// 调小让天空稍暗，太阳圆盘才能在对比下清晰浮现（0.05：天顶→~0.2 天蓝）。
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

void main() {
    // gl_FragCoord → NDC：frag 坐标在 [0,w]×[0,h]，NDC 在 [-1,1]。
    //（不用 VS 的 vNDC varying —— 无 VAO 全屏三角形上插值不可靠）
    vec2 ndc_xy = (gl_FragCoord.xy / uResolution) * 2.0 - 1.0;
    // ndc → 逆 VP → 世界 far 平面一点；视线方向 = 世界点 - 相机位置。
    vec4 wp = uInvVP * vec4(ndc_xy, 1.0, 1.0);
    vec3 dir = normalize(wp.xyz / wp.w - uCamPos);

    vec3 sky;
    if (dir.y < 0.0) {
        // 地平线以下：纯色地色（避免天空倒影感），不采样大气模型
        sky = uGroundColor;
    } else {
        // 上半球：Preetham 大气模型算天空色（含晚霞方向非对称），乘亮度缩放
        sky = preethamSky(dir, normalize(uSunDir), uTurbidity) * SKY_LUMINANCE_SCALE;
    }

    // ── 昼夜过渡：太阳低于地平线时，Preetham 本就发散的日光要淡出到夜空 ──
    // 用太阳仰角(sunDir.y)做衰减：太阳越低，日光贡献越弱；完全落山→夜空底色。
    // 夜空底色：深蓝黑渐变（天顶略蓝、地平线深）——手动程序色，避免 Preetham
    // 在 sun<0 时残留的黄色霞带。恒星在天空暗时浮现（见下）。
    float sun_y = normalize(uSunDir).y;
    // 夜空底色：纯深蓝黑，不含暖调
    vec3 night_base = vec3(0.015, 0.02, 0.05);          // 深蓝夜空
    // 日光强度：sun_y=1 时全天光；sun_y<=0.03 时日光衰减到接近 0（衔接夜空）。
    float daylight = clamp((sun_y - 0.03) / 0.10, 0.0, 1.0);  // 0=夜 1=日
    sky = mix(night_base, sky, daylight);

    // 恒星：仅天空暗（夜空）时可见；用视线方向哈希做伪随机星点。
    // 亮度随 daylight 减弱（夜晚才有星）。
    float star = 0.0;
    if (daylight < 0.5) {
        vec2 sp = vec2(dir.x, dir.z) * 60.0;             // 星场尺度
        vec2 cell = floor(sp);
        vec2 f = fract(sp) - 0.5;
        // 每格一个伪随机星（cell 哈希），亮度/半径抖动
        float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
        float st = smoothstep(0.7, 1.0, h);              // 少数格有星
        st *= clamp(1.0 - length(f) / 0.08, 0.0, 1.0);   // 星点小核
        star = st * (0.15 + 0.85 * fract(h * 77.0));     // 亮度抖动
    }
    sky += vec3(star) * (1.0 - daylight);

    // 太阳/月亮发光圆盘：视线方向与太阳方向夹角越小越亮。
    // 用高斯衰减画盘；bodyRadius 控盘大小（角半径弧度），bodyGlow 控制盘边缘
    // 弥散（0=清晰圆，越大越模糊散开）。亮度取 1.2（避免过曝成一团白）。
    float cos_angle = dot(normalize(dir), normalize(uSunDir));
    float angle = acos(clamp(cos_angle, -1.0, 1.0));
    float uv = angle / max(uBodyRadius, 1e-5);
    // 距离归一化：uv=1 在盘缘，uv=0 盘心。盘心亮度 1，盘缘快速衰减。
    // 分母 1+bodyGlow*40：bodyGlow 越大衰减越缓（弥散大），0 时高斯很陡(清晰盘)。
    float glow = exp(-uv * uv * (1.0 + uBodyGlow * 40.0));
    sky += uBodyColor * glow * (uIsMoon == 1 ? 0.3 : 1.2);

    // 季节色温 + 亮度；图像输出需 tone map（天空亮度可达 O(1)~O(10)，压缩防过曝）
    sky *= uSeason * uIntensity;
    sky = sky / (1.0 + sky);   // Reinhard tone map 到 [0,1]

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
