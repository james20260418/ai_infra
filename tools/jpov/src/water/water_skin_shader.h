// JPOV 水皮（water skin）pass —— GLSL
//
// 语义：世界 `y = level_y` 的基准面上，叠加**波浪高度场 h(x,z,t)**（可为零）。
//   · 无波浪（uWaveCount = 0）⇒ 纯平面，逐字节退化为旧版；
//   · 有波浪 ⇒ 求交改为「平面初值 + Newton」，法线取高度场解析梯度。
// 只画「界面」这一层，**不管水体介质**（介质的吸收/注入由合成趟的 MediumAt 负责）。
//
// ⭐ 为什么是「全屏三角形 + 解析求交」而不是一块网格/quad（2026-10-28 已验证，别重开）：
//   视线越接近水平，与平面的交点距离 → ∞（眼高 1.5 m 时地平线在 4.4 km）。
//   任何有限 quad 都会在「quad 远端边界 ↔ 真地平线」之间留一条缝，缝里是天球下半球的
//   ground 色 —— 看得到一片"陆地"。几何上无解 ⇒ 只能逐像素解交点。
//   波浪只把「平面解析解」换成「高度场 Newton 解」，屏幕空间求交这条路径不变。
//   副产品：没有「水面尺寸」这个参数（接口里也就不给）。
//
// ⭐ 输出 = **一层**（WBO 覆盖集，见 src/wbo/wbo_target.h 与 docs/jpov_water_contract.md §2）：
//     att0 accum  = vec4(C, α)   ← (ONE, ONE) 累加：C 是**预乘色**、α 是遮挡率
//     att1 reveal = α            ← (ZERO, ONE_MINUS_SRC_COLOR) ⇒ 单层后 = 1 − α
//     att3 depth  = t · α        ← **射线距离**（不是视轴深度！）
//   ⚠️ reveal 槽要写 **α 本身**：WBO 的 revealage 混合是 dst' = dst·(1 − src)，
//      清成 1 ⇒ 单层后纹理里就是 1 − α。写反会得到“反相菲涅尔”。
//   ⚠️ att3 必须是**射线距离 t**：合成趟的 `FieldAt(ro, rd, z)` 与 `z_op` 都以射线参数为准；
//      写视轴深度会让层被放早 1/cosθ（画面角落 ~1.5×），且贴水面那段水体的注入被吃掉。
//
// ⭐ 层的两个分支（契约 §2.2 / §2.3）
//   水下（相机在水面以下）：
//     · 环外（θi > 临界角）Fresnel 返回 1 ⇒ α = 1，整个背景被盖掉；
//     · 环内 α = F（2%~6%），背景（= 已经过天空折射的天球）透出；
//     · C = F · S_P(∞) —— 「深海积分反射」：从 P 沿镜像方向到**无穷远**的介质积分。
//   水上（相机在水面以上）：
//     · C = F · sky(mirror)（探针；将来接 SSR 时换成 A·SSR + (1−A)·sky）；
//     · α = 1 − (1−F)/n² —— **÷n² 折进透射率**：水下内容透过界面观察要按 L/n² 守恒变暗。
//
// ⚠️ 波浪（2026-11-18 起）只改**界面几何**：求交点、法线、菲涅尔、反射方向、日盘像。
//    水体介质仍按 `level_y` 的水平半空间积分（波浪化介质见 known_issue §11.5，另 PR）。
//
// ─────────────────────────────────────────────────────────────────────────────
// 结构（2026-11-25 重构：拆成「共享 prelude + 水上/水下着色 + 调试 + 4 个 main」）──
//   1. kWaterSkinVs            全屏三角形 VS（3 顶点，无 VBO）
//   2. kWaterSkinCommonGlsl    公共：uniform + 辅助函数 + WaterSurface 结构体 +
//                              ResolveSurface（求交/求导，**永不 discard**）+
//                              RejectOccluded（唯一 discard 点）+ EmitWBO
//   3. kWaterSkinAboveGlsl     水上着色：ShadeAbove（反射 + 太阳 GGX + F + α）
//   4. kWaterSkinBelowGlsl     水下着色：ShadeBelow（深海积分反射 + F + α）
//   5. kWaterSkinDebugGlsl     调试：uWaveDebug + EmitDebug（生产 program 不含它）
//   6. kWaterSkin*MainGlsl     ×4 个 main（水上/水下 × 生产/调试）
//
//   为什么拆：①「导数必须先于 discard」这条不变式从注释纪律变成结构约束（ResolveSurface
//   不含 discard）；②水上 program 不拼介质、水下 program 不拼 GGX/太阳；③调试色退出生产
//   program，不再脏写 WBO 槽。逐像素行为与拆前**完全一致**（gold + gpu_test 兜底）。
//
//   拼装顺序（见 water_skin_renderer.cc::Init）：
//     水上生产：#version + kWaterInterfaceUniformsGlsl + kWaterWaveGlsl + kGgxSpecularGlsl
//               + kWaterSkinCommonGlsl + kWaterSkinAboveGlsl + kWaterSkinAboveMainGlsl
//     水下生产：#version + kWaterInterfaceUniformsGlsl + kWaterMediumUniformsGlsl + kWaterMediumGlsl
//               + kWaterWaveGlsl + kWaterSkinCommonGlsl + kWaterSkinBelowGlsl + kWaterSkinBelowMainGlsl
//     水上/水下调试：对应生产 + kWaterSkinDebugGlsl + 对应 debug main。

#ifndef JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_SHADER_H_

namespace jpov {
namespace water {

// 全屏三角形（3 顶点，无 VBO）。vNdc 在屏幕内插值出 [-1,1] 的 NDC 坐标。
// 顶点取 (-1,-1) (3,-1) (-1,3)：覆盖整个视口，且不会在 llvmpipe 上触发
// 「超大三角形 varying 插值退化」（本项目天空/雾都因此改成全屏 + 反投影）。
inline constexpr const char* kWaterSkinVs = R"glsl(
#version 330 core
out vec2 vNdc;
void main() {
    vec2 c = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    vNdc = c;
    gl_Position = vec4(c, 0.0, 1.0);
}
)glsl";

// ───────────────────────── 公共：uniform + 辅助 + 结构体 + 求交 + 拒绝 + 发射 ─────────────────────────
//
// 相机基约定（与 Primitives3DRenderer::BuildMVP 的 lookAt 一致）：
//   fwd = normalize(target − position)
//   right = normalize(cross(fwd, up))
//   up' = cross(right, fwd)            ← 必须正交化，否则与投影矩阵的 img 不一致
//   rd = normalize(fwd + right·(ndc.x·tanHalfFov·aspect) + up'·(ndc.y·tanHalfFov))
//
// 介质部分（`MediumAt` 与那组 uWater* uniform）由 kWaterMediumGlsl 提供；
// 波浪部分（`WaveSampleAt` / `WaveSurfaceHit` 与 uWave* uniform）由 kWaterWaveGlsl 提供：
// 本 pass 只**调用**它们，不重复实现物理。
// ⚠️ 本片段**不含 `#version`**：使用方要在最前面拼 `#version 330 core\n`，再按上面的
//    拼装顺序拼各片段。uWaterLevelY / uWaterIor 由 kWaterInterfaceUniformsGlsl 提供。
inline constexpr const char* kWaterSkinCommonGlsl = R"glsl(
in vec2 vNdc;

layout(location = 0) out vec4  oAccum;    // (C, α)：C 为预乘色
layout(location = 1) out float oReveal;   // α 本身
layout(location = 3) out float oDepth;    // t · α（射线距离）

uniform vec2  uViewport;     // 3D FBO 尺寸（把 gl_FragCoord 换成 uv 采场景深度）
uniform vec3  uCamPos;       // 相机世界位置
uniform vec3  uCamFwd;       // 视线方向（单位）
uniform vec3  uCamRight;     // 右（单位，已正交化）
uniform vec3  uCamUp;        // 上（单位，已正交化）
uniform float uTanHalfFov;   // tan(fov/2)
uniform float uAspect;       // 宽/高
uniform float uNear;
uniform float uFar;
uniform float uDepthBiasM;       // 水面与场景深度的比较容差（米）
uniform sampler2D   uSceneDepth; // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）

// ── 太阳的像（解析 GGX，盘状光源）──
// 由 Renderer 从 cmds.sun 填。与介质那份 uWaterSun* 分开：介质可以关，水面依然有日盘像。
// ⚠️ uSunToward 必须在公共里（ResolveSurface 用它算半向量 H），其余太阳项只被水上着色消费。
uniform int   uHasSun;            // 0 = 无太阳 ⇒ 这一项恒 0
uniform vec3  uSunToward;         // 指向太阳（气侧，单位）
uniform vec3  uSunIrradiance;     // color × intensity（辐照度语义）
uniform float uSunAngularRadius;  // 盘角半径（弧度，> 0）
uniform float uSunFootprint;      // 像素足迹带宽开关（1 = 开；GPU 对拍测试置 0）

const float kBackgroundDepth = 0.999999;

// 无偏振电介质菲涅尔（含全内反射）。与 water_optics.h 的 FresnelDielectric 同源。
float FresnelDielectric(float cos_i, float n1, float n2) {
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

// NDC 深度 → 视轴深度（米）。与 water_skin_math.h 的 LinearizeDepthNdc 同源。
float LinearizeDepthNdc(float ndc_z, float near_plane, float far_plane) {
    float ndc = ndc_z * 2.0 - 1.0;
    return 2.0 * near_plane * far_plane / (far_plane + near_plane - ndc * (far_plane - near_plane));
}

// ── 一次求交/求导的全部中间量 ──
// ResolveSurface 填满它；着色函数（ShadeAbove/ShadeBelow）只读它，不再各自重算。
struct WaterSurface {
    bool  hit;            // 射线打中水面（t > 0）
    bool  wave_ok;        // 波浪求交成功（无波浪恒 true）
    float t;              // 交点射线参数（欧氏，波浪时 = Newton 解）
    float t_plane;        // 平面交点（深度测试用，波浪无关）
    vec3  N;              // 法线（波浪扰动或平面）
    vec3  rd;             // 视线方向（单位）
    vec3  H;              // 半向量（太阳像用，① 无条件先算）
    float cos_i;          // |dot(rd, N)|
    float footprint_a2;   // 太阳像足迹带宽（① dFdx/dFdy）
    float normal_var_px;  // 像素内法线方差（③b，specular AA）
    float var_p;          // 入射面投影的斜率方差（探针 LOD）
    float var_p_mean;     // 各向同性平均（F_eff 用）
};

// 着色输出（生产只用 C/α；调试读 fresnel_debug/env_lod_debug/refl_debug，水下恒 0）。
// ⚠️ 放公共里：ShadeAbove / ShadeBelow 都产出它，EmitDebug 读它。
struct WaterShade {
    vec3  C;               // 层色（P 点辐射度，未经相机段衰减）
    float alpha;           // 层遮挡率
    float fresnel_debug;   // 调试用（uWaveDebug=13）：水上=F，水下=0
    float env_lod_debug;   // 调试用（uWaveDebug=11）：水上=env_lod，水下=0
    vec3  refl_debug;      // 调试用（uWaveDebug=12）：水上=refl，水下=0
};

// 求交 + 求导 + 斜率统计。**永不 discard**（discard 一律在 RejectOccluded），
// 这样 dFdx/dFdy 必然在一致的控制流里求值 —— 历史上反复追的「地平线裂隙/暗带」的机器
// 就是「导数之前 discard 杀 helper invocation ⇒ 导数垃圾」。谁想插 discard 都插不进这里。
void ResolveSurface(out WaterSurface s) {
    s.rd = normalize(uCamFwd
                     + uCamRight * (vNdc.x * uTanHalfFov * uAspect)
                     + uCamUp    * (vNdc.y * uTanHalfFov));

    // ── ① 太阳像的半向量与像素足迹：**必须无条件先算** ──
    //    半向量 H 与法线无关 ⇒ 可以在求交（可能 discard）之前算完；NoV/NoL/NoH 之后用 N 算。
    vec3 Vw = -s.rd;                       // P → 相机
    s.H = Vw + uSunToward;                 // 半向量（未归一）
    float hlen = length(s.H);
    s.H = (hlen > 1.0e-6) ? (s.H / hlen) : vec3(0.0, 1.0, 0.0);
    vec3 dHdx = dFdx(s.H);
    vec3 dHdy = dFdy(s.H);
    // 像素足迹并入斜率方差（specular AA 口径，docs/jpov_specular_bandwidth.md §1.3③）。
    s.footprint_a2 = 0.25 * (dot(dHdx, dHdx) + dot(dHdy, dHdy));

    // ── ③ 视线 ∩ 水面（无波浪 ⇒ 平面解析解；有波浪 ⇒ 二分求交）──
    float t_raw = (uWaterLevelY - uCamPos.y) / s.rd.y;
    s.hit = (t_raw > 0.0);
    s.t = max(t_raw, 1.0e-3);   // 不可见的像素用安全值，只为让导数有定义
    s.t_plane = s.t;

    // ── ③b 法线 + 像素内法线方差（specular AA 的唯一正确来源）──
    s.N = vec3(0.0, 1.0, 0.0);
    s.wave_ok = true;
    float pixel_angle = 2.0 * uTanHalfFov / uViewport.y;
    float footprint = WaveFootprint(uCamPos, s.rd, uWaterLevelY, pixel_angle);
    if (uWaveCount > 0) {
        vec3 pos_w = vec3(0.0);   // WaveSurfaceHit 的 out 参数；交点由 s.t 表达，这里不读
        vec3 nrm_w = vec3(0.0, 1.0, 0.0);
        float t_wave = s.t;
        s.wave_ok = WaveSurfaceHit(uCamPos, s.rd, uWaterLevelY, footprint,
                                   t_wave, pos_w, nrm_w);
        s.t = s.wave_ok ? t_wave : s.t;
        s.N = s.wave_ok ? nrm_w : vec3(0.0, 1.0, 0.0);
    }
    // **只用横向差分**（设计选择，不是权宜）：水面/天空的边界是**水平**的，那一行的 2×2 quad
    // 里上半是天空（hit=false，N 没有意义）⇒ dFdy(N) 会把方差顶到天上；横向邻居始终是水面
    // ⇒ dFdx 安全。各向同性下 E|dN/dx|² = E|dN/dy|²，所以 0.5·|dFdx|² 与 0.25·(|dx|²+|dy|²) 期望相同。
    vec3 dNdx = dFdx(s.N);
    s.normal_var_px = 0.5 * dot(dNdx, dNdx);

    // ── ③d 法线场过滤：把「看不见的波」折算成粗糙度 ─────────────────────────
    float sxx, sxz, szz;
    WaveSlopeTensor(footprint, sxx, sxz, szz);
    // 两个投影，各有各的消费者（2026-11-24 修：以前只有一个"视线投影"被三个消费者共用）：
    //   · var_p_slope：入射面方向 —— 反射方向在该平面内摆动，所以探针模糊（env_lod）用它；
    //   · var_p_mean：张量各向同性的平均 —— 反射率 R_eff 是对半球积分，不能用某一个方向的投影。
    vec2 td = s.rd.xz;
    float tdl = length(td);
    td = (tdl > 1.0e-6) ? (td / tdl) : vec2(1.0, 0.0);
    s.var_p = td.x * td.x * sxx + 2.0 * td.x * td.y * sxz + td.y * td.y * szz;
    s.var_p_mean = 0.5 * (sxx + szz);

    s.cos_i = abs(dot(s.rd, s.N));
}

// 唯一 discard 点：命中/求交失败 + 场景深度拒绝。
// skip_depth = true ⇒ 跳过深度拒绝（调试 uWaveDebug==3 的 A/B 对照）。
void RejectOccluded(WaterSurface s, bool skip_depth) {
    if (!s.hit) {
        discard;   // 交点在相机后方
    }
    if (!s.wave_ok) {
        discard;   // 求交失败 / 交点跑到相机后方
    }
    if (skip_depth) {
        return;
    }
    // 用**平面**交点距离（不用波浪求交的 t）：后者在掠射时相邻像素能差几公里 ⇒ view_z 抖动。
    float view_z = s.t_plane * dot(s.rd, uCamFwd);
    float scene_ndc = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
    if (scene_ndc < kBackgroundDepth) {
        float scene_z = LinearizeDepthNdc(scene_ndc, uNear, uFar);
        if (view_z > scene_z + uDepthBiasM) {
            discard;   // 不透明场景挡在水面前
        }
    }
}

// WBO 发射（生产程序唯一出口）。
void EmitWBO(vec3 C, float alpha, float t) {
    oAccum  = vec4(C, alpha);
    oReveal = alpha;
    oDepth  = t * alpha;
}
)glsl";

// ───────────────────────── 水上着色 ─────────────────────────
// 反射 + 太阳的像 + F + 透射率。需要 kGgxSpecularGlsl（太阳 GGX）与 uEnv*（探针）。
inline constexpr const char* kWaterSkinAboveGlsl = R"glsl(
uniform float uEnvScale;     // 天光反射消费量 [0,1]（= EnvSpecular::sky_scale）
uniform float uHorizonClampSin;  // 反射方向夹到地平线以上的最小 y（= sin 1°）
uniform samplerCube uEnvCube;    // 天光探针（大气色，不含日月盘）

// 粗糙界面的**反射方向反照率**（角度平均菲涅尔）：
//   R_eff(μo, α) = ∫ f_ss(ωo,ωi)·F(ωo·h)·cosθi dωi
// 掠射时远低于平面菲涅尔：2° 仰角下 0.80 → 0.34（σ²≈0.017）/0.24（0.05）/0.15（0.15）。
// 差额就是进了透射的那部分（T_eff = 1 − R_eff）→ 海面因此不会“水天一色”。
//
// ⚠️ **输入是斜率方差 σ²，不是 a2**（2026-11-24 修，与 water_optics.h 同源）：
//    Beckmann ↔ GGX 的标准映射是 α² = 2σ²（GGX 自身方差 = α²/2）；旧版把 σ² 当 a2 用
//    ⇒ 粗糙度少一半 ⇒ 远海反射率偏镜面/偏亮。
// 只被水上着色消费（水下的 F 是平面 FresnelDielectric），所以放水上片段。
float RoughFresnelAlbedo(float cos_i, float slope_var, float ior) {
    float a2 = 2.0 * max(slope_var, 0.0);
    float s = sqrt(a2);
    float shifted = FresnelDielectric(clamp(cos_i + 0.52 * pow(max(s, 1.0e-6), 0.72),
                                           1.0e-4, 0.999), 1.0, ior);
    float plain = FresnelDielectric(clamp(cos_i, 1.0e-4, 0.999), 1.0, ior);
    float w = smoothstep(0.05, 0.5, cos_i);
    return mix(shifted, plain, w);
}

void ShadeAbove(WaterSurface s, out WaterShade o) {
    // ── ⑤ 角度平均菲涅尔（粗糙界面的方向反照率）──
    float F = RoughFresnelAlbedo(s.cos_i, s.var_p_mean, uWaterIor);

    // ── ⑥ 太阳的像（解析 GGX，盘状光源）—— 只做乘除，无导数 ⇒ 可在 discard 之后 ──
    vec3 sun_spec = vec3(0.0);
    float NoV = dot(-s.rd, s.N);
    if (uHasSun != 0) {
        float NoL = dot(uSunToward, s.N);
        if (NoV > 0.0 && NoL > 0.0) {
            float NoH = max(dot(s.H, s.N), 0.0);
            // 带宽 = 盘角半径（θ/2，半向量只偏一半）+ 像素足迹 + 像素内法线方差。
            // 上限 0.25（⇒ α ≤ 0.5，半角约 26°）是**数值兜底**：极端掠射时 dFdx 会把足迹
            // 方差推出数量级，不夹会把日斑摊成一大片糊。
            // ⚠️ **不再叠加解析 σ_slope**（2026-11-18 定）：波浪法线是逐像素解析梯度，
            //    再把 σ_slope 当带宽加进去 = 双计 + 抹平闪光（known_issue §11）。
            // 方差项 → α² 也要 ×2（α² = 2σ²，与 RoughFresnelAlbedo 同一口径）。
            float a2 = min(GgxDiskBandwidthA2(uSunAngularRadius)
                           + uSunFootprint * s.footprint_a2
                           + 2.0 * s.normal_var_px,
                           0.25);
            float D = GgxDistribution(NoH, a2);
            float G = GgxSmithG1(NoV, a2) * GgxSmithG1(NoL, a2);
            float Fw = F;
            sun_spec = uSunIrradiance * (Fw * D * G / max(4.0 * NoV * NoL, 1.0e-6)) * NoL;
        }
    }

    // ── 水面以上：天光探针沿镜像方向（方向必须夹到地平线以上 —— 探针下半球是“地色”）──
    vec3 rdir = reflect(s.rd, s.N);
    float xz = sqrt(rdir.x * rdir.x + rdir.z * rdir.z);
    if (rdir.y < uHorizonClampSin) {
        if (xz > 1.0e-6) {
            float scale = sqrt(1.0 - uHorizonClampSin * uHorizonClampSin) / xz;
            rdir = vec3(rdir.x * scale, uHorizonClampSin, rdir.z * scale);
        } else {
            rdir = vec3(0.0, 1.0, 0.0);
        }
    } else {
        rdir = normalize(rdir);
    }
    // ── 探针 LOD（拉糊）：把「像素足迹内**未解析**的斜率方差」折成模糊角 ──
    // 两个来源相加（方差可加）：var_p（谱侧被截断的波 + 短波 MSS）+ normal_var_px（屏幕侧）。
    // 镜面反射把斜率放大 2× ⇒ 需要的模糊角 ≈ 2σ；128² 探针的 texel 角 = (π/2)/128 · 2^lod。
    // 上限随反射方向仰角放开：探针下半球是地色，地平线附近糊大了会把地色卷进来。
    const float kEnvTexelAngle = 1.5707963268 / 128.0;   // 常量表达式（GLSL const 限制）
    float refl_sigma = sqrt(max(s.var_p, 0.0) + max(s.normal_var_px, 0.0));
    float env_lod_need = log2(max(2.0 * refl_sigma, 1.0e-6) / kEnvTexelAngle);
    float env_lod_cap = mix(1.5, 3.0, smoothstep(0.05, 0.35, rdir.y));
    float env_lod = clamp(env_lod_need, 0.0, env_lod_cap);
    vec3 refl = textureLod(uEnvCube, rdir, env_lod).rgb * uEnvScale;

    // 环境反射 + 太阳的像。
    // ⚠️ 太阳项是**加法**：探针不含日月盘（kEnvProbe），SSR 也不负责它。
    o.C = F * refl + sun_spec;
    // 透射率 = (1−F)/n²（辐射度跨介质守恒）；WBO 的 α 是“背景衰减率”= 1 − T。
    o.alpha = 1.0 - (1.0 - F) / (uWaterIor * uWaterIor);
    o.fresnel_debug = F;
    o.env_lod_debug = env_lod;
    o.refl_debug = refl;
}
)glsl";

// ───────────────────────── 水下着色 ─────────────────────────
// 深海积分反射（MediumAt 到无穷远）+ F + α。需要 kWaterMediumGlsl（MediumAt）。
inline constexpr const char* kWaterSkinBelowGlsl = R"glsl(
// “无穷远”的射线参数（均质介质下 τ 早已饱和）。
const float kDeepSeaRayT = 1.0e6;

void ShadeBelow(WaterSurface s, out WaterShade o) {
    // ── ⑤ 水→空气菲涅尔（含全内反射）──
    float F = FresnelDielectric(s.cos_i, uWaterIor, 1.0);

    // 湍海积分反射：从 P 沿镜像方向到无穷远的介质积分（同一个 MediumAt，z→∞）。
    // ⚠️ P 往水面下推 1 mm：MediumAt 用 ro.y 判“从空气进水/本来就在水里”，
    //    落在平面上时是浮点不确定的（会影响折射后的 dy_w 与 B）。
    vec3 P = uCamPos + s.rd * s.t;
    P.y -= 1.0e-3;
    vec3 mirror = reflect(s.rd, s.N);
    // ⚠️ 波浪把法线倾斜到让镜像方向**朝上**时，半空间介质会立刻"出水" ⇒ S=0 ⇒ 环内纯黑。
    //    把镜像方向钳到水面以下（极小负 y），那片区域回到正常的水体自发光。
    if (mirror.y > -1.0e-3) {
        float xz = length(mirror.xz);
        mirror = (xz > 1.0e-6)
                     ? vec3(mirror.x * (1.0 / max(xz, 1.0e-6)), -1.0e-3,
                            mirror.z * (1.0 / max(xz, 1.0e-6)))
                     : vec3(0.0, -1.0, 0.0);
        mirror = normalize(mirror);
    }
    vec3 tau_deep;   // MediumAt 的 out 参数：深海反射只用 S，不需要 τ
    vec3 S_deep;
    MediumAt(P, mirror, kDeepSeaRayT, tau_deep, S_deep);
    o.C = F * S_deep;
    o.alpha = F;          // 环外 Fresnel 已回到 1 ⇒ α = 1（整片替换）
    o.fresnel_debug = 0.0;
    o.env_lod_debug = 0.0;
    o.refl_debug = vec3(0.0);
}
)glsl";

// ───────────────────────── 调试 ─────────────────────────
// 只有调试 program 拼这个片段：uWaveDebug + EmitDebug。生产 program 一行调试代码都没有。
inline constexpr const char* kWaterSkinDebugGlsl = R"glsl(
uniform int uWaveDebug;          // 1 = 输出法线颜色（debug）

void EmitDebug(WaterSurface s, WaterShade o, int mode) {
    if (mode == 7) {
        // alpha：0.02~0.06 = 窗内（F 很小），1.0 = 环外 TIR。看楔形是否落在 0/1 跳变带上。
        oAccum  = vec4(vec3(o.alpha), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 8) {
        // 求交是否成功（1 = ok，0 = 退化/回退平面法线）
        oAccum  = vec4(s.wave_ok ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 9) {
        // cos_i：临界角 0.661 附近就是窗缘
        oAccum  = vec4(vec3(s.cos_i), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 6) {
        // 纯红不透明：只回答「这一像素有没有水面覆盖」。
        oAccum  = vec4(1.0, 0.0, 0.0, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 12) {
        // 探针反射色（未乘 F）：判断"硬边"是不是探针本身带的
        oAccum  = vec4(o.refl_debug, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 13) {
        // 反射率 F
        oAccum  = vec4(vec3(o.fresnel_debug), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 11) {
        // 探针 LOD（调试）：0 = 原图，1 = LOD3。看"拉糊"到底有没有生效/生效在哪。
        float v = o.env_lod_debug / 3.0;
        oAccum  = vec4(v, v, v, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 2) {
        // 像素内法线变化量（×20 便于肉眼：0.05 rad/px → 1.0）
        float v = clamp(sqrt(s.normal_var_px) * 20.0, 0.0, 1.0);
        oAccum  = vec4(v, v, v, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    // 默认（含 mode 1 / 3 / 4 / 5 / 10…）：法线可视化（N*0.5+0.5）
    oAccum  = vec4(s.N * 0.5 + 0.5, 1.0);
    oReveal = 1.0;
    oDepth  = 1.0;
}
)glsl";

// ───────────────────────── 4 个 main ─────────────────────────
inline constexpr const char* kWaterSkinAboveMainGlsl = R"glsl(
void main() {
    WaterSurface s;
    ResolveSurface(s);
    RejectOccluded(s, false);
    WaterShade o;
    ShadeAbove(s, o);
    EmitWBO(o.C, o.alpha, s.t);
}
)glsl";

inline constexpr const char* kWaterSkinBelowMainGlsl = R"glsl(
void main() {
    WaterSurface s;
    ResolveSurface(s);
    RejectOccluded(s, false);
    WaterShade o;
    ShadeBelow(s, o);
    EmitWBO(o.C, o.alpha, s.t);
}
)glsl";

inline constexpr const char* kWaterSkinAboveDebugMainGlsl = R"glsl(
void main() {
    WaterSurface s;
    ResolveSurface(s);
    RejectOccluded(s, uWaveDebug == 3);
    WaterShade o;
    ShadeAbove(s, o);
    EmitDebug(s, o, uWaveDebug);
}
)glsl";

inline constexpr const char* kWaterSkinBelowDebugMainGlsl = R"glsl(
void main() {
    WaterSurface s;
    ResolveSurface(s);
    RejectOccluded(s, uWaveDebug == 3);
    WaterShade o;
    ShadeBelow(s, o);
    EmitDebug(s, o, uWaveDebug);
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
